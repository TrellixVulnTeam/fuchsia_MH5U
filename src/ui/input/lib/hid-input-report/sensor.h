// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_INPUT_LIB_HID_INPUT_REPORT_SENSOR_H_
#define SRC_UI_INPUT_LIB_HID_INPUT_REPORT_SENSOR_H_

#include "src/ui/input/lib/hid-input-report/device.h"

namespace hid_input_report {

class Sensor : public Device {
 public:
  ParseResult ParseReportDescriptor(const hid::ReportDescriptor& hid_report_descriptor) override;

  ParseResult CreateDescriptor(fidl::AnyArena& allocator,
                               fuchsia_input_report::wire::DeviceDescriptor& descriptor) override;

  std::optional<uint8_t> InputReportId() const override { return report_id_; }

  DeviceType GetDeviceType() const override { return DeviceType::kSensor; }

 private:
  ParseResult ParseInputReportInternal(
      const uint8_t* data, size_t len, fidl::AnyArena& allocator,
      fuchsia_input_report::wire::InputReport& input_report) override;

  hid::Attributes values_[fuchsia_input_report::wire::kSensorMaxValues] = {};
  size_t num_values_ = 0;

  size_t report_size_ = 0;
  uint8_t report_id_ = 0;
};

}  // namespace hid_input_report

#endif  // SRC_UI_INPUT_LIB_HID_INPUT_REPORT_SENSOR_H_
