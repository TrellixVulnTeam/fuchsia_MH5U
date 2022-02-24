// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/lib/test/virtual_device.h"

#include <lib/syslog/cpp/macros.h>

#include "src/media/audio/lib/format/driver_format.h"
#include "src/media/audio/lib/timeline/timeline_function.h"

namespace media::audio::test {

template <class Iface>
VirtualDevice<Iface>::VirtualDevice(TestFixture* fixture, HermeticAudioEnvironment* environment,
                                    const audio_stream_unique_id_t& device_id, Format format,
                                    int64_t frame_count, size_t inspect_id,
                                    std::optional<DevicePlugProperties> plug_properties,
                                    float expected_gain_db,
                                    std::optional<DeviceClockProperties> device_clock_properties)
    : format_(format),
      frame_count_(frame_count),
      inspect_id_(inspect_id),
      expected_gain_db_(expected_gain_db),
      rb_(format, frame_count) {
  environment->ConnectToService(fidl_.NewRequest());
  fixture->AddErrorHandler(fidl_, "VirtualAudioDevice");
  WatchEvents();

  std::array<uint8_t, 16> device_id_array;
  std::copy(std::begin(device_id.data), std::end(device_id.data), std::begin(device_id_array));
  fidl_->SetUniqueId(device_id_array);

  if (plug_properties) {
    fidl_->SetPlugProperties(plug_properties->plug_change_time.get(), plug_properties->plugged,
                             plug_properties->hardwired, plug_properties->can_notify);
  }

  if (!AudioSampleFormatToDriverSampleFormat(format_.sample_format(), &driver_format_)) {
    FX_CHECK(false) << "Failed to convert Fmt 0x" << std::hex
                    << static_cast<uint32_t>(format_.sample_format()) << " to driver format.";
  }
  fidl_->ClearFormatRanges();
  fidl_->AddFormatRange(driver_format_, format_.frames_per_second(), format_.frames_per_second(),
                        static_cast<int8_t>(format_.channels()),
                        static_cast<uint8_t>(format_.channels()), ASF_RANGE_FLAG_FPS_CONTINUOUS);

  fidl_->SetFifoDepth(kFifoDepthBytes);
  fidl_->SetExternalDelay(kExternalDelay.get());

  fidl_->SetRingBufferRestrictions(static_cast<uint32_t>(frame_count),
                                   static_cast<uint32_t>(frame_count),
                                   static_cast<uint32_t>(frame_count));

  auto ring_buffer_ms = static_cast<uint32_t>(
      static_cast<double>(frame_count) / static_cast<double>(format_.frames_per_second()) * 1000);
  fidl_->SetNotificationFrequency(ring_buffer_ms / kNotifyMs);

  if (device_clock_properties) {
    fidl_->SetClockProperties(device_clock_properties->domain,
                              device_clock_properties->initial_rate_adjustment_ppm);
  }

  fidl_->Add();
}

template <class Iface>
VirtualDevice<Iface>::~VirtualDevice() {
  ResetEvents();
  if (fidl_.is_bound()) {
    fidl_->Remove();
  }
}

template <class Iface>
void VirtualDevice<Iface>::ResetEvents() {
  fidl_.events().OnSetFormat = nullptr;
  fidl_.events().OnSetGain = nullptr;
  fidl_.events().OnBufferCreated = nullptr;
  fidl_.events().OnStart = nullptr;
  fidl_.events().OnStop = nullptr;
  fidl_.events().OnPositionNotify = nullptr;
}

template <class Iface>
void VirtualDevice<Iface>::WatchEvents() {
  fidl_.events().OnSetFormat = [this](int32_t fps, uint32_t fmt, int32_t num_chans,
                                      zx_duration_t ext_delay) {
    received_set_format_ = true;
    EXPECT_EQ(fps, format_.frames_per_second());
    EXPECT_EQ(fmt, driver_format_);
    EXPECT_EQ(num_chans, format_.channels());
    EXPECT_EQ(ext_delay, kExternalDelay.get());
    FX_LOGS(DEBUG) << "OnSetFormat callback: " << fps << ", " << fmt << ", " << num_chans << ", "
                   << ext_delay;
  };

  fidl_.events().OnSetGain = [this](bool cur_mute, bool cur_agc, float cur_gain_db) {
    EXPECT_EQ(cur_gain_db, expected_gain_db_);
    EXPECT_FALSE(cur_mute);
    EXPECT_FALSE(cur_agc);
    FX_LOGS(DEBUG) << "OnSetGain callback: " << cur_mute << ", " << cur_agc << ", " << cur_gain_db;
  };

  fidl_.events().OnBufferCreated = [this](zx::vmo ring_buffer_vmo,
                                          uint32_t driver_reported_frame_count,
                                          uint32_t notifications_per_ring) {
    ASSERT_EQ(frame_count_, driver_reported_frame_count);
    ASSERT_TRUE(received_set_format_);
    rb_vmo_ = std::move(ring_buffer_vmo);
    rb_.MapVmo(rb_vmo_);
    FX_LOGS(DEBUG) << "OnBufferCreated callback: " << driver_reported_frame_count << " frames, "
                   << notifications_per_ring << " notifs/ring";
  };

  fidl_.events().OnStart = [this](zx_time_t start_time) {
    ASSERT_TRUE(received_set_format_);
    ASSERT_TRUE(rb_vmo_.is_valid());
    received_start_ = true;
    start_time_ = zx::time(start_time);
    // Compute a function to translate from ring buffer position to device time.
    auto ns_per_byte = TimelineRate::Product(format_.frames_per_ns().Inverse(),
                                             TimelineRate(1, format_.bytes_per_frame()));
    running_pos_to_ref_time_ = TimelineFunction(start_time_.get(), 0, ns_per_byte);
    FX_LOGS(DEBUG) << "OnStart callback: " << start_time;
  };

  fidl_.events().OnStop = [this](zx_time_t stop_time, uint32_t ring_pos) {
    received_stop_ = true;
    stop_time_ = zx::time(stop_time);
    stop_pos_ = ring_pos;
    FX_LOGS(DEBUG) << "OnStop callback: " << stop_time << ", " << ring_pos;
  };

  fidl_.events().OnPositionNotify = [this](zx_time_t monotonic_time, uint32_t ring_pos) {
    // compare to prev ring_pos - if less, then add rb_.SizeBytes().
    if (ring_pos < ring_pos_) {
      running_ring_pos_ += rb_.SizeBytes();
    }
    running_ring_pos_ += ring_pos;
    running_ring_pos_ -= ring_pos_;
    ring_pos_ = ring_pos;
    FX_LOGS(TRACE) << "OnPositionNotify callback: " << monotonic_time << ", " << ring_pos;
  };
}

template <class Iface>
zx::time VirtualDevice<Iface>::NextSynchronizedTimestamp(zx::time min_time) const {
  // Compute the next synchronized position, then iterate until we find a synchronized
  // position at min_time or later.
  int64_t running_pos_sync = ((running_ring_pos_ / rb_.SizeBytes()) + 1) * rb_.SizeBytes();
  while (true) {
    zx::time sync_time = zx::time(running_pos_to_ref_time_.Apply(running_pos_sync));
    if (sync_time >= min_time) {
      return sync_time;
    }
    running_pos_sync += rb_.SizeBytes();
  }
}

template <class Iface>
int64_t VirtualDevice<Iface>::RingBufferFrameAtTimestamp(zx::time ref_time) const {
  int64_t running_pos = running_pos_to_ref_time_.ApplyInverse(ref_time.get());
  return running_pos / format_.bytes_per_frame();
}

// Only two instantiations are needed.
template class VirtualDevice<fuchsia::virtualaudio::Output>;
template class VirtualDevice<fuchsia::virtualaudio::Input>;

}  // namespace media::audio::test
