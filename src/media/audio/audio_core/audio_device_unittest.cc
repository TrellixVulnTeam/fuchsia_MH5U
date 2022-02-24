// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_device.h"

#include <cstring>
#include <memory>

#include <gtest/gtest.h>

#include "src/media/audio/audio_core/audio_device_manager.h"
#include "src/media/audio/audio_core/audio_driver.h"
#include "src/media/audio/audio_core/device_registry.h"
#include "src/media/audio/audio_core/testing/fake_audio_driver.h"
#include "src/media/audio/audio_core/testing/threading_model_fixture.h"
#include "src/media/audio/lib/clock/testing/clock_test.h"

namespace media::audio {
namespace {

class FakeAudioDevice : public AudioDevice {
 public:
  FakeAudioDevice(AudioDevice::Type type, ThreadingModel* threading_model, DeviceRegistry* registry,
                  LinkMatrix* link_matrix, std::shared_ptr<AudioClockFactory> clock_factory)
      : AudioDevice(type, "", threading_model, registry, link_matrix, clock_factory,
                    std::make_unique<AudioDriver>(this)) {}

  // Needed because AudioDevice is an abstract class
  void ApplyGainLimits(fuchsia::media::AudioGainInfo* in_out_info,
                       fuchsia::media::AudioGainValidFlags set_flags) override {}
  void OnWakeup() FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain().token()) override {
    driver()->GetDriverInfo();
  }
  void OnDriverInfoFetched() FXL_EXCLUSIVE_LOCKS_REQUIRED(mix_domain().token()) override {
    driver_info_fetched_ = true;
  }
  bool driver_info_fetched_ = false;
};

class AudioDeviceTest : public testing::ThreadingModelFixture {
 protected:
  static constexpr uint32_t kCustomClockDomain = 42;

  void SetUp() override {
    device_ = std::make_shared<FakeAudioDevice>(
        AudioObject::Type::Input, &threading_model(), &context().device_manager(),
        &context().link_matrix(), context().clock_factory());

    zx::channel c1, c2;
    ASSERT_EQ(ZX_OK, zx::channel::create(0, &c1, &c2));
    remote_driver_ = std::make_unique<testing::FakeAudioDriver>(std::move(c1), dispatcher());
    remote_driver_->set_clock_domain(kCustomClockDomain);

    device_->driver()->Init(std::move(c2));
    remote_driver_->Start();
  }
  std::shared_ptr<FakeAudioDevice> device_;
  std::unique_ptr<testing::FakeAudioDriver> remote_driver_;
};

TEST_F(AudioDeviceTest, UniqueIdFromString) {
  const auto id_result_from_invalid_length = AudioDevice::UniqueIdFromString("efef");
  EXPECT_TRUE(id_result_from_invalid_length.is_error());

  const auto id_result_from_invalid_content =
      AudioDevice::UniqueIdFromString("eeeeeeeeeeeeeeeeeeeeeeeeeeeeee&8");
  EXPECT_TRUE(id_result_from_invalid_content.is_error());

  const audio_stream_unique_id_t unique_id = {.data = {0xff, 0xeb}};
  const auto valid_string = AudioDevice::UniqueIdToString(unique_id);
  const auto id_result_from_valid = AudioDevice::UniqueIdFromString(valid_string);
  EXPECT_TRUE(id_result_from_valid.is_ok());

  EXPECT_EQ(memcmp(id_result_from_valid.value().data, unique_id.data, 16), 0)
      << "Expected: " << valid_string
      << " got: " << AudioDevice::UniqueIdToString(id_result_from_valid.value());
}

TEST_F(AudioDeviceTest, UniqueIdFromStringMixedCase) {
  const audio_stream_unique_id_t unique_id = {
      .data = {0xff, 0xeb, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
  const auto valid_string = "FFeB0000000000000000000000000000";
  const auto id_result_from_valid = AudioDevice::UniqueIdFromString(valid_string);
  EXPECT_TRUE(id_result_from_valid.is_ok());

  EXPECT_EQ(memcmp(id_result_from_valid.value().data, unique_id.data, 16), 0)
      << "Expected: " << valid_string
      << " got: " << AudioDevice::UniqueIdToString(id_result_from_valid.value());
}

// After GetDriverInfo, the clock domain has been set and the ref clock is valid.
TEST_F(AudioDeviceTest, ReferenceClockIsAdvancing) {
  threading_model().FidlDomain().ScheduleTask(device_->Startup());

  RunLoopUntilIdle();
  EXPECT_TRUE(device_->driver_info_fetched_);
  clock::testing::VerifyAdvances(device_->reference_clock(), context().clock_factory());
}

TEST_F(AudioDeviceTest, DefaultClockIsClockMono) {
  threading_model().FidlDomain().ScheduleTask(device_->Startup());
  RunLoopUntilIdle();
  EXPECT_TRUE(device_->driver_info_fetched_);

  clock::testing::VerifyIsSystemMonotonic(device_->reference_clock());
}

}  // namespace
}  // namespace media::audio
