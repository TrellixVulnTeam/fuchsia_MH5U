// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/ui/input/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/syslog/cpp/macros.h>

#include <src/lib/fostr/fidl/fuchsia/ui/input/formatting.h>

#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/lib/ui/input/device_state.h"
#include "src/lib/ui/input/input_device_impl.h"
#include "src/ui/lib/input_report_reader/input_reader.h"

namespace print_input {

class App : public fuchsia::ui::input::InputDeviceRegistry,
            public ui_input::InputDeviceImpl::Listener {
 public:
  App() : reader_(this, true) { reader_.Start(); }
  ~App() {}

  void OnDeviceDisconnected(ui_input::InputDeviceImpl* input_device) {
    FX_VLOGS(1) << "UnregisterDevice " << input_device->id();

    if (devices_.count(input_device->id()) != 0) {
      devices_[input_device->id()].second->OnUnregistered();
      devices_.erase(input_device->id());
    }
  }

  void OnReport(ui_input::InputDeviceImpl* input_device, fuchsia::ui::input::InputReport report) {
    FX_VLOGS(2) << "DispatchReport " << input_device->id() << " " << report;
    if (devices_.count(input_device->id()) == 0) {
      FX_VLOGS(1) << "DispatchReport: Unknown device " << input_device->id();
      return;
    }

    fuchsia::math::Size size;
    size.width = 100.0;
    size.height = 100.0;

    ui_input::DeviceState* state = devices_[input_device->id()].second.get();

    FX_CHECK(state);
    state->Update(std::move(report), size);
  }

 private:
  void RegisterDevice(
      fuchsia::ui::input::DeviceDescriptor descriptor,
      fidl::InterfaceRequest<fuchsia::ui::input::InputDevice> input_device_request) {
    uint32_t device_id = next_device_token_++;

    FX_VLOGS(1) << "RegisterDevice " << descriptor << " -> " << device_id;

    FX_CHECK(devices_.count(device_id) == 0);

    std::unique_ptr<ui_input::InputDeviceImpl> input_device =
        std::make_unique<ui_input::InputDeviceImpl>(device_id, std::move(descriptor),
                                                    std::move(input_device_request), this);

    std::unique_ptr<ui_input::DeviceState> state = std::make_unique<ui_input::DeviceState>(
        input_device->id(), input_device->descriptor(),
        ui_input::OnEventCallback(
            [this](fuchsia::ui::input::InputEvent event) { OnEvent(std::move(event)); }));
    ui_input::DeviceState* state_ptr = state.get();
    auto device_pair = std::make_pair(std::move(input_device), std::move(state));
    devices_.emplace(device_id, std::move(device_pair));
    state_ptr->OnRegistered();
  }

  void OnEvent(fuchsia::ui::input::InputEvent event) { FX_LOGS(INFO) << event; }

  uint32_t next_device_token_ = 0;
  ui_input::InputReader reader_;
  std::unordered_map<uint32_t, std::pair<std::unique_ptr<ui_input::InputDeviceImpl>,
                                         std::unique_ptr<ui_input::DeviceState>>>
      devices_;

  FXL_DISALLOW_COPY_AND_ASSIGN(App);
};

}  // namespace print_input

int main(int argc, char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line))
    return 1;

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  print_input::App app;
  loop.Run();
  return 0;
}
