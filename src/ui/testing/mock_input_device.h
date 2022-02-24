// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_TESTING_MOCK_INPUT_DEVICE_H_
#define SRC_UI_TESTING_MOCK_INPUT_DEVICE_H_

#include <fuchsia/ui/input/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fit/function.h>

#include "src/lib/fxl/macros.h"

namespace ui_input {
namespace test {

using OnReportCallback = fit::function<void(fuchsia::ui::input::InputReport report)>;

class MockInputDevice : public fuchsia::ui::input::InputDevice {
 public:
  MockInputDevice(uint32_t device_id, fuchsia::ui::input::DeviceDescriptor descriptor,
                  fidl::InterfaceRequest<fuchsia::ui::input::InputDevice> input_device_request,
                  OnReportCallback on_report_callback);
  ~MockInputDevice();

  uint32_t id() { return id_; }
  fuchsia::ui::input::DeviceDescriptor* descriptor() { return &descriptor_; }

  // |InputDevice|
  void DispatchReport(fuchsia::ui::input::InputReport report) override;

 private:
  uint32_t id_;
  fuchsia::ui::input::DeviceDescriptor descriptor_;
  fidl::Binding<fuchsia::ui::input::InputDevice> input_device_binding_;
  OnReportCallback on_report_callback_;

  FXL_DISALLOW_COPY_AND_ASSIGN(MockInputDevice);
};

}  // namespace test
}  // namespace ui_input

#endif  // SRC_UI_TESTING_MOCK_INPUT_DEVICE_H_
