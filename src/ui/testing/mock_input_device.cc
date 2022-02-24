// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/testing/mock_input_device.h"

namespace ui_input {
namespace test {

MockInputDevice::MockInputDevice(
    uint32_t device_id, fuchsia::ui::input::DeviceDescriptor descriptor,
    fidl::InterfaceRequest<fuchsia::ui::input::InputDevice> input_device_request,
    OnReportCallback on_report_callback)
    : id_(device_id),
      descriptor_(std::move(descriptor)),
      input_device_binding_(this, std::move(input_device_request)),
      on_report_callback_(std::move(on_report_callback)) {}

MockInputDevice::~MockInputDevice() {}

void MockInputDevice::DispatchReport(fuchsia::ui::input::InputReport report) {
  if (on_report_callback_)
    on_report_callback_(std::move(report));
}

}  // namespace test
}  // namespace ui_input
