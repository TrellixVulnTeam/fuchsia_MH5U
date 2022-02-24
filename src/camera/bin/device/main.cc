// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/camera2/hal/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/executor.h>
#include <lib/async/cpp/wait.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/log_settings.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace-provider/provider.h>

#include <string>

#include "src/camera/bin/device/device_impl.h"
#include "src/camera/bin/device/metrics_reporter.h"

int main(int argc, char* argv[]) {
  syslog::SetLogSettings({.min_log_level = CAMERA_MIN_LOG_LEVEL}, {"camera", "camera_device"});

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  async::Executor executor(loop.dispatcher());
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher());
  auto context = sys::ComponentContext::Create();

  // Verify arguments.
  if (argc != 2 || argv[1][0] == '\0') {
    FX_PLOGS(FATAL, ZX_ERR_INVALID_ARGS)
        << "Component must be initialized with outgoing service name.";
    return EXIT_FAILURE;
  }
  std::string outgoing_service_name = argv[1];

  // Connect to required environment services.
  fuchsia::camera2::hal::ControllerHandle controller;
  zx_status_t status = context->svc()->Connect(controller.NewRequest());
  if (status != ZX_OK) {
    FX_PLOGS(FATAL, status) << "Failed to request controller service.";
    return EXIT_FAILURE;
  }

  fuchsia::sysmem::AllocatorHandle allocator;
  status = context->svc()->Connect(allocator.NewRequest());
  if (status != ZX_OK) {
    FX_PLOGS(FATAL, status) << "Failed to request allocator service.";
    return EXIT_FAILURE;
  }

  fuchsia::ui::policy::DeviceListenerRegistryHandle registry;
  status = context->svc()->Connect(registry.NewRequest());
  if (status != ZX_OK) {
    FX_PLOGS(FATAL, status) << "Failed to request registry service.";
    return EXIT_FAILURE;
  }

  // Post a quit task in the event the device enters a bad state.
  zx::event event;
  FX_CHECK(zx::event::create(0, &event) == ZX_OK);
  async::Wait wait(event.get(), ZX_EVENT_SIGNALED, 0,
                   [&](async_dispatcher_t* dispatcher, async::Wait* wait, zx_status_t status,
                       const zx_packet_signal_t* signal) {
                     FX_LOGS(FATAL) << "Device signaled bad state.";
                     loop.Quit();
                   });
  ZX_ASSERT(wait.Begin(loop.dispatcher()) == ZX_OK);

  // Create our metrics reporter.
  camera::MetricsReporter::Initialize(*context, /* enable_cobalt = */ true);

  // Create the device and publish its service.
  auto result =
      camera::DeviceImpl::Create(loop.dispatcher(), executor, std::move(controller),
                                 std::move(allocator), std::move(registry), std::move(event));
  std::unique_ptr<camera::DeviceImpl> device;
  executor.schedule_task(
      result.then([&context, &device, &loop, &outgoing_service_name](
                      fpromise::result<std::unique_ptr<camera::DeviceImpl>, zx_status_t>& result) {
        if (result.is_error()) {
          FX_PLOGS(FATAL, result.error()) << "Failed to create device.";
          loop.Quit();
          return;
        }
        device = result.take_value();

        // TODO(fxbug.dev/44628): publish discoverable service name once supported
        zx_status_t status =
            context->outgoing()->AddPublicService(device->GetHandler(), outgoing_service_name);
        if (status != ZX_OK) {
          FX_PLOGS(FATAL, status) << "Failed to publish service.";
          loop.Quit();
          return;
        }
        context->outgoing()->ServeFromStartupInfo();
      }));

  loop.Run();
  return EXIT_SUCCESS;
}
