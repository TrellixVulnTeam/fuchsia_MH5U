// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/bin/sessionmgr/sessionmgr_impl.h"

#include <fuchsia/element/cpp/fidl.h>
#include <fuchsia/session/cpp/fidl.h>
#include <fuchsia/ui/app/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>
#include <lib/zx/eventpair.h>
#include <zircon/status.h>

#include <memory>

#include <fbl/unique_fd.h>

#include "fuchsia/intl/cpp/fidl.h"
#include "lib/fdio/directory.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/vfs/cpp/pseudo_dir.h"
#include "src/lib/files/directory.h"
#include "src/lib/fsl/io/fd.h"
#include "src/lib/fsl/types/type_converters.h"
#include "src/modular/bin/basemgr/cobalt/cobalt.h"
#include "src/modular/bin/sessionmgr/puppet_master/make_production_impl.h"
#include "src/modular/bin/sessionmgr/story_runner/story_controller_impl.h"
#include "src/modular/lib/common/teardown.h"
#include "src/modular/lib/fidl/array_to_string.h"
#include "src/modular/lib/fidl/clone.h"
#include "src/modular/lib/fidl/json_xdr.h"

namespace modular {

using cobalt_registry::ModularLifetimeEventsMetricDimensionEventType;

namespace {

constexpr char kSessionEnvironmentLabelPrefix[] = "session-";

// The name in the outgoing debug directory (hub) for developer session control
// services.
constexpr char kSessionCtlDir[] = "sessionctl";

// Creates a function that can be used as termination action passed to OnTerminate(),
// which when called invokes the reset() method on the object pointed to by the
// argument. Used to reset() fidl pointers and std::unique_ptr<>s fields.
template <typename X>
fit::function<void(fit::function<void()>)> Reset(std::unique_ptr<X>* const field) {
  return [field](fit::function<void()> cont) {
    field->reset();
    cont();
  };
}

template <typename X>
fit::function<void(fit::function<void()>)> Reset(fidl::InterfacePtr<X>* const field) {
  return [field](fit::function<void()> cont) {
    field->Unbind();
    cont();
  };
}

// Creates a function that can be used as termination action passed to OnTerminate(),
// which when called asynchronously invokes the Teardown() method on the object
// pointed to by the argument. Used to teardown AppClient and AsyncHolder
// members.
template <typename X>
fit::function<void(fit::function<void()>)> Teardown(const zx::duration timeout,
                                                    const char* const message, X* const field) {
  return [timeout, message, field](fit::function<void()> cont) {
    field->Teardown(timeout, [message, cont = std::move(cont)] {
      if (message) {
        FX_DLOGS(INFO) << "- " << message << " down.";
      }
      cont();
    });
  };
}

}  // namespace

SessionmgrImpl::SessionmgrImpl(sys::ComponentContext* const component_context,
                               modular::ModularConfigAccessor config_accessor,
                               inspect::Node node_object)
    : sessionmgr_context_(component_context),
      config_accessor_(std::move(config_accessor)),
      inspect_root_node_(std::move(node_object)),
      story_provider_impl_("StoryProviderImpl"),
      agent_runner_("AgentRunner"),
      weak_ptr_factory_(this) {
  sessionmgr_context_->outgoing()->AddPublicService<fuchsia::modular::internal::Sessionmgr>(
      [this](fidl::InterfaceRequest<fuchsia::modular::internal::Sessionmgr> request) {
        bindings_.AddBinding(this, std::move(request));
      });
}

SessionmgrImpl::~SessionmgrImpl() = default;

// Initialize is called for each new session, denoted by a unique session_id. In other words, it
// initializes a session, not a SessionmgrImpl (despite the class-scoped name). (Ironically, the
// |finitish_initialization_| lambda does initialize some Sessionmgr-scoped resources only once,
// upon demand.)
void SessionmgrImpl::Initialize(
    std::string session_id,
    fidl::InterfaceHandle<fuchsia::modular::internal::SessionContext> session_context,
    fuchsia::sys::ServiceList v2_services_for_sessionmgr,
    fidl::InterfaceRequest<fuchsia::io::Directory> svc_from_v1_sessionmgr,
    fuchsia::ui::views::ViewToken view_token, fuchsia::ui::views::ViewRefControl control_ref,
    fuchsia::ui::views::ViewRef view_ref) {
  FX_LOGS(INFO) << "SessionmgrImpl::Initialize() called.";

  session_context_ = session_context.Bind();
  OnTerminate(Reset(&session_context_));

  session_storage_ = std::make_unique<SessionStorage>();
  OnTerminate(Reset(&session_storage_));

  InitializeSessionEnvironment(session_id);

  // Create |puppet_master_| before |agent_runner_| to ensure agents can use it when terminating.
  InitializePuppetMaster();
  InitializeElementManager();

  InitializeStartupAgentLauncher(std::move(v2_services_for_sessionmgr));
  InitializeAgentRunner(config_accessor_.session_shell_app_config().url());
  InitializeStartupAgents();

  scenic::ViewRefPair view_ref_pair =
      scenic::ViewRefPair{.control_ref = {std::move(control_ref.reference)},
                          .view_ref = {std::move(view_ref.reference)}};

  InitializeSessionShell(CloneStruct(config_accessor_.session_shell_app_config()),
                         std::move(view_token), std::move(view_ref_pair));

  // We create |story_provider_impl_| after |agent_runner_| so
  // story_provider_impl_ is terminated before agent_runner_, which will cause
  // all modules to be terminated before agents are terminated. Agents must
  // outlive the stories which contain modules that are connected to those
  // agents.
  InitializeStoryProvider(CloneStruct(config_accessor_.story_shell_app_config()),
                          config_accessor_.use_session_shell_for_story_shell_factory());
  ConnectSessionShellToStoryProvider();

  InitializeSessionCtl();

  ServeSvcFromV1SessionmgrDir(std::move(svc_from_v1_sessionmgr));

  ReportEvent(ModularLifetimeEventsMetricDimensionEventType::BootedToSessionMgr);
}

void SessionmgrImpl::ConnectSessionShellToStoryProvider() {
  // If connecting to the SessionShell errors out, use the GraphicalPresenter
  ui_handlers_.session_shell.set_error_handler(
      [weak_this = weak_ptr_factory_.GetWeakPtr()](zx_status_t status) mutable {
        if (!weak_this) {
          return;
        }
        FX_PLOGS(INFO, status) << "Failed to connect to SessionShell, using GraphicalPresenter";
        if (weak_this->story_provider_impl_.get() && weak_this->ui_handlers_.graphical_presenter) {
          weak_this->story_provider_impl_.get()->SetPresentationProtocol(
              PresentationProtocolPtr{std::move(weak_this->ui_handlers_.graphical_presenter)});
        }
      });

  // If connecting to the GraphicalPresenter errors out, use the SessionShell
  ui_handlers_.graphical_presenter.set_error_handler(
      [weak_this = weak_ptr_factory_.GetWeakPtr()](zx_status_t status) mutable {
        if (!weak_this) {
          return;
        }
        FX_PLOGS(INFO, status) << "Failed to connect to GraphicalPresenter, using SessionShell";
        if (weak_this->story_provider_impl_.get() && weak_this->ui_handlers_.session_shell) {
          weak_this->story_provider_impl_.get()->SetPresentationProtocol(
              PresentationProtocolPtr{std::move(weak_this->ui_handlers_.session_shell)});
        }
      });

  ConnectToSessionShellService(ui_handlers_.session_shell.NewRequest());
  ConnectToSessionShellService(ui_handlers_.graphical_presenter.NewRequest());
}

// Create an environment in which to launch story shells and mods. Note that agents cannot be
// launched from this environment because the environment hosts its data directories in a
// session-specific subdirectory of data, and certain agents in existing test devices expect the
// data at a hardcoded, top-level /data directory.
//
// True separation among multiple sessions is currently NOT supported for many reasons, so as
// a temporary workaround, agents are started in the /sys realm via a different launcher.
//
// Future implementations will use the new SessionFramework, which will provide support for
// multiple sessions.
void SessionmgrImpl::InitializeSessionEnvironment(std::string session_id) {
  session_id_ = std::move(session_id);

  // Create the session's environment (in which we run stories, modules, agents, and so on) as a
  // child of sessionmgr's environment. Add session-provided additional services, |kEnvServices|.
  static const auto* const kEnvServices =
      new std::vector<std::string>{fuchsia::intl::PropertyProvider::Name_};

  session_environment_ = std::make_unique<Environment>(
      /* parent_env = */ sessionmgr_context_->svc()->Connect<fuchsia::sys::Environment>(),
      std::string(kSessionEnvironmentLabelPrefix) + session_id_, *kEnvServices,
      /* kill_on_oom = */ true);

  // Get the default launcher from the new |session_environment_| to wrap in an
  // |ArgvInjectingLauncher|
  fuchsia::sys::LauncherPtr session_environment_launcher;
  session_environment_->environment()->GetLauncher(session_environment_launcher.NewRequest());

  // Wrap the launcher and override it with the new |ArgvInjectingLauncher|
  ArgvInjectingLauncher::ArgvMap argv_map;
  for (auto& component : config_accessor_.sessionmgr_config().component_args()) {
    argv_map.insert(std::make_pair(component.url(), component.args()));
  }
  session_environment_->OverrideLauncher(
      std::make_unique<ArgvInjectingLauncher>(std::move(session_environment_launcher), argv_map));

  // Add session-provided services.
  session_environment_->AddService<fuchsia::intl::PropertyProvider>(
      [this](fidl::InterfaceRequest<fuchsia::intl::PropertyProvider> request) {
        if (terminating_) {
          request.Close(ZX_ERR_UNAVAILABLE);
          return;
        }
        sessionmgr_context_->svc()->Connect<fuchsia::intl::PropertyProvider>(std::move(request));
      });

  OnTerminate(Reset(&session_environment_));
}

void SessionmgrImpl::InitializeStartupAgentLauncher(
    fuchsia::sys::ServiceList v2_services_for_sessionmgr) {
  FX_DCHECK(puppet_master_impl_);

  startup_agent_launcher_ = std::make_unique<StartupAgentLauncher>(
      &config_accessor_,
      [this](fidl::InterfaceRequest<fuchsia::modular::PuppetMaster> request) {
        puppet_master_impl_->Connect(std::move(request));
      },
      [this](fidl::InterfaceRequest<fuchsia::modular::SessionRestartController> request) {
        if (terminating_) {
          request.Close(ZX_ERR_UNAVAILABLE);
          return;
        }
        session_restart_controller_bindings_.AddBinding(this, std::move(request));
      },
      [this](fidl::InterfaceRequest<fuchsia::intl::PropertyProvider> request) {
        if (terminating_) {
          request.Close(ZX_ERR_UNAVAILABLE);
          return;
        }
        sessionmgr_context_->svc()->Connect<fuchsia::intl::PropertyProvider>(std::move(request));
      },
      [this](fidl::InterfaceRequest<fuchsia::element::Manager> request) {
        if (terminating_) {
          request.Close(ZX_ERR_UNAVAILABLE);
          return;
        }
        element_manager_impl_->Connect(std::move(request));
      },
      std::move(v2_services_for_sessionmgr), [this]() { return terminating_; });
  OnTerminate(Reset(&startup_agent_launcher_));
}

void SessionmgrImpl::InitializeAgentRunner(const std::string& session_shell_url) {
  FX_DCHECK(startup_agent_launcher_);

  // Initialize the AgentRunner.
  //
  // The AgentRunner must use its own |ArgvInjectingLauncher|, different from the
  // |ArgvInjectingLauncher| launcher used for mods: The AgentRunner's launcher must come from the
  // sys realm (the realm that sessionmgr is running in) due to devices in the field which rely on
  // agents /data path mappings being consistent. There is no current solution for the migration of
  // /data when a component topology changes. This will be resolved in Session Framework, which
  // will soon deprecated and replace this Modular solution.
  //
  // Create a new launcher that uses sessionmgr's realm launcher.
  std::map<std::string, std::string> agent_service_index;
  for (auto& entry : config_accessor_.sessionmgr_config().agent_service_index()) {
    agent_service_index.emplace(entry.service_name(), entry.agent_url());
  }

  ArgvInjectingLauncher::ArgvMap argv_map;
  for (auto& component : config_accessor_.sessionmgr_config().component_args()) {
    argv_map.insert(std::make_pair(component.url(), component.args()));
  }

  fuchsia::sys::LauncherPtr launcher;
  session_environment_->environment()->GetLauncher(launcher.NewRequest());
  agent_runner_launcher_ = std::make_unique<ArgvInjectingLauncher>(std::move(launcher), argv_map);

  auto restart_session_on_agent_crash =
      config_accessor_.sessionmgr_config().restart_session_on_agent_crash();
  restart_session_on_agent_crash.push_back(session_shell_url);

  agent_runner_.reset(new AgentRunner(
      &config_accessor_, agent_runner_launcher_.get(), startup_agent_launcher_.get(),
      &inspect_root_node_,
      /*on_critical_agent_crash=*/[this] { RestartDueToCriticalFailure(); },
      std::move(agent_service_index), config_accessor_.sessionmgr_config().session_agents(),
      restart_session_on_agent_crash, sessionmgr_context_));
  OnTerminate(Teardown(kAgentRunnerTimeout, "AgentRunner", &agent_runner_));
}

void SessionmgrImpl::InitializeStartupAgents() {
  FX_DCHECK(startup_agent_launcher_);
  FX_DCHECK(agent_runner_.get());

  startup_agent_launcher_->StartAgents(agent_runner_.get(),
                                       config_accessor_.sessionmgr_config().session_agents(),
                                       config_accessor_.sessionmgr_config().startup_agents());
}

void SessionmgrImpl::InitializeStoryProvider(
    fuchsia::modular::session::AppConfig story_shell_config,
    bool use_session_shell_for_story_shell_factory) {
  FX_DCHECK(agent_runner_.get());
  FX_DCHECK(session_environment_);
  FX_DCHECK(session_storage_);
  FX_DCHECK(startup_agent_launcher_);

  // The StoryShellFactory to use when creating story shells, or nullptr if no
  // such factory exists.
  fidl::InterfacePtr<fuchsia::modular::StoryShellFactory> story_shell_factory_ptr;

  if (use_session_shell_for_story_shell_factory) {
    ConnectToSessionShellService(story_shell_factory_ptr.NewRequest());
  }

  ComponentContextInfo component_context_info{
      agent_runner_.get(), config_accessor_.sessionmgr_config().session_agents()};
  story_provider_impl_.reset(new StoryProviderImpl(
      session_environment_.get(), session_storage_.get(), std::move(story_shell_config),
      std::move(story_shell_factory_ptr), component_context_info, startup_agent_launcher_.get(),
      &inspect_root_node_));
  OnTerminate(Teardown(kStoryProviderTimeout, "StoryProvider", &story_provider_impl_));
}

void SessionmgrImpl::InitializePuppetMaster() {
  FX_DCHECK(session_storage_);

  story_command_executor_ = MakeProductionStoryCommandExecutor(session_storage_.get());
  OnTerminate(Reset(&story_command_executor_));

  puppet_master_impl_ =
      std::make_unique<PuppetMasterImpl>(session_storage_.get(), story_command_executor_.get());
  OnTerminate(Reset(&puppet_master_impl_));
}

void SessionmgrImpl::InitializeElementManager() {
  FX_DCHECK(session_storage_);

  element_manager_impl_ = std::make_unique<ElementManagerImpl>(session_storage_.get());
  OnTerminate(Reset(&element_manager_impl_));
}

void SessionmgrImpl::InitializeSessionCtl() {
  FX_DCHECK(puppet_master_impl_);

  session_ctl_ = std::make_unique<SessionCtl>(sessionmgr_context_->outgoing()->debug_dir(),
                                              kSessionCtlDir, puppet_master_impl_.get());
  OnTerminate(Reset(&session_ctl_));
}

void SessionmgrImpl::ServeSvcFromV1SessionmgrDir(
    fidl::InterfaceRequest<fuchsia::io::Directory> svc_from_v1_sessionmgr) {
  zx_status_t status = svc_from_v1_sessionmgr_dir_.Serve(fuchsia::io::OPEN_RIGHT_READABLE |
                                                             fuchsia::io::OPEN_RIGHT_WRITABLE |
                                                             fuchsia::io::OPEN_FLAG_DIRECTORY,
                                                         svc_from_v1_sessionmgr.TakeChannel());
  if (status != ZX_OK) {
    FX_PLOGS(FATAL, status) << "Failed to serve the svc_from_v1_sessionmgr_dir";
  }
}

void SessionmgrImpl::InitializeSessionShell(
    fuchsia::modular::session::AppConfig session_shell_config,
    fuchsia::ui::views::ViewToken view_token, scenic::ViewRefPair view_ref_pair) {
  FX_DCHECK(session_environment_);
  FX_DCHECK(agent_runner_.get());
  FX_DCHECK(puppet_master_impl_);

  session_shell_url_ = session_shell_config.url();

  ComponentContextInfo component_context_info{
      agent_runner_.get(), config_accessor_.sessionmgr_config().session_agents()};
  session_shell_component_context_impl_ = std::make_unique<ComponentContextImpl>(
      component_context_info, session_shell_url_, session_shell_url_);
  OnTerminate(Reset(&session_shell_component_context_impl_));

  // |service_list| enumerates which services are made available to the session
  // shell.
  auto service_list = std::make_unique<fuchsia::sys::ServiceList>();
  for (const auto& service_name : agent_runner_->GetAgentServices()) {
    service_list->names.push_back(service_name);
  }

  agent_runner_->PublishAgentServices(session_shell_url_, &session_shell_services_);

  service_list->names.push_back(fuchsia::modular::SessionShellContext::Name_);
  session_shell_services_.AddService<fuchsia::modular::SessionShellContext>([this](auto request) {
    if (terminating_) {
      request.Close(ZX_ERR_UNAVAILABLE);
      return;
    }
    session_shell_context_bindings_.AddBinding(this, std::move(request));
  });

  service_list->names.push_back(fuchsia::modular::ComponentContext::Name_);
  session_shell_services_.AddService<fuchsia::modular::ComponentContext>([this](auto request) {
    if (terminating_) {
      request.Close(ZX_ERR_UNAVAILABLE);
      return;
    }
    session_shell_component_context_impl_->Connect(std::move(request));
  });

  service_list->names.push_back(fuchsia::modular::PuppetMaster::Name_);
  session_shell_services_.AddService<fuchsia::modular::PuppetMaster>([this](auto request) {
    if (terminating_) {
      request.Close(ZX_ERR_UNAVAILABLE);
      return;
    }
    puppet_master_impl_->Connect(std::move(request));
  });

  service_list->names.push_back(fuchsia::element::Manager::Name_);
  session_shell_services_.AddService<fuchsia::element::Manager>([this](auto request) {
    if (terminating_) {
      request.Close(ZX_ERR_UNAVAILABLE);
      return;
    }
    element_manager_impl_->Connect(std::move(request));
  });

  // The services in |session_shell_services_| are provided through the
  // connection held in |session_shell_service_provider| connected to
  // |session_shell_services_|.
  {
    fuchsia::sys::ServiceProviderPtr session_shell_service_provider;
    session_shell_services_.AddBinding(session_shell_service_provider.NewRequest());
    service_list->provider = std::move(session_shell_service_provider);
  }

  for (const auto& service_name : service_list->names) {
    fuchsia::sys::ServiceProviderPtr service_provider;
    session_shell_services_.AddBinding(service_provider.NewRequest());
    zx_status_t status = svc_from_v1_sessionmgr_dir_.AddEntry(
        service_name, std::make_unique<vfs::Service>(
                          [service_provider = std::move(service_provider), service_name](
                              zx::channel request, async_dispatcher_t* dispatcher) {
                            service_provider->ConnectToService(service_name, std::move(request));
                          }));

    if (status != ZX_OK) {
      FX_PLOGS(WARNING, status)
          << "Could not add service_list handler to svc_from_v1_sessionmgr, for service name: "
          << service_name;
    }
  }

  auto session_shell_app = std::make_unique<AppClient<fuchsia::modular::Lifecycle>>(
      session_environment_->GetLauncher(), std::move(session_shell_config),
      /* data_origin = */ "", std::move(service_list));

  fuchsia::ui::app::ViewProviderPtr view_provider;
  session_shell_app->services().ConnectToService(view_provider.NewRequest());
  view_provider->CreateViewWithViewRef(std::move(view_token.value),
                                       std::move(view_ref_pair.control_ref),
                                       std::move(view_ref_pair.view_ref));

  agent_runner_->AddRunningAgent(session_shell_url_, std::move(session_shell_app));
}

void SessionmgrImpl::Terminate(fit::function<void()> done) {
  FX_LOGS(INFO) << "Sessionmgr::Terminate()";
  terminating_ = true;
  terminate_done_ = std::move(done);

  TerminateRecurse(static_cast<int>(on_terminate_cbs_.size()) - 1);
}

void SessionmgrImpl::GetComponentContext(
    fidl::InterfaceRequest<fuchsia::modular::ComponentContext> request) {
  session_shell_component_context_impl_->Connect(std::move(request));
}

void SessionmgrImpl::GetPresentation(
    fidl::InterfaceRequest<fuchsia::ui::policy::Presentation> request) {
  session_context_->GetPresentation(std::move(request));
}

void SessionmgrImpl::GetStoryProvider(
    fidl::InterfaceRequest<fuchsia::modular::StoryProvider> request) {
  story_provider_impl_->Connect(std::move(request));
}

void SessionmgrImpl::Logout() { Restart(); }

void SessionmgrImpl::Restart() { session_context_->Restart(); }

void SessionmgrImpl::RestartDueToCriticalFailure() {
  session_context_->RestartDueToCriticalFailure();
}

void SessionmgrImpl::OnTerminate(fit::function<void(fit::function<void()>)> action) {
  on_terminate_cbs_.emplace_back(std::move(action));
}

void SessionmgrImpl::TerminateRecurse(const int i) {
  if (i >= 0) {
    on_terminate_cbs_[i]([this, i] { TerminateRecurse(i - 1); });
  } else {
    FX_LOGS(INFO) << "Sessionmgr::Terminate(): done";
    terminate_done_();
  }
}

}  // namespace modular
