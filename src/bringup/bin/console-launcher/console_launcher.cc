// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/bringup/bin/console-launcher/console_launcher.h"

#include <fcntl.h>
#include <fidl/fuchsia.hardware.virtioconsole/cpp/wire.h>
#include <fuchsia/boot/cpp/fidl.h>
#include <fuchsia/kernel/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/namespace.h>
#include <lib/fdio/spawn.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>
#include <lib/fit/defer.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/syslog/global.h>
#include <lib/zircon-internal/paths.h>
#include <zircon/compiler.h>

#include <fbl/algorithm.h>

namespace console_launcher {

namespace {

// Get the root job from the root job service.
zx_status_t GetRootJob(zx::job* root_job) {
  auto svc_dir = sys::ServiceDirectory::CreateFromNamespace();
  fuchsia::kernel::RootJobSyncPtr root_job_ptr;
  zx_status_t status = svc_dir->Connect(root_job_ptr.NewRequest());
  if (status != ZX_OK) {
    return status;
  }
  return root_job_ptr->Get(root_job);
}

// Wait for the requested file.  Its parent directory must exist.
zx_status_t WaitForFile(const char* path, zx::time deadline) {
  char path_copy[PATH_MAX];
  if (strlen(path) >= PATH_MAX) {
    return ZX_ERR_INVALID_ARGS;
  }
  strcpy(path_copy, path);

  char* last_slash = strrchr(path_copy, '/');
  // Waiting on the root of the fs or paths with no slashes is not supported by this function
  if (last_slash == path_copy || last_slash == nullptr) {
    return ZX_ERR_NOT_SUPPORTED;
  }
  last_slash[0] = 0;
  char* dirname = path_copy;
  char* basename = last_slash + 1;

  auto watch_func = [](int dirfd, int event, const char* fn, void* cookie) -> zx_status_t {
    auto basename = static_cast<const char*>(cookie);
    if (event != WATCH_EVENT_ADD_FILE) {
      return ZX_OK;
    }
    if (!strcmp(fn, basename)) {
      return ZX_ERR_STOP;
    }
    return ZX_OK;
  };

  fbl::unique_fd dirfd(open(dirname, O_RDONLY));
  if (!dirfd.is_valid()) {
    return ZX_ERR_INVALID_ARGS;
  }
  zx_status_t status = fdio_watch_directory(dirfd.get(), watch_func, deadline.get(),
                                            reinterpret_cast<void*>(basename));
  if (status == ZX_ERR_STOP) {
    return ZX_OK;
  }
  return status;
}

}  // namespace

zx::status<ConsoleLauncher> ConsoleLauncher::Create() {
  ConsoleLauncher launcher;

  zx::job root_job;
  // TODO(fxbug.dev/33957): Remove all uses of the root job.
  zx_status_t status = GetRootJob(&root_job);
  if (status != ZX_OK) {
    FX_LOGF(ERROR, nullptr, "Failed to get root job: %s", zx_status_get_string(status));
    return zx::error(status);
  }
  status = zx::job::create(root_job, 0u, &launcher.shell_job_);
  if (status != ZX_OK) {
    FX_LOGF(ERROR, nullptr, "Failed to create shell_job: %s", zx_status_get_string(status));
    return zx::error(status);
  }
  status = launcher.shell_job_.set_property(ZX_PROP_NAME, "zircon-shell", 16);
  if (status != ZX_OK) {
    FX_LOGF(ERROR, nullptr, "Failed to set shell_job job name: %s", zx_status_get_string(status));
    return zx::error(status);
  }

  return zx::ok(std::move(launcher));
}

std::optional<Arguments> GetArguments(const fidl::WireSyncClient<fuchsia_boot::Arguments>& client) {
  Arguments ret;

  fuchsia_boot::wire::BoolPair bool_keys[]{
      {fidl::StringView{"console.shell"}, false},
      {fidl::StringView{"kernel.shell"}, false},
      {fidl::StringView{"console.is_virtio"}, false},
      {fidl::StringView{"devmgr.log-to-debuglog"}, false},
  };
  auto bool_resp =
      client->GetBools(fidl::VectorView<fuchsia_boot::wire::BoolPair>::FromExternal(bool_keys));
  if (!bool_resp.ok()) {
    printf("console-launcher: failed to get boot bools\n");
    return std::nullopt;
  }

  ret.run_shell = bool_resp->values[0];
  // If the kernel console is running a shell we can't launch our own shell.
  ret.run_shell = ret.run_shell & !bool_resp->values[1];
  ret.is_virtio = bool_resp->values[2];
  ret.log_to_debuglog = bool_resp->values[3];

  fidl::StringView vars[]{
      fidl::StringView{"TERM"},
      fidl::StringView{"console.path"},
      fidl::StringView{"zircon.autorun.boot"},
      fidl::StringView{"zircon.autorun.system"},
  };
  auto resp = client->GetStrings(fidl::VectorView<fidl::StringView>::FromExternal(vars));
  if (!resp.ok()) {
    printf("console-launcher: failed to get console path\n");
    return std::nullopt;
  }

  if (resp->values[0].is_null()) {
    ret.term += "uart";
  } else {
    ret.term += std::string{resp->values[0].data(), resp->values[0].size()};
  }
  if (!resp->values[1].is_null()) {
    ret.device = std::string{resp->values[1].data(), resp->values[1].size()};
  }
  if (!resp->values[2].is_null()) {
    ret.autorun_boot = std::string{resp->values[2].data(), resp->values[2].size()};
  }
  if (!resp->values[3].is_null()) {
    ret.autorun_system = std::string{resp->values[3].data(), resp->values[3].size()};
  }

  return ret;
}

std::optional<fbl::unique_fd> ConsoleLauncher::GetVirtioFd(const Arguments& args,
                                                           fbl::unique_fd device_fd) {
  zx::channel virtio_channel;
  zx_status_t status =
      fdio_get_service_handle(device_fd.release(), virtio_channel.reset_and_get_address());
  if (status != ZX_OK) {
    printf("console-launcher: failed to get console handle '%s'\n", args.device.data());
    return std::nullopt;
  }

  zx::channel local, remote;
  status = zx::channel::create(0, &local, &remote);
  if (status != ZX_OK) {
    printf("console-launcher: failed to create channel for console '%s'\n", args.device.data());
    return std::nullopt;
  }

  fidl::WireSyncClient<fuchsia_hardware_virtioconsole::Device> virtio_client(
      std::move(virtio_channel));
  virtio_client->GetChannel(std::move(remote));

  fdio_t* fdio;
  status = fdio_create(local.release(), &fdio);
  if (status != ZX_OK) {
    printf("console-launcher: failed to setup fdio for console '%s'\n", args.device.data());
    return std::nullopt;
  }

  fbl::unique_fd fd(fdio_bind_to_fd(fdio, -1, 3));
  if (!fd.is_valid()) {
    fdio_unsafe_release(fdio);
    printf("console-launcher: failed to transfer fdio for console '%s'\n", args.device.data());
    return std::nullopt;
  }

  return fd;
}

zx_status_t ConsoleLauncher::LaunchShell(const Arguments& args) {
  if (!args.run_shell) {
    FX_LOGS(INFO) << "console-launcher: disabled";
    return ZX_OK;
  }

  zx_status_t status = WaitForFile(args.device.data(), zx::time::infinite());
  if (status != ZX_OK) {
    FX_LOGS(INFO) << "console-launcher: failed to wait for console";
    printf("console-launcher: failed to wait for console '%s' (%s)\n", args.device.data(),
           zx_status_get_string(status));
    return status;
  }

  fbl::unique_fd fd(open(args.device.data(), O_RDWR));
  if (!fd.is_valid()) {
    printf("console-launcher: failed to open console '%s'\n", args.device.data());
    return ZX_ERR_INVALID_ARGS;
  }

  // TODO(fxbug.dev/33183): Clean this up once devhost stops speaking fuchsia.io.File
  // on behalf of drivers.  Once that happens, the virtio-console driver
  // should just speak that instead of this shim interface.
  if (args.is_virtio) {
    std::optional<fbl::unique_fd> result = GetVirtioFd(args, std::move(fd));
    if (!result) {
      return ZX_ERR_INTERNAL;
    }
    fd = std::move(*result);
  }

  const char* argv[] = {ZX_SHELL_DEFAULT, nullptr};
  const char* environ[] = {args.term.data(), nullptr};

  std::vector<fdio_spawn_action_t> actions;
  // Add an action to set the new process name.
  {
    fdio_spawn_action_t name = {};
    name.action = FDIO_SPAWN_ACTION_SET_NAME;
    name.name.data = "sh:console";
    actions.push_back(std::move(name));
  }

  // Get our current namespace so we can pass it to the shell process.
  fdio_flat_namespace_t* flat = nullptr;
  status = fdio_ns_export_root(&flat);
  if (status != ZX_OK) {
    return status;
  }
  auto free_flat = fit::defer([&flat]() { fdio_ns_free_flat_ns(flat); });

  // Go through each directory in our namespace and copy all of them except /system-delayed.
  for (size_t i = 0; i < flat->count; i++) {
    if (strcmp(flat->path[i], "/system-delayed") == 0) {
      continue;
    }
    fdio_spawn_action_t add_dir = {};
    add_dir.action = FDIO_SPAWN_ACTION_ADD_NS_ENTRY;
    add_dir.ns.handle = flat->handle[i];
    add_dir.ns.prefix = flat->path[i];
    actions.push_back(std::move(add_dir));
  }

  // Add an action to transfer the STDIO handle.
  {
    fdio_spawn_action_t stdio = {};
    stdio.action = FDIO_SPAWN_ACTION_TRANSFER_FD;
    stdio.fd = {.local_fd = fd.release(), .target_fd = FDIO_FLAG_USE_FOR_STDIO};
    actions.push_back(std::move(stdio));
  }

  uint32_t flags = FDIO_SPAWN_CLONE_ALL & ~FDIO_SPAWN_CLONE_STDIO & ~FDIO_SPAWN_CLONE_NAMESPACE;

  FX_LOGF(INFO, nullptr, "Launching %s (%s)\n", argv[0], actions[0].name.data);
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  status = fdio_spawn_etc(shell_job_.get(), flags, argv[0], argv, environ, actions.size(),
                          actions.data(), shell_process_.reset_and_get_address(), err_msg);
  if (status != ZX_OK) {
    printf("console-launcher: failed to launch console shell: %s: %d (%s)\n", err_msg, status,
           zx_status_get_string(status));
    return status;
  }
  return ZX_OK;
}

zx_status_t ConsoleLauncher::WaitForShellExit() {
  zx_status_t status =
      shell_process_.wait_one(ZX_PROCESS_TERMINATED, zx::time::infinite(), nullptr);
  if (status != ZX_OK) {
    printf("console-launcher: failed to wait for console shell termination (%s)\n",
           zx_status_get_string(status));
    return status;
  }
  zx_info_process_t proc_info;
  status =
      shell_process_.get_info(ZX_INFO_PROCESS, &proc_info, sizeof(proc_info), nullptr, nullptr);
  if (status != ZX_OK) {
    printf("console-launcher: failed to determine console shell termination cause (%s)\n",
           zx_status_get_string(status));
    return status;
  }
  printf("console-launcher: console shell exited (started=%d exited=%d, return_code=%ld)\n",
         (proc_info.flags & ZX_INFO_PROCESS_FLAG_STARTED) != 0,
         (proc_info.flags & ZX_INFO_PROCESS_FLAG_EXITED) != 0, proc_info.return_code);
  return ZX_OK;
}

}  // namespace console_launcher
