// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_LIB_TEST_HERMETIC_AUDIO_TEST_H_
#define SRC_MEDIA_AUDIO_LIB_TEST_HERMETIC_AUDIO_TEST_H_

#include <fuchsia/thermal/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/device/audio.h>

#include <queue>
#include <unordered_map>
#include <unordered_set>

#include <test/thermal/cpp/fidl.h>

#include "src/media/audio/lib/format/audio_buffer.h"
#include "src/media/audio/lib/test/capturer_shim.h"
#include "src/media/audio/lib/test/constants.h"
#include "src/media/audio/lib/test/hermetic_audio_environment.h"
#include "src/media/audio/lib/test/inspect.h"
#include "src/media/audio/lib/test/renderer_shim.h"
#include "src/media/audio/lib/test/test_fixture.h"
#include "src/media/audio/lib/test/virtual_device.h"

namespace media::audio::test {

// Restrictions on usage:
//
// 1. This class is thread hostile: none of its methods can be called concurrently.
// 2. It is illegal for two or more instances to be alive at any time. (This restriction
//    is satisfied by ordinary usage of gtest.)
//
class HermeticAudioTest : public TestFixture {
 protected:
  // Tests that require real-time response should have no data loss from overflow or underflow if
  // run in a capable environment, but known issues can prevent this.
  // TODO(fxbug.dev/80003): re-enable underflow detection once outstanding bugs are resolved.
  static constexpr bool kEnableAllOverflowAndUnderflowChecksInRealtimeTests = false;

  // TestSuite functions are run once per test suite; a suite can configure
  // HermeticAudioEnvironment::Options for all tests by calling `SetTestSuiteEnvironmentOptions()`
  // in an override of `SetUpTestSuite()`.
  static void SetTestSuiteEnvironmentOptions(HermeticAudioEnvironment::Options options);

  // The default implementation calls SetTestSuiteEnvironmentOptions() with default Options.
  // Test suites can override this to provide custom behavior.
  static void SetUpTestSuite();

  void SetUp() override;
  void TearDown() override;

  HermeticAudioEnvironment* environment() {
    auto ptr = HermeticAudioTest::environment_.get();
    FX_CHECK(ptr) << "No Environment; Did you forget to call SetUp?";
    return ptr;
  }

  // The returned pointers are owned by this class.
  template <fuchsia::media::AudioSampleFormat SampleFormat>
  VirtualOutput<SampleFormat>* CreateOutput(
      const audio_stream_unique_id_t& device_id, TypedFormat<SampleFormat> format,
      int64_t frame_count, std::optional<DevicePlugProperties> plug_properties = std::nullopt,
      float device_gain_db = 0,
      std::optional<DeviceClockProperties> device_clock_properties = std::nullopt);

  template <fuchsia::media::AudioSampleFormat SampleFormat>
  VirtualInput<SampleFormat>* CreateInput(
      const audio_stream_unique_id_t& device_id, TypedFormat<SampleFormat> format,
      int64_t frame_count, std::optional<DevicePlugProperties> plug_properties = std::nullopt,
      float device_gain_db = 0,
      std::optional<DeviceClockProperties> device_clock_properties = std::nullopt);

  template <fuchsia::media::AudioSampleFormat SampleFormat>
  AudioRendererShim<SampleFormat>* CreateAudioRenderer(
      TypedFormat<SampleFormat> format, int64_t frame_count,
      fuchsia::media::AudioRenderUsage usage = fuchsia::media::AudioRenderUsage::MEDIA,
      std::optional<zx::clock> reference_clock = std::nullopt);

  template <fuchsia::media::AudioSampleFormat SampleFormat>
  AudioCapturerShim<SampleFormat>* CreateAudioCapturer(
      TypedFormat<SampleFormat> format, int64_t frame_count,
      fuchsia::media::AudioCapturerConfiguration config);

  template <fuchsia::media::AudioSampleFormat SampleFormat>
  UltrasoundRendererShim<SampleFormat>* CreateUltrasoundRenderer(TypedFormat<SampleFormat> format,
                                                                 int64_t frame_count,
                                                                 bool wait_for_creation = true);

  template <fuchsia::media::AudioSampleFormat SampleFormat>
  UltrasoundCapturerShim<SampleFormat>* CreateUltrasoundCapturer(TypedFormat<SampleFormat> format,
                                                                 int64_t frame_count,
                                                                 bool wait_for_creation = true);

