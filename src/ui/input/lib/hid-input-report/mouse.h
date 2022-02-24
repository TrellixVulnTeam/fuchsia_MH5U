// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_INPUT_LIB_HID_INPUT_REPORT_MOUSE_H_
#define SRC_UI_INPUT_LIB_HID_INPUT_REPORT_MOUSE_H_

#include "src/ui/input/lib/hid-input-report/device.h"

namespace hid_input_report {

class Mouse : public Device {
 public:
  ParseResult ParseReportDescriptor(const hid::ReportDescriptor& hid_report_descriptor) override;

  ParseResult CreateDescriptor(fidl::AnyArena& allocator,
                               fuchsia_input_report::wire::DeviceDescriptor& descriptor) override;

  std::optional<uint8_t> InputReportId() const override { return report_id_; }

  DeviceType GetDeviceType() const override { return DeviceType::kMouse; }

 private:
  ParseResult ParseInputReportInternal(
      const uint8_t* data, size_t len, fidl::AnyArena& allocator,
      fuchsia_input_report::wire::InputReport& input_report) override;

  std::optional<hid::Attributes> movement_x_;
  std::optional<hid::Attributes> movement_y_;
  std::optional<hid::Attributes> position_x_;
  std::optional<hid::Attributes> position_y_;
  std::optional<hid::Attributes> scroll_v_;
  std::array<hid::Attributes, fuchsia_input_report::wire::kMouseMaxNumButtons> buttons_;
  size_t num_buttons_ = 0;

  size_t report_size_ = 0;
  uint8_t report_id_ = 0;
};

}  // namespace hid_input_report

#endif  // SRC_UI_INPUT_LIB_HID_INPUT_REPORT_MOUSE_H_
