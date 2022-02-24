// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sandbox.h"

#include <fcntl.h>
#include <fuchsia/net/cpp/fidl.h>
#include <fuchsia/netemul/guest/cpp/fidl.h>
#include <fuchsia/netstack/cpp/fidl.h>
#include <fuchsia/virtualization/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/vfs.h>
#include <lib/fpromise/promise.h>
#include <lib/fpromise/result.h>
#include <lib/fpromise/sequencer.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/sys/cpp/termination_reason.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>
#include <zircon/status.h>

#include <src/lib/pkg_url/fuchsia_pkg_url.h>
#include <src/virtualization/tests/guest_console.h>
#include <src/virtualization/tests/guest_constants.h>

#include "fuchsia/virtualization/cpp/fidl.h"
#include "src/lib/cmx/cmx.h"
#include "src/lib/fxl/strings/concatenate.h"

namespace netemul {

static const char* kDebianGuestUrl = "fuchsia-pkg://fuchsia.com/debian_guest#meta/debian_guest.cmx";
static const char* kEthertapEndpointMountPath = "class/ethernet/";
static const char* kNetworkDeviceEndpointMountPath = "class/network/";
static const char* kGuestManagerUrl =
    "fuchsia-pkg://fuchsia.com/guest_manager#meta/guest_manager.cmx";
static const char* kGuestDiscoveryUrl =
    "fuchsia-pkg://fuchsia.com/guest_discovery_service#meta/"
    "guest_discovery_service.cmx";
static const char* kNetstackIntermediaryUrl =
    "fuchsia-pkg://fuchsia.com/netemul-sandbox#meta/netstack-intermediary.cmx";

#define STATIC_MSG_STRUCT(name, msgv) \
  struct name {                       \
    static const char* msg;           \
  };                                  \
  const char* name::msg = msgv;

STATIC_MSG_STRUCT(kMsgApp, "app")
STATIC_MSG_STRUCT(kMsgTest, "test")

// Sandbox uses two threads to operate:
// a main thread (which it's initialized with)
// + a helper thread.
// The macros below are used to assert that methods on
// the sandbox class are called on the proper thread
#define ASSERT_DISPATCHER(disp) ZX_ASSERT((disp) == async_get_default_dispatcher())
#define ASSERT_MAIN_DISPATCHER ASSERT_DISPATCHER(main_dispatcher_)
#define ASSERT_HELPER_DISPATCHER ASSERT_DISPATCHER(helper_loop_->dispatcher())

namespace {

// Return true if the given configuration is a Linux guest.
bool IsLinuxGuest(const config::Guest& guest) { return guest.guest_image_url() == kDebianGuestUrl; }

// Generate a virtualization GuestConfig (used to launch VMSs) from our Sandbox guest config.
fuchsia::virtualization::GuestConfig CreateGuestCfg(const config::Guest& guest) {
  fuchsia::virtualization::GuestConfig cfg;
  cfg.set_virtio_gpu(false);

  // For Linux guests, configure kernel debug serial.
  if (IsLinuxGuest(guest)) {
    for (std::string_view arg : kLinuxKernelSerialDebugCmdline) {
      cfg.mutable_cmdline_add()->emplace_back(arg);
    }
  }

  if (!guest.macs().empty()) {
    for (const auto& [mac, network] : guest.macs()) {
      fuchsia::virtualization::NetSpec out{};
      uint32_t bytes[6];
      int matched = std::sscanf(mac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &bytes[0], &bytes[1],
                                &bytes[2], &bytes[3], &bytes[4], &bytes[5]);
      FX_CHECK(matched == 6) << "Could not parse MAC address in guest config: " << mac;
      for (size_t i = 0; i != 6; ++i) {
        out.mac_address.octets[i] = static_cast<uint8_t>(bytes[i]);
      }
      out.enable_bridge = false;
      cfg.mutable_net_devices()->push_back(out);
    }

    // Prevent the guest from receiving a default MAC address from the VirtioNet
    // internals.
    cfg.set_default_net(false);
  }

  return cfg;
}

}  // namespace

Sandbox::Sandbox(SandboxArgs args) : env_config_(std::move(args.config)) {
  auto services = sys::ServiceDirectory::CreateFromNamespace();
  services->Connect(parent_env_.NewRequest());
  services->Connect(loader_.NewRequest());
  parent_env_.set_error_handler(
      [](zx_status_t err) { FX_LOGS(ERROR) << "Lost connection to parent environment"; });
}

Sandbox::~Sandbox() {
  ASSERT_MAIN_DISPATCHER;
  if (helper_loop_) {
    helper_loop_->Quit();
    helper_loop_->JoinThreads();
    // Remove all pending process handlers before shutting
    // down the loop to prevent error callbacks from
    // being fired.
    procs_.clear();
    helper_loop_ = nullptr;
  }
}

void Sandbox::Start(async_dispatcher_t* dispatcher) {
  main_dispatcher_ = dispatcher;
  setup_done_ = false;
  test_spawned_ = false;

  if (!parent_env_ || !loader_) {
    Terminate(SandboxResult::Status::INTERNAL_ERROR, "Missing parent environment or loader");
    return;
  }
  if (env_config_.disabled()) {
    Terminate(SandboxResult::Status::SUCCESS, "Test is disabled");
    return;
  }

  helper_loop_ = std::make_unique<async::Loop>(&kAsyncLoopConfigNoAttachToCurrentThread);
  if (helper_loop_->StartThread("helper-thread") != ZX_OK) {
    Terminate(SandboxResult::Status::INTERNAL_ERROR, "Can't start config thread");
    return;
  }
  helper_executor_ = std::make_unique<async::Executor>(helper_loop_->dispatcher());

  SandboxEnv::Events global_events;
  global_events.service_terminated = [this](const std::string& service, int64_t exit_code,
                                            TerminationReason reason) {
    if (helper_loop_ && (reason != TerminationReason::EXITED || exit_code != 0)) {
      async::PostTask(helper_loop_->dispatcher(), [this, service]() {
        std::stringstream ss;
        ss << service << " terminated prematurely";
        PostTerminate(SandboxResult::Status::SERVICE_EXITED, ss.str());
      });
    }
  };

  global_events.devfs_terminated = [this]() {
    if (helper_loop_) {
      async::PostTask(helper_loop_->dispatcher(), [this]() {
        PostTerminate(SandboxResult::Status::INTERNAL_ERROR,
                      "Isolated devmgr terminated prematurely");
      });
    }
  };
  global_events.network_tun_terminated = [this]() {
    if (helper_loop_) {
      async::PostTask(helper_loop_->dispatcher(), [this]() {
        PostTerminate(SandboxResult::Status::INTERNAL_ERROR, "network-tun terminated prematurely");
      });
    }
  };

  sandbox_env_ = std::make_shared<SandboxEnv>(sys::ServiceDirectory::CreateFromNamespace(),
                                              std::move(global_events));
  sandbox_env_->set_default_name(env_config_.default_url());
  sandbox_env_->set_devfs_enabled(true);

  if (services_created_callback_) {
    services_created_callback_();
  }

  StartEnvironments();
}

void Sandbox::Terminate(SandboxResult result) {
  // all processes must have been emptied to call callback
  ASSERT_MAIN_DISPATCHER;
  ZX_ASSERT(procs_.empty());

  if (helper_loop_) {
    helper_loop_->Quit();
    helper_loop_->JoinThreads();
    helper_loop_ = nullptr;
  }

  if (!result.is_success() || env_config_.capture() == config::CaptureMode::ALWAYS) {
    // check if any of the network dumps have data, and just dump them to
    // stdout:
    if (net_dumps_ && net_dumps_->HasData()) {
      std::cout << "PCAP dump for all network data ===================" << std::endl;
      net_dumps_->dump().DumpHex(&std::cout);
      std::cout << "================================================" << std::endl;
    }
  }

  if (termination_callback_) {
    termination_callback_(std::move(result));
  }
}

void Sandbox::Terminate(netemul::SandboxResult::Status status, std::string description) {
  Terminate(SandboxResult(status, std::move(description)));
}

void Sandbox::PostTerminate(SandboxResult result) {
  ASSERT_HELPER_DISPATCHER;
  // kill all component controllers before posting termination
  procs_.clear();
  async::PostTask(main_dispatcher_,
                  [this, result = std::move(result)]() mutable { Terminate(std::move(result)); });
}

void Sandbox::PostTerminate(netemul::SandboxResult::Status status, std::string description) {
  PostTerminate(SandboxResult(status, std::move(description)));
}

Sandbox::Promise Sandbox::RunRootConfiguration(ManagedEnvironment::Options root_options) {
  fpromise::bridge<void, SandboxResult> bridge;
  async::PostTask(main_dispatcher_, [this, completer = std::move(bridge.completer),
                                     root_options = std::move(root_options)]() mutable {
    ASSERT_MAIN_DISPATCHER;
    root_ = ManagedEnvironment::CreateRoot(parent_env_, sandbox_env_, std::move(root_options));
    root_->SetRunningCallback([this, completer = std::move(completer)]() mutable {
      if (root_environment_created_callback_) {
        root_environment_created_callback_(root_.get());
      }
      completer.complete_ok();
    });
  });

  return bridge.consumer.promise().and_then([this]() { return ConfigureRootEnvironment(); });
}

Sandbox::Promise Sandbox::RunGuestConfiguration(ManagedEnvironment::Options guest_options) {
  fpromise::bridge<void, SandboxResult> bridge;
  async::PostTask(main_dispatcher_, [this, completer = std::move(bridge.completer),
                                     guest_options = std::move(guest_options)]() mutable {
    ASSERT_MAIN_DISPATCHER;
    guest_ = ManagedEnvironment::CreateRoot(parent_env_, sandbox_env_, std::move(guest_options));
    sandbox_env_->guest_env_ = guest_;
    guest_->SetRunningCallback(
        [completer = std::move(completer)]() mutable { completer.complete_ok(); });
  });

  return bridge.consumer.promise().and_then([this]() { return ConfigureGuestEnvironment(); });
}

void Sandbox::StartEnvironments() {
  ASSERT_MAIN_DISPATCHER;

  async::PostTask(helper_loop_->dispatcher(), [this]() {
    if (!ConfigureNetworks()) {
      PostTerminate(SandboxResult(SandboxResult::Status::NETWORK_CONFIG_FAILED));
      return;
    }

    ManagedEnvironment::Options root_options;
    if (!CreateEnvironmentOptions(env_config_.environment(), &root_options)) {
      PostTerminate(SandboxResult::Status::ENVIRONMENT_CONFIG_FAILED,
                    "Root environment can't load options");
      return;
    }

    ManagedEnvironment::Options guest_options;
    if (!CreateGuestOptions(env_config_.guests(), &guest_options)) {
      PostTerminate(SandboxResult::Status::ENVIRONMENT_CONFIG_FAILED, "Invalid guest config");
      return;
    }

    if (env_config_.guests().empty()) {
      fpromise::schedule_for_consumer(
          helper_executor_.get(),
          RunRootConfiguration(std::move(root_options)).or_else([this](SandboxResult& result) {
            PostTerminate(std::move(result));
          }));
    } else {
      fpromise::schedule_for_consumer(
          helper_executor_.get(),
          RunGuestConfiguration(std::move(guest_options))
              .and_then([this, root_options = std::move(root_options)]() mutable {
                return RunRootConfiguration(std::move(root_options));
              })
              .or_else([this](SandboxResult& result) { PostTerminate(std::move(result)); }));
    }
  });
}

// configure networks runs in an auxiliary thread, so we can use
// synchronous calls to the fidl service
bool Sandbox::ConfigureNetworks() {
  ASSERT_HELPER_DISPATCHER;
  // start by configuring the networks:

  if (env_config_.networks().empty()) {
    return true;
  }

  fuchsia::netemul::network::NetworkContextSyncPtr net_ctx;

  auto req = net_ctx.NewRequest();

  // bind to network context
  async::PostTask(main_dispatcher_, [req = std::move(req), this]() mutable {
    sandbox_env_->network_context().GetHandler()(std::move(req));
  });

  fuchsia::netemul::network::NetworkManagerSyncPtr net_manager;
  fuchsia::netemul::network::EndpointManagerSyncPtr endp_manager;
  net_ctx->GetNetworkManager(net_manager.NewRequest());
  net_ctx->GetEndpointManager(endp_manager.NewRequest());

  for (const auto& net_cfg : env_config_.networks()) {
    zx_status_t status;
    fidl::InterfaceHandle<fuchsia::netemul::network::Network> network_h;
    if (net_manager->CreateNetwork(net_cfg.name(), fuchsia::netemul::network::NetworkConfig(),
                                   &status, &network_h) != ZX_OK ||
        status != ZX_OK) {
      FX_LOGS(ERROR) << "Create network failed";
      return false;
    }

    auto network = network_h.BindSync();

    if (env_config_.capture() != config::CaptureMode::NONE) {
      if (!net_dumps_) {
        net_dumps_ = std::make_unique<NetWatcher<InMemoryDump>>();
      }
      fidl::InterfacePtr<fuchsia::netemul::network::FakeEndpoint> fake_endpoint;
      network->CreateFakeEndpoint(fake_endpoint.NewRequest());
      net_dumps_->Watch(net_cfg.name(), std::move(fake_endpoint));
    }

    for (const auto& endp_cfg : net_cfg.endpoints()) {
      fuchsia::netemul::network::EndpointConfig fidl_config;
      fidl::InterfaceHandle<fuchsia::netemul::network::Endpoint> endp_h;

      fidl_config.backing = endp_cfg.backing();
      fidl_config.mtu = endp_cfg.mtu();
      if (endp_cfg.mac()) {
        fidl_config.mac = std::make_unique<fuchsia::net::MacAddress>();
        endp_cfg.mac()->Clone(fidl_config.mac.get());
      }

      if (endp_manager->CreateEndpoint(endp_cfg.name(), std::move(fidl_config), &status, &endp_h) !=
              ZX_OK ||
          status != ZX_OK) {
        FX_LOGS(ERROR) << "Create endpoint failed";
        return false;
      }

      auto endp = endp_h.BindSync();

      if (endp_cfg.up()) {
        if (endp->SetLinkUp(true) != ZX_OK) {
          FX_LOGS(ERROR) << "Set endpoint up failed";
          return false;
        }
      }

      // add endpoint to network:
      if (network->AttachEndpoint(endp_cfg.name(), &status) != ZX_OK || status != ZX_OK) {
        FX_LOGS(ERROR) << "Attaching endpoint " << endp_cfg.name() << " to network "
                       << net_cfg.name() << " failed";
        return false;
      }

      // save the endpoint handle:
      network_handles_.emplace_back(endp.Unbind().TakeChannel());
    }

    // save the network handle:
    network_handles_.emplace_back(network.Unbind().TakeChannel());
  }

  return true;
}

// Create environment options runs in an auxiliary thread, so we can use
// synchronous calls to fidl services
bool Sandbox::CreateEnvironmentOptions(const config::Environment& config,
                                       ManagedEnvironment::Options* options) {
  ASSERT_HELPER_DISPATCHER;
  options->set_name(config.name());
  options->set_inherit_parent_launch_services(config.inherit_services());

  std::vector<fuchsia::netemul::environment::VirtualDevice>* devices = options->mutable_devices();
  if (!config.devices().empty()) {
    fuchsia::netemul::network::EndpointManagerSyncPtr epm;
    async::PostTask(main_dispatcher_, [req = epm.NewRequest(), this]() mutable {
      sandbox_env_->network_context().endpoint_manager().Bind(std::move(req));
    });
    for (const auto& device : config.devices()) {
      auto& nd = devices->emplace_back();

      fidl::InterfaceHandle<fuchsia::netemul::network::Endpoint> endp_h;
      auto status = epm->GetEndpoint(device, &endp_h);
      if (status != ZX_OK || !endp_h.is_valid()) {
        FX_LOGS(ERROR) << "Can't find endpoint " << device << " on endpoint manager";
        return false;
      }

      auto endp = endp_h.BindSync();
      if (endp->GetProxy(nd.device.NewRequest()) != ZX_OK) {
        FX_LOGS(ERROR) << "Can't get proxy on endpoint " << device;
        return false;
      }
      fuchsia::netemul::network::EndpointConfig ep_config;
      if (endp->GetConfig(&ep_config) != ZX_OK) {
        FX_LOGS(ERROR) << "Can't get endpoint configuration " << device;
      }
      std::string_view base_path(ep_config.backing ==
                                         fuchsia::netemul::network::EndpointBacking::ETHERTAP
                                     ? kEthertapEndpointMountPath
                                     : kNetworkDeviceEndpointMountPath);
      nd.path = fxl::Concatenate({base_path, device});
    }
  }

  std::vector<fuchsia::netemul::environment::LaunchService>* services = options->mutable_services();
  for (const auto& svc : config.services()) {
    auto& ns = services->emplace_back();
    ns.name = svc.name();
    ns.url = svc.launch().GetUrlOrDefault(sandbox_env_->default_name());
    ns.arguments = svc.launch().arguments();
  }

  // Logger options
  fuchsia::netemul::environment::LoggerOptions* logger_options = options->mutable_logger_options();
  const config::LoggerOptions& config_logger_options = config.logger_options();
  logger_options->set_enabled(config_logger_options.enabled());
  logger_options->set_klogs_enabled(config_logger_options.klogs_enabled());

  fuchsia::logger::LogFilterOptions* log_filter_options = logger_options->mutable_filter_options();
  const config::LoggerFilterOptions& config_logger_filter_options = config_logger_options.filters();
  log_filter_options->verbosity = config_logger_filter_options.verbosity();
  log_filter_options->tags = config_logger_filter_options.tags();

  return true;
}

bool Sandbox::CreateGuestOptions(const std::vector<config::Guest>& guests,
                                 ManagedEnvironment::Options* options) {
  if (guests.empty()) {
    return true;
  }

  fuchsia::netemul::environment::LoggerOptions* logger = options->mutable_logger_options();
  logger->set_enabled(true);
  logger->set_syslog_output(true);

  std::vector<fuchsia::netemul::environment::LaunchService>* services = options->mutable_services();
  {
    auto& ls = services->emplace_back();
    ls.name = fuchsia::virtualization::Manager::Name_;
    ls.url = kGuestManagerUrl;
  }
  {
    auto& ls = services->emplace_back();
    ls.name = fuchsia::netemul::guest::GuestDiscovery::Name_;
    ls.url = kGuestDiscoveryUrl;
  }

  std::vector<std::string> netstack_args;
  for (const config::Guest& guest : guests) {
    for (const auto& [mac, network] : guest.macs()) {
      netstack_args.push_back(fxl::Concatenate({"--interface=", mac, "=", network}));
    }
  }

  if (!netstack_args.empty()) {
    for (const std::string& name :
         {fuchsia::netstack::Netstack::Name_, fuchsia::net::virtualization::Control::Name_}) {
      auto& ls = services->emplace_back();
      ls.name = name;
      ls.url = kNetstackIntermediaryUrl;
      ls.arguments = netstack_args;
    }
  }

  return true;
}

Sandbox::Promise Sandbox::ConfigureRootEnvironment() {
  ASSERT_HELPER_DISPATCHER;
  // connect to environment:
  auto svc = std::make_shared<fuchsia::netemul::environment::ManagedEnvironmentSyncPtr>();
  auto req = svc->NewRequest();

  async::PostTask(main_dispatcher_,
                  [this, req = std::move(req)]() mutable { root_->Bind(std::move(req)); });

  return ConfigureEnvironment(svc, &env_config_.environment(), true);
}

Sandbox::Promise Sandbox::ConfigureGuestEnvironment() {
  ASSERT_HELPER_DISPATCHER;
  auto svc = std::make_shared<fuchsia::netemul::environment::ManagedEnvironmentSyncPtr>();
  auto req = svc->NewRequest();

  async::PostTask(main_dispatcher_,
                  [this, req = std::move(req)]() mutable { guest_->Bind(std::move(req)); });

  return StartGuests(svc, &env_config_);
}

Sandbox::Promise Sandbox::StartChildEnvironment(const ConfiguringEnvironmentPtr& parent,
                                                const config::Environment* config) {
  ASSERT_HELPER_DISPATCHER;

  return fpromise::make_promise(
             [this, parent,
              config]() -> fpromise::result<ConfiguringEnvironmentPtr, SandboxResult> {
               ManagedEnvironment::Options options;
               if (!CreateEnvironmentOptions(*config, &options)) {
                 return fpromise::error(
                     SandboxResult(SandboxResult::Status::ENVIRONMENT_CONFIG_FAILED));
               }
               auto child_env =
                   std::make_shared<fuchsia::netemul::environment::ManagedEnvironmentSyncPtr>();
               if ((*parent)->CreateChildEnvironment(child_env->NewRequest(), std::move(options)) !=
                   ZX_OK) {
                 return fpromise::error(
                     SandboxResult(SandboxResult::Status::ENVIRONMENT_CONFIG_FAILED));
               }

               return fpromise::ok(std::move(child_env));
             })
      .and_then([this, config](ConfiguringEnvironmentPtr& child_env) {
        return ConfigureEnvironment(child_env, config);
      });
}

static fit::closure MakeRecurringTask(async_dispatcher_t* dispatcher, fit::closure cb,
                                      zx::duration frequency) {
  return [dispatcher, cb = std::move(cb), frequency]() mutable {
    cb();
    async::PostDelayedTask(dispatcher, MakeRecurringTask(dispatcher, std::move(cb), frequency),
                           frequency);
  };
}

Sandbox::Promise Sandbox::LaunchGuestEnvironment(const ConfiguringEnvironmentPtr& env,
                                                 const config::Guest& guest) {
  ASSERT_HELPER_DISPATCHER;

  // Launch the guest.
  fpromise::bridge<void, SandboxResult> bridge;
  std::shared_ptr completer =
      std::make_shared<decltype(bridge.completer)>(std::move(bridge.completer));

  fuchsia::virtualization::GuestPtr guest_controller;
  guest_controller.set_error_handler([completer](zx_status_t status) {
    std::stringstream ss;
    ss << "Could not create guest console: " << zx_status_get_string(status);
    completer->complete_error(SandboxResult(SandboxResult::Status::SETUP_FAILED, ss.str()));
  });

  realm_->LaunchInstance(guest.guest_image_url(), guest.guest_label(), CreateGuestCfg(guest),
                         guest_controller.NewRequest(),
                         [completer](uint32_t cid) mutable { completer->complete_ok(); });

  return bridge.consumer.promise()
      .and_then([guest_controller = std::move(guest_controller),
                 this]() mutable -> fpromise::promise<zx::socket, SandboxResult> {
        fpromise::bridge<void, SandboxResult> uart_bridge;
        std::shared_ptr uart_completer =
            std::make_shared<decltype(uart_bridge.completer)>(std::move(uart_bridge.completer));

        fpromise::bridge<zx::socket, SandboxResult> console_bridge;
        std::shared_ptr console_completer = std::make_shared<decltype(console_bridge.completer)>(
            std::move(console_bridge.completer));

        guest_controller.set_error_handler([uart_completer, console_completer](zx_status_t status) {
          std::stringstream ss;
          ss << "Failed while fetching guest console and UART: " << zx_status_get_string(status);
          if (uart_completer) {
            uart_completer->complete_error(
                SandboxResult(SandboxResult::Status::SETUP_FAILED, ss.str()));
          }
          if (console_completer) {
            console_completer->complete_error(
                SandboxResult(SandboxResult::Status::SETUP_FAILED, ss.str()));
          }
        });

        // Fetch the guest's console.
        guest_controller->GetConsole(
            [console_completer](fuchsia::virtualization::Guest_GetConsole_Result result) mutable {
              if (result.is_err()) {
                std::stringstream ss;
                ss << "Could not get guest console socket: " << zx_status_get_string(result.err());
                console_completer->complete_error(
                    SandboxResult(SandboxResult::Status::SETUP_FAILED, ss.str()));
              } else {
                console_completer->complete_ok(std::move(result.response().socket));
              }
            });

        // Fetch the guest's UART, and start logging it.
        guest_controller->GetSerial(
            [uart_completer, this](fuchsia::virtualization::Guest_GetSerial_Result result) mutable {
              if (result.is_err()) {
                std::stringstream ss;
                ss << "Could not get guest serial socket: " << zx_status_get_string(result.err());
                uart_completer->complete_error(
                    SandboxResult(SandboxResult::Status::SETUP_FAILED, ss.str()));
              } else {
                // Start logging guest serial immediately, even if still waiting for
                // the GetConsole call to finish.
                guest_uart_.emplace(&Logger::Get(), std::move(result.response().socket));
                uart_completer->complete_ok();
              }
            });

        // Wait for both the console and serial to be fetched.
        return fpromise::join_promises(console_bridge.consumer.promise(),
                                       uart_bridge.consumer.promise())
            .then(
                [](fpromise::result<std::tuple<fpromise::result<zx::socket, netemul::SandboxResult>,
                                               fpromise::result<void, netemul::SandboxResult>>>&
                       result) -> fpromise::result<zx::socket, netemul::SandboxResult> {
                  auto& [console_result, uart_result] = result.value();
                  if (uart_result.is_error()) {
                    return fpromise::error(uart_result.error());
                  }
                  return std::move(console_result);
                })
            // Keep |guest_controller| alive; otherwise the callback won't fire.
            .inspect([guest_controller = std::move(guest_controller)](
                         const fpromise::result<zx::socket, SandboxResult>&) {});
      })
      .and_then([this, &guest](zx::socket& socket) -> PromiseResult {
        // Wait until the guest's serial console becomes stable to ensure the guest is mostly done
        // booting.
        GuestConsole serial(std::make_unique<ZxSocket>(std::move(socket)));
        {
          zx_status_t status = serial.Start(zx::time::infinite());
          if (status != ZX_OK) {
            return fpromise::error(SandboxResult(SandboxResult::Status::SETUP_FAILED,
                                                 "Could not start guest serial connection"));
          }
        }

        if (IsLinuxGuest(guest)) {
          // Wait till we know there is a pty listening for input.
          zx_status_t status = serial.RepeatCommandTillSuccess(
              "echo guest ready", "$", "guest ready", zx::time::infinite(), zx::sec(1));
          if (status != ZX_OK) {
            return fpromise::error(
                SandboxResult(SandboxResult::Status::SETUP_FAILED,
                              "Could not communicate with guest over serial connection"));
          }
          // Wait until guest_interaction_daemon is running.
          status = serial.ExecuteBlocking(
              "journalctl -f --no-tail -u guest_interaction_daemon | grep -m1 Listening", "$",
              zx::time::infinite(), nullptr);
          if (status != ZX_OK) {
            return fpromise::error(
                SandboxResult(SandboxResult::Status::SETUP_FAILED,
                              "Could not communicate with guest over serial connection"));
          }
          // Periodically log the guest state.
          MakeRecurringTask(
              main_dispatcher_,
              [serial = std::move(serial)]() mutable {
                zx_status_t status =
                    serial.ExecuteBlocking("journalctl -u guest_interaction_daemon --no-pager", "$",
                                           zx::time::infinite(), nullptr);
                if (status != ZX_OK) {
                  FX_LOGS(ERROR) << "periodic serial task failed: " << zx_status_get_string(status);
                }
              },
              zx::sec(10))();
        }

        return fpromise::ok();
      });
}

Sandbox::Promise Sandbox::SendGuestFiles(const ConfiguringEnvironmentPtr& env,
                                         const config::Guest& guest) {
  ASSERT_HELPER_DISPATCHER;

  return fpromise::make_promise([env, &guest]() {
    fuchsia::netemul::guest::GuestDiscoveryPtr gds;
    fuchsia::netemul::guest::GuestInteractionPtr gis;

    (*env)->ConnectToService(fuchsia::netemul::guest::GuestDiscovery::Name_,
                             gds.NewRequest().TakeChannel());

    gds->GetGuest(fuchsia::netemul::guest::DEFAULT_REALM, guest.guest_label(), gis.NewRequest());

    std::vector<Sandbox::Promise> transfer_promises;
    for (const auto& file_info : guest.files()) {
      fidl::InterfaceHandle<fuchsia::io::File> put_file;
      zx_status_t open_status =
          fdio_open(("/definition/" + file_info.first).c_str(), ZX_FS_RIGHT_READABLE,
                    put_file.NewRequest().TakeChannel().release());

      if (open_status != ZX_OK) {
        transfer_promises.clear();
        transfer_promises.emplace_back(fpromise::make_promise([file_info]() {
          return fpromise::error(SandboxResult(SandboxResult::Status::SETUP_FAILED,
                                               "Could not open " + file_info.first));
        }));
        break;
      }

      fpromise::bridge<void, SandboxResult> bridge;
      gis->PutFile(
          std::move(put_file), file_info.second,
          [file_info, completer = std::move(bridge.completer)](zx_status_t put_result) mutable {
            if (put_result != ZX_OK) {
              completer.complete_error(SandboxResult(SandboxResult::Status::SETUP_FAILED,
                                                     "Failed to copy " + file_info.first));
            } else {
              completer.complete_ok();
            }
          });
      transfer_promises.emplace_back(bridge.consumer.promise());
    }
    return fpromise::join_promise_vector(std::move(transfer_promises))
        .then([gis = std::move(gis)](
                  fpromise::result<std::vector<PromiseResult>>& result) -> PromiseResult {
          auto results = result.take_value();
          for (auto& r : results) {
            if (r.is_error()) {
              return r;
            }
          }
          return fpromise::ok();
        });
  });
}

Sandbox::Promise Sandbox::StartGuests(const ConfiguringEnvironmentPtr& env,
                                      const config::Config* config) {
  ASSERT_HELPER_DISPATCHER;
  if (!realm_) {
    fuchsia::virtualization::ManagerPtr guest_environment_manager;
    (*env)->ConnectToService(fuchsia::virtualization::Manager::Name_,
                             guest_environment_manager.NewRequest().TakeChannel());
    guest_environment_manager->Create(fuchsia::netemul::guest::DEFAULT_REALM, realm_.NewRequest());
  }

  std::vector<Sandbox::Promise> promises;

  for (const auto& guest : config->guests()) {
    promises.emplace_back(LaunchGuestEnvironment(env, guest).and_then(SendGuestFiles(env, guest)));
  }

  return fpromise::join_promise_vector(std::move(promises))
      .then([](fpromise::result<std::vector<PromiseResult>>& result) -> PromiseResult {
        auto results = result.take_value();
        for (auto& r : results) {
          if (r.is_error()) {
            return r;
          }
        }
        return fpromise::ok();
      });
}

Sandbox::Promise Sandbox::StartEnvironmentSetup(const config::Environment* config,
                                                ConfiguringEnvironmentLauncher launcher) {
  return fpromise::make_promise([this, config, launcher = std::move(launcher)] {
    auto prom = fpromise::make_result_promise(PromiseResult(fpromise::ok())).box();
    for (const auto& setup : config->setup()) {
      prom = prom.and_then([this, setup = &setup, launcher]() {
                   return LaunchSetup(launcher.get(),
                                      setup->GetUrlOrDefault(sandbox_env_->default_name()),
                                      setup->arguments());
                 })
                 .box();
    }
    return prom;
  });
}

Sandbox::Promise Sandbox::StartEnvironmentAppsAndTests(
    const netemul::config::Environment* config,
    netemul::Sandbox::ConfiguringEnvironmentLauncher launcher) {
  return fpromise::make_promise([this, config, launcher = std::move(launcher)]() -> PromiseResult {
    for (const auto& app : config->apps()) {
      auto& url = app.GetUrlOrDefault(sandbox_env_->default_name());
      if (!LaunchProcess<kMsgApp>(launcher.get(), url, app.arguments(), false)) {
        std::stringstream ss;
        ss << "Failed to launch app " << url;
        return fpromise::error(SandboxResult(SandboxResult::Status::INTERNAL_ERROR, ss.str()));
      }
    }

    for (const auto& test : config->test()) {
      auto& url = test.GetUrlOrDefault(sandbox_env_->default_name());
      if (!LaunchProcess<kMsgTest>(launcher.get(), url, test.arguments(), true)) {
        std::stringstream ss;
        ss << "Failed to launch test " << url;
        return fpromise::error(SandboxResult(SandboxResult::Status::INTERNAL_ERROR, ss.str()));
      }
      // save that at least one test was spawned.
      test_spawned_ = true;
    }

    return fpromise::ok();
  });
}

Sandbox::Promise Sandbox::StartEnvironmentInner(const ConfiguringEnvironmentPtr& env,
                                                const config::Environment* config) {
  ASSERT_HELPER_DISPATCHER;
  auto launcher = std::make_shared<fuchsia::sys::LauncherSyncPtr>();
  return fpromise::make_promise([launcher, env]() -> PromiseResult {
           // get launcher
           if ((*env)->GetLauncher(launcher->NewRequest()) != ZX_OK) {
             return fpromise::error(SandboxResult(SandboxResult::Status::INTERNAL_ERROR,
                                                  "Can't get environment launcher"));
           }
           return fpromise::ok();
         })
      .and_then(StartEnvironmentSetup(config, launcher))
      .and_then(StartEnvironmentAppsAndTests(config, launcher));
}

Sandbox::Promise Sandbox::ConfigureEnvironment(const ConfiguringEnvironmentPtr& env,
                                               const config::Environment* config, bool root) {
  ASSERT_HELPER_DISPATCHER;

  std::vector<Sandbox::Promise> promises;

  // iterate on children
  for (const auto& child : config->children()) {
    // start each one of the child environments
    promises.emplace_back(StartChildEnvironment(env, &child));
  }

  // start this processes inside this environment
  promises.emplace_back(StartEnvironmentInner(env, config));

  return fpromise::join_promise_vector(std::move(promises))
      .then([this, root](fpromise::result<std::vector<PromiseResult>>& result) -> PromiseResult {
        auto results = result.take_value();
        for (auto& r : results) {
          if (r.is_error()) {
            return r;
          }
        }
        if (root) {
          EnableTestObservation();
        }
        return fpromise::ok();
      });
}

template <typename T>
bool Sandbox::LaunchProcess(fuchsia::sys::LauncherSyncPtr* launcher, const std::string& url,
                            const std::vector<std::string>& arguments, bool is_test) {
  ASSERT_HELPER_DISPATCHER;

  fuchsia::sys::LaunchInfo linfo;
  linfo.url = url;
  linfo.arguments = arguments;

  auto ticket = procs_.size();
  auto& proc = procs_.emplace_back();

  if (is_test) {
    RegisterTest(ticket);
  }

  proc.set_error_handler([this, url](zx_status_t status) {
    std::stringstream ss;
    ss << "Component controller for " << url << " reported error " << zx_status_get_string(status);
    PostTerminate(SandboxResult::Status::COMPONENT_FAILURE, ss.str());
  });

  // we observe test processes return code
  proc.events().OnTerminated = [url, this, is_test, ticket](int64_t code,
                                                            TerminationReason reason) {
    FX_LOGS(INFO) << T::msg << " " << url << " terminated with (" << code
                  << ") reason: " << sys::HumanReadableTerminationReason(reason);
    // remove the error handler:
    procs_[ticket].set_error_handler(nullptr);
    if (is_test) {
      if (reason == TerminationReason::EXITED) {
        if (code != 0) {
          // test failed, early bail
          PostTerminate(SandboxResult::Status::TEST_FAILED, url);
        } else {
          // unregister test ticket
          UnregisterTest(ticket);
        }
      } else {
        std::stringstream ss;
        ss << "Test component " << url
           << " failure: " << sys::HumanReadableTerminationReason(reason);
        PostTerminate(SandboxResult::Status::COMPONENT_FAILURE, ss.str());
      }
    }
  };

  if ((*launcher)->CreateComponent(std::move(linfo), proc.NewRequest()) != ZX_OK) {
    FX_LOGS(ERROR) << "couldn't launch " << T::msg << ": " << url;
    return false;
  }

  return true;
}

Sandbox::Promise Sandbox::LaunchSetup(fuchsia::sys::LauncherSyncPtr* launcher,
                                      const std::string& url,
                                      const std::vector<std::string>& arguments) {
  ASSERT_HELPER_DISPATCHER;

  fpromise::bridge<void, SandboxResult> bridge;

  fuchsia::sys::LaunchInfo linfo;
  linfo.url = url;
  linfo.arguments = arguments;

  auto ticket = procs_.size();
  auto& proc = procs_.emplace_back();

  if ((*launcher)->CreateComponent(std::move(linfo), proc.NewRequest()) != ZX_OK) {
    std::stringstream ss;
    ss << "Failed to launch setup " << url;
    bridge.completer.complete_error(SandboxResult(SandboxResult::Status::INTERNAL_ERROR, ss.str()));
  } else {
    proc.set_error_handler([this, url](zx_status_t status) {
      std::stringstream ss;
      ss << "Component controller for " << url << " reported error "
         << zx_status_get_string(status);
      PostTerminate(SandboxResult::Status::COMPONENT_FAILURE, ss.str());
    });

    // we observe test processes return code
    proc.events().OnTerminated = [url, this, ticket, completer = std::move(bridge.completer)](
                                     int64_t code, TerminationReason reason) mutable {
      FX_LOGS(INFO) << "Setup " << url << " terminated with (" << code
                    << ") reason: " << sys::HumanReadableTerminationReason(reason);
      // remove the error handler:
      procs_[ticket].set_error_handler(nullptr);
      if (code == 0 && reason == TerminationReason::EXITED) {
        completer.complete_ok();
      } else {
        completer.complete_error(SandboxResult(SandboxResult::Status::SETUP_FAILED, url));
      }
    };
  }

  return bridge.consumer.promise();
}

void Sandbox::EnableTestObservation() {
  ASSERT_HELPER_DISPATCHER;

  setup_done_ = true;

  // if we're not observing any tests,
  // consider it a failure.
  if (!test_spawned_) {
    FX_LOGS(ERROR) << "No tests were specified";
    PostTerminate(SandboxResult::EMPTY_TEST_SET);
    return;
  }

  if (tests_.empty()) {
    // all tests finished successfully
    PostTerminate(SandboxResult::SUCCESS);
    return;
  }

  // if a timeout is specified, start counting it from now:
  if (env_config_.timeout() != zx::duration::infinite()) {
    async::PostDelayedTask(
        helper_loop_->dispatcher(),
        [this]() {
          FX_LOGS(ERROR) << "Test timed out.";
          PostTerminate(SandboxResult::TIMEOUT);
        },
        env_config_.timeout());
  }
}

void Sandbox::RegisterTest(size_t ticket) {
  ASSERT_HELPER_DISPATCHER;

  tests_.insert(ticket);
}

void Sandbox::UnregisterTest(size_t ticket) {
  ASSERT_HELPER_DISPATCHER;

  tests_.erase(ticket);
  if (setup_done_ && tests_.empty()) {
    // all tests finished successfully
    PostTerminate(SandboxResult::SUCCESS);
  }
}

bool SandboxArgs::ParseFromJSON(const rapidjson::Value& facet, json::JSONParser* json_parser) {
  if (!config.ParseFromJSON(facet, json_parser)) {
    FX_LOGS(ERROR) << "netemul facet failed to parse: " << json_parser->error_str();
    return false;
  }
  return true;
}

bool SandboxArgs::ParseFromString(const std::string& str) {
  json::JSONParser json_parser;
  auto facet = json_parser.ParseFromString(str, "fuchsia.netemul facet");
  if (json_parser.HasError()) {
    FX_LOGS(ERROR) << "netemul facet failed to parse: " << json_parser.error_str();
    return false;
  }

  return ParseFromJSON(facet, &json_parser);
}

bool SandboxArgs::ParseFromCmxFileAt(int dir, const std::string& path) {
  component::CmxMetadata cmx;
  json::JSONParser json_parser;
  if (!cmx.ParseFromFileAt(dir, path, &json_parser)) {
    FX_LOGS(ERROR) << "cmx file failed to parse: " << json_parser.error_str();
    return false;
  }

  return ParseFromJSON(cmx.GetFacet(config::Config::Facet), &json_parser);
}

std::ostream& operator<<(std::ostream& os, const SandboxResult& result) {
  switch (result.status_) {
    case SandboxResult::Status::SUCCESS:
      os << "Success";
      break;
    case SandboxResult::Status::NETWORK_CONFIG_FAILED:
      os << "Network configuration failed";
      break;
    case SandboxResult::Status::SERVICE_EXITED:
      os << "Service exited";
      break;
    case SandboxResult::Status::ENVIRONMENT_CONFIG_FAILED:
      os << "Environment configuration failed";
      break;
    case SandboxResult::Status::TEST_FAILED:
      os << "Test failed";
      break;
    case SandboxResult::Status::COMPONENT_FAILURE:
      os << "Component failure";
      break;
    case SandboxResult::Status::SETUP_FAILED:
      os << "Setup failed";
      break;
    case SandboxResult::Status::EMPTY_TEST_SET:
      os << "Test set is empty";
      break;
    case SandboxResult::Status::TIMEOUT:
      os << "Timeout";
      break;
    case SandboxResult::Status::INTERNAL_ERROR:
      os << "Internal Error";
      break;
    case SandboxResult::Status::UNSPECIFIED:
      os << "Unspecified error";
      break;
    default:
      os << "Undefined(" << static_cast<uint32_t>(result.status_) << ")";
  }
  if (!result.description_.empty()) {
    os << ": " << result.description_;
  }
  return os;
}
}  // namespace netemul