  // Validate inspect metrics.
  void ExpectInspectMetrics(VirtualOutputImpl* output, const ExpectedInspectProperties& props);
  void ExpectInspectMetrics(VirtualInputImpl* input, const ExpectedInspectProperties& props);
  void ExpectInspectMetrics(RendererShimImpl* renderer, const ExpectedInspectProperties& props);
  void ExpectInspectMetrics(CapturerShimImpl* capturer, const ExpectedInspectProperties& props);

  // Fail the test if any overflow or underflow is reported. This includes the below four subcases:
  // * Output underflow: data was lost because we awoke too late to provide data.
  // * Pipeline underflow: pipeline processing took longer than expected (for now, this includes
  //   cases where the time overrun did not necessarily result in data loss).
  // * Renderer underflow: data was lost because a renderer client provided it to us too late.
  // * Capturer overflow: data was lost because we had no available buffer from a capturer-client.
  void ExpectNoOverflowsOrUnderflows();
  void ExpectNoOutputUnderflows();
  void ExpectNoPipelineUnderflows();
  void ExpectNoRendererUnderflows();
  void ExpectNoCapturerOverflows();

  template <fuchsia::media::AudioSampleFormat OutputFormat>
  bool DeviceHasUnderflows(VirtualOutput<OutputFormat>* device);

  // Unbind and forget about the given object.
  void Unbind(VirtualOutputImpl* device);
  void Unbind(VirtualInputImpl* device);
  void Unbind(RendererShimImpl* renderer);
  void Unbind(CapturerShimImpl* capturer);

  // Takes ownership of the AudioDeviceEnumerator. This is useful when tests need to watch for
  // low-level device enumeration events. This is incompatible with CreateInput and CreateOutput.
  fuchsia::media::AudioDeviceEnumeratorPtr TakeOwnershipOfAudioDeviceEnumerator();

  // Direct access to FIDL channels. Using these objects directly may not play well with this class.
  // These are provided for special cases only.
  fuchsia::media::AudioCorePtr audio_core_;
  fuchsia::media::AudioDeviceEnumeratorPtr audio_dev_enum_;

  ::test::thermal::ControlSyncPtr& thermal_test_control() { return thermal_test_control_sync_; }
  fuchsia::media::audio::EffectsControllerSyncPtr& effects_controller() {
    return effects_controller_;
  }

 private:
  // Configurable for an entire test suite by calling `SetUpTestSuiteWithOptions()`.
  static std::optional<HermeticAudioEnvironment::Options> test_suite_options_;

  // Initializes the HermeticAudioEnvironment for each test instance during `SetUp()`.
  void SetUpEnvironment();
  // Tears down the HermeticAudioEnvironment for each test instance during `TearDown()`.
  void TearDownEnvironment();

  void WatchForDeviceArrivals();
  void WaitForDeviceDepartures();
  void OnDeviceAdded(fuchsia::media::AudioDeviceInfo info);
  void OnDefaultDeviceChanged(uint64_t old_default_token, uint64_t new_default_token);
  void ExpectInspectMetrics(const std::vector<std::string>& path,
                            const ExpectedInspectProperties& props);

  struct DeviceInfo {
    std::unique_ptr<VirtualOutputImpl> output;
    std::unique_ptr<VirtualInputImpl> input;
    std::optional<fuchsia::media::AudioDeviceInfo> info;
    bool is_removed = false;
    bool is_default = false;
  };

  // Ensures all devices have been accounted for before the most recent OnDefaultDeviceChanged
  // callback can be processed.
  bool initial_devices_received_ = false;
  std::queue<uint64_t> pending_default_device_tokens_;

  std::unordered_map<uint64_t, std::string> token_to_unique_id_;
  std::unordered_map<std::string, DeviceInfo> devices_;
  std::vector<std::unique_ptr<CapturerShimImpl>> capturers_;
  std::vector<std::unique_ptr<RendererShimImpl>> renderers_;

  std::unique_ptr<HermeticAudioEnvironment> environment_;
  fuchsia::virtualaudio::ControlSyncPtr virtual_audio_control_sync_;
  fuchsia::thermal::ControllerPtr thermal_controller_;
  ::test::thermal::ControlSyncPtr thermal_test_control_sync_;
  fuchsia::ultrasound::FactoryPtr ultrasound_factory_;
  fuchsia::media::audio::EffectsControllerSyncPtr effects_controller_;

  size_t capturer_shim_next_inspect_id_ = 1;
  size_t renderer_shim_next_inspect_id_ = 1;
  size_t virtual_output_next_inspect_id_ = 0;
  size_t virtual_input_next_inspect_id_ = 0;
};

}  // namespace media::audio::test

#endif  // SRC_MEDIA_AUDIO_LIB_TEST_HERMETIC_AUDIO_TEST_H_
