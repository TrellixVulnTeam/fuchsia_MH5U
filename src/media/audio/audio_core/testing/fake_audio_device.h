// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_DEVICE_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_DEVICE_H_

#include "src/media/audio/audio_core/audio_device.h"
#include "src/media/audio/audio_core/audio_driver.h"
#include "src/media/audio/audio_core/device_registry.h"
#include "src/media/audio/audio_core/mixer/mixer.h"
#include "src/media/audio/audio_core/mixer/no_op.h"

namespace media::audio::testing {

class FakeAudioDevice : public AudioDevice {
 public:
  FakeAudioDevice(AudioDevice::Type type, ThreadingModel* threading_model, DeviceRegistry* registry,
                  LinkMatrix* link_matrix, std::shared_ptr<AudioClockFactory> clock_factory)
      : AudioDevice(type, "", threading_model, registry, link_matrix, std::move(clock_factory),
                    std::make_unique<AudioDriver>(this)),
        mix_domain_(threading_model->AcquireMixDomain("fake-audio-device")) {}

  bool driver_info_fetched() { return driver_info_fetched_; }
  bool driver_config_complete() { return driver_config_complete_; }
  bool driver_start_complete() { return driver_start_complete_; }
  bool driver_stop_complete() { return driver_stop_complete_; }
  std::pair<bool, zx::time> driver_plug_state() { return {driver_plug_state_, driver_plug_time_}; }

  // |media::audio::AudioDevice|
  void ApplyGainLimits(fuchsia::media::AudioGainInfo* in_out_info,
                       fuchsia::media::AudioGainValidFlags set_flags) override {}
  void OnWakeup() override {}
  void OnDriverInfoFetched() override { driver_info_fetched_ = true; }
  void OnDriverConfigComplete() override { driver_config_complete_ = true; }
  void OnDriverStartComplete() override { driver_start_complete_ = true; }
  void OnDriverStopComplete() override { driver_stop_complete_ = true; }
  void OnDriverPlugStateChange(bool plugged, zx::time plug_time) override {
    driver_plug_state_ = plugged;
    driver_plug_time_ = plug_time;
  }

  using AudioDevice::SetPresentationDelay;

 protected:
  ThreadingModel::OwnedDomainPtr mix_domain_;

 private:
  bool driver_info_fetched_ = false;
  bool driver_config_complete_ = false;
  bool driver_start_complete_ = false;
  bool driver_stop_complete_ = false;
  bool driver_plug_state_ = false;
  zx::time driver_plug_time_;
};

class FakeAudioInput : public FakeAudioDevice {
 public:
  static std::shared_ptr<FakeAudioInput> Create(ThreadingModel* threading_model,
                                                DeviceRegistry* registry, LinkMatrix* link_matrix,
                                                std::shared_ptr<AudioClockFactory> clock_factory) {
    return std::make_shared<FakeAudioInput>(threading_model, registry, link_matrix,
                                            std::move(clock_factory));
  }

  FakeAudioInput(ThreadingModel* threading_model, DeviceRegistry* registry, LinkMatrix* link_matrix,
                 std::shared_ptr<AudioClockFactory> clock_factory)
      : FakeAudioDevice(Type::Input, threading_model, registry, link_matrix,
                        std::move(clock_factory)) {}
};

class FakeAudioOutput : public FakeAudioDevice {
 public:
  static std::shared_ptr<FakeAudioOutput> Create(ThreadingModel* threading_model,
                                                 DeviceRegistry* registry, LinkMatrix* link_matrix,
                                                 std::shared_ptr<AudioClockFactory> clock_factory) {
    return std::make_shared<FakeAudioOutput>(threading_model, registry, link_matrix,
                                             std::move(clock_factory));
  }

  FakeAudioOutput(ThreadingModel* threading_model, DeviceRegistry* registry,
                  LinkMatrix* link_matrix, std::shared_ptr<AudioClockFactory> clock_factory)
      : FakeAudioDevice(Type::Output, threading_model, registry, link_matrix,
                        std::move(clock_factory)) {}

  fpromise::result<std::pair<std::shared_ptr<Mixer>, ExecutionDomain*>, zx_status_t>
  InitializeSourceLink(const AudioObject& source, std::shared_ptr<ReadableStream> stream) override {
    stream->SetPresentationDelay(presentation_delay());
    stream_ = std::move(stream);
    return fpromise::ok(std::make_pair(mixer_, mix_domain_.get()));
  }

  const std::shared_ptr<ReadableStream>& stream() const { return stream_; }

 private:
  std::shared_ptr<ReadableStream> stream_;
  std::shared_ptr<mixer::NoOp> mixer_ = std::make_shared<mixer::NoOp>();
};

}  // namespace media::audio::testing

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_TESTING_FAKE_AUDIO_DEVICE_H_
