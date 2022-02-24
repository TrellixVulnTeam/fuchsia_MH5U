// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_DAI_TEST_DAI_TEST_H_
#define SRC_MEDIA_AUDIO_DRIVERS_DAI_TEST_DAI_TEST_H_

#include <fidl/fuchsia.hardware.audio/cpp/wire.h>
#include <fuchsia/hardware/audio/cpp/banjo.h>
#include <fuchsia/hardware/audio/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async/cpp/task.h>

#include <optional>

#include <ddktl/device.h>
#include <ddktl/fidl.h>
#include <sdk/lib/fidl/cpp/binding.h>

namespace audio::daitest {

class DaiTest;
using DaiTestDeviceType =
    ddk::Device<DaiTest, ddk::Messageable<fuchsia_hardware_audio::Device>::Mixin>;

class DaiTest : public DaiTestDeviceType,
                public ddk::internal::base_protocol,
                public ::fuchsia::hardware::audio::StreamConfig {
 public:
  explicit DaiTest(zx_device_t* parent, bool is_input);
  virtual ~DaiTest() = default;
  void DdkRelease() { delete this; }
  zx_status_t InitPDev();

 private:
  // FIDL LLCPP methods for fuchsia.hardware.audio.Device.
  void GetChannel(GetChannelRequestView request, GetChannelCompleter::Sync& completer) override;

  // FIDL HLCPP methods for fuchsia.hardware.audio.StreamConfig.
  void GetProperties(GetPropertiesCallback callback) override;
  void GetSupportedFormats(GetSupportedFormatsCallback callback) override;
  void CreateRingBuffer(
      ::fuchsia::hardware::audio::Format format,
      ::fidl::InterfaceRequest<::fuchsia::hardware::audio::RingBuffer> intf) override;
  void WatchGainState(WatchGainStateCallback callback) override;
  void SetGain(::fuchsia::hardware::audio::GainState target_state) override;
  void WatchPlugState(WatchPlugStateCallback callback) override;
  void GetHealthState(GetHealthStateCallback callback) override { callback({}); }
  void SignalProcessingConnect(
      fidl::InterfaceRequest<fuchsia::hardware::audio::signalprocessing::SignalProcessing>
          signal_processing) override {
    signal_processing.Close(ZX_ERR_NOT_SUPPORTED);
  }

  std::optional<fidl::Binding<::fuchsia::hardware::audio::StreamConfig>> stream_config_binding_;
  ::fuchsia::hardware::audio::DaiSyncPtr dai_;
  async::Loop loop_;
  ddk::DaiProtocolClient proto_client_;
  zx_time_t plug_time_ = 0;
  bool is_input_ = false;
};

}  // namespace audio::daitest

#endif  // SRC_MEDIA_AUDIO_DRIVERS_DAI_TEST_DAI_TEST_H_
