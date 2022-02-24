// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/audio_driver.h"

#include <fuchsia/media/cpp/fidl.h>
#include <lib/async/cpp/time.h>
#include <lib/fidl/cpp/clone.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>
#include <lib/zx/clock.h>
#include <zircon/errors.h>
#include <zircon/status.h>

#include <algorithm>
#include <cstdio>
#include <iomanip>

#include <audio-proto-utils/format-utils.h>
#include <fbl/algorithm.h>

#include "src/media/audio/lib/clock/clone_mono.h"
#include "src/media/audio/lib/clock/utils.h"
#include "src/media/audio/lib/format/driver_format.h"

namespace media::audio {
namespace {

// For non-zero value N, log every Nth position notification. If 0, don't log any.
static constexpr uint16_t kPositionNotificationDisplayInterval = 0;

// TODO(fxbug.dev/39092): Log a cobalt metric for this.
void LogMissedCommandDeadline(zx::duration delay) {
  FX_LOGS(WARNING) << "Driver command missed deadline by " << delay.to_nsecs() << "ns";
}

}  // namespace

AudioDriver::AudioDriver(AudioDevice* owner) : AudioDriver(owner, LogMissedCommandDeadline) {}

AudioDriver::AudioDriver(AudioDevice* owner, DriverTimeoutHandler timeout_handler)
    : owner_(owner),
      timeout_handler_(std::move(timeout_handler)),
      versioned_ref_time_to_frac_presentation_frame_(
          fbl::MakeRefCounted<VersionedTimelineFunction>()) {
  FX_DCHECK(owner_ != nullptr);
}

zx_status_t AudioDriver::Init(zx::channel stream_channel) {
  TRACE_DURATION("audio", "AudioDriver::Init");
  // TODO(fxbug.dev/13665): Figure out a better way to assert this!
  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
  FX_DCHECK(state_ == State::Uninitialized);

  // Fetch the KOID of our stream channel. We use this unique ID as our device's device token.
  zx_status_t res;
  zx_info_handle_basic_t sc_info;
  res = stream_channel.get_info(ZX_INFO_HANDLE_BASIC, &sc_info, sizeof(sc_info), nullptr, nullptr);
  if (res != ZX_OK) {
    FX_PLOGS(ERROR, res) << "Failed to to fetch stream channel KOID";
    return res;
  }
  stream_channel_koid_ = sc_info.koid;

  stream_config_fidl_ =
      fidl::InterfaceHandle<fuchsia::hardware::audio::StreamConfig>(std::move(stream_channel))
          .Bind();
  if (!stream_config_fidl_.is_bound()) {
    FX_LOGS(ERROR) << "Failed to get stream channel";
    return ZX_ERR_INTERNAL;
  }
  stream_config_fidl_.set_error_handler([this](zx_status_t status) -> void {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
    ShutdownSelf("Stream channel closed", status);
  });

  cmd_timeout_.set_handler([this] {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
    DriverCommandTimedOut();
  });

  // We are now initialized, but we don't know any fundamental driver level info, such as:
  //
  // 1) This device's persistent unique ID.
  // 2) The list of formats supported by this device.
  // 3) The user-visible strings for this device (manufacturer, product, etc...).
  state_ = State::MissingDriverInfo;
  return ZX_OK;
}

void AudioDriver::Cleanup() {
  TRACE_DURATION("audio", "AudioDriver::Cleanup");
  // TODO(fxbug.dev/13665): Figure out a better way to assert this!
  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
  std::shared_ptr<ReadableRingBuffer> readable_ring_buffer;
  std::shared_ptr<WritableRingBuffer> writable_ring_buffer;
  {
    std::lock_guard<std::mutex> lock(ring_buffer_state_lock_);
    readable_ring_buffer = std::move(readable_ring_buffer_);
    writable_ring_buffer = std::move(writable_ring_buffer_);
  }
  versioned_ref_time_to_frac_presentation_frame_->Update({});
  readable_ring_buffer = nullptr;
  writable_ring_buffer = nullptr;

  cmd_timeout_.Cancel();
  stream_config_fidl_ = nullptr;
  ring_buffer_fidl_ = nullptr;
}

std::optional<Format> AudioDriver::GetFormat() const {
  TRACE_DURATION("audio", "AudioDriver::GetFormat");
  std::lock_guard<std::mutex> lock(configured_format_lock_);
  return configured_format_;
}

zx_status_t AudioDriver::GetDriverInfo() {
  TRACE_DURATION("audio", "AudioDriver::GetDriverInfo");
  // TODO(fxbug.dev/13665): Figure out a better way to assert this!
  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());

  // We have to be operational in order to fetch supported formats.
  if (!operational()) {
    FX_LOGS(ERROR) << "Cannot fetch supported formats while non-operational (state = "
                   << static_cast<uint32_t>(state_) << ")";
    return ZX_ERR_BAD_STATE;
  }

  // If already fetching initial driver info, get out now and inform our owner when this completes.
  if (fetching_driver_info()) {
    return ZX_OK;
  }

  // Send the commands to get:
  // - persistent unique ID.
  // - manufacturer string.
  // - product string.
  // - gain capabilities.
  // - current gain state.
  // - supported format list.
  // - clock domain.

  // Get unique IDs, strings and gain capabilites.
  stream_config_fidl_->GetProperties([this](fuchsia::hardware::audio::StreamProperties props) {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
    if (state_ != State::MissingDriverInfo) {
      FX_LOGS(ERROR) << "Bad state (" << static_cast<uint32_t>(state_)
                     << ") while handling get string response.";
      ShutdownSelf("Bad state.", ZX_ERR_INTERNAL);
    }
    hw_gain_state_.can_mute = props.has_can_mute() && props.can_mute();
    hw_gain_state_.can_agc = props.has_can_agc() && props.can_agc();
    hw_gain_state_.min_gain = props.min_gain_db();
    hw_gain_state_.max_gain = props.max_gain_db();
    hw_gain_state_.gain_step = props.gain_step_db();

    if (props.has_unique_id()) {
      std::memcpy(persistent_unique_id_.data, props.unique_id().data(),
                  sizeof(persistent_unique_id_.data));
    }

    if (props.has_manufacturer()) {
      manufacturer_name_ = props.manufacturer();
    }
    if (props.has_product()) {
      product_name_ = props.product();
    }

    clock_domain_ = props.clock_domain();
    FX_LOGS(DEBUG) << "Received clock domain " << clock_domain_;

    // Now that we have our clock domain, we can establish our audio device clock
    SetUpClocks();

    auto res = OnDriverInfoFetched(kDriverInfoHasUniqueId | kDriverInfoHasMfrStr |
                                   kDriverInfoHasProdStr | kDriverInfoHasClockDomain);
    if (res != ZX_OK) {
      ShutdownSelf("Failed to update info fetched.", res);
    }

    pd_hardwired_ = (props.plug_detect_capabilities() ==
                     fuchsia::hardware::audio::PlugDetectCapabilities::HARDWIRED);
  });

  // Get current gain state.
  // We only fetch once per OnDriverInfoFetched, the we are guaranteed by the
  // audio driver interface definition that the driver will reply to the first watch request, we
  // can get the gain state by issuing a watch FIDL call.
  stream_config_fidl_->WatchGainState([this](fuchsia::hardware::audio::GainState state) {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
    hw_gain_state_.cur_mute = state.has_muted() && state.muted();
    hw_gain_state_.cur_agc = state.has_agc_enabled() && state.agc_enabled();
    hw_gain_state_.cur_gain = state.gain_db();
    auto res = OnDriverInfoFetched(kDriverInfoHasGainState);
    if (res != ZX_OK) {
      ShutdownSelf("Failed to update info fetched.", res);
    }
  });

  // Get list of supported formats.
  stream_config_fidl_->GetSupportedFormats(
      [this](std::vector<fuchsia::hardware::audio::SupportedFormats> formats) {
        OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
        formats_.reserve(formats.size());
        for (auto& i : formats) {
          formats_.emplace_back(std::move(*i.mutable_pcm_supported_formats()));
        }
        // Record that we have fetched our format list. This will transition us to Unconfigured
        // state and let our owner know if we are done fetching all the initial driver info needed
        // to operate.
        auto res = OnDriverInfoFetched(kDriverInfoHasFormats);
        if (res != ZX_OK) {
          ShutdownSelf("Failed to update info fetched.", res);
        }
      });

  // Setup our command timeout.
  fetch_driver_info_deadline_ =
      async::Now(owner_->mix_domain().dispatcher()) + kDefaultShortCmdTimeout;
  SetupCommandTimeout();
  return ZX_OK;
}

// Confirm that PcmSupportedFormats is well-formed (return false if not) and log the contents
bool AudioDriver::ValidatePcmSupportedFormats(
    std::vector<fuchsia::hardware::audio::PcmSupportedFormats>& formats, bool is_input) {
  for (size_t format_index = 0u; format_index < formats.size(); ++format_index) {
    if constexpr (IdlePolicy::kLogChannelFrequencyRanges) {
      FX_LOGS(INFO) << __FUNCTION__ << ": " << (is_input ? " Input" : "Output")
                    << " PcmSupportedFormats[" << format_index << "] for "
                    << (is_input ? " Input" : "Output");
    }

    if (!formats[format_index].has_channel_sets()) {
      FX_LOGS(WARNING) << (is_input ? " Input" : "Output") << " PcmSupportedFormats["
                       << format_index << "] table does not have required ChannelSets";
      return false;
    }

    if (formats[format_index].frame_rates().empty()) {
      FX_LOGS(WARNING) << (is_input ? " Input" : "Output") << " PcmSupportedFormats["
                       << format_index << "].frame_rates contains no entries";
      return false;
    }

    auto& channel_sets = formats[format_index].channel_sets();
    for (size_t channel_set_index = 0u; channel_set_index < channel_sets.size();
         ++channel_set_index) {
      auto& channel_set = channel_sets[channel_set_index];
      if (!channel_set.has_attributes()) {
        FX_LOGS(WARNING) << (is_input ? " Input" : "Output") << " PcmSupportedFormats["
                         << format_index << "].channel_sets[" << channel_set_index
                         << "] table does not have required attributes";
        return false;
      }

      if constexpr (IdlePolicy::kLogChannelFrequencyRanges) {
        auto& chan_set_attribs = channel_set.attributes();
        for (size_t channel_index = 0u; channel_index < chan_set_attribs.size(); ++channel_index) {
          if (!chan_set_attribs[channel_index].has_min_frequency()) {
            FX_LOGS(INFO) << (is_input ? " Input" : "Output") << " PcmSupportedFormats["
                          << format_index << "].channel_sets[" << channel_set_index
                          << "].chan_set_attribs[" << channel_index
                          << "] does not have min_frequency";
          }
          if (!chan_set_attribs[channel_index].has_max_frequency()) {
            FX_LOGS(INFO) << (is_input ? " Input" : "Output") << " PcmSupportedFormats["
                          << format_index << "].channel_sets[" << channel_set_index
                          << "].chan_set_attribs[" << channel_index
                          << "] does not have max_frequency";
          }
        }
      }
    }
  }

  return true;
}

zx_status_t AudioDriver::Configure(const Format& format, zx::duration min_ring_buffer_duration) {
  TRACE_DURATION("audio", "AudioDriver::Configure");
  // TODO(fxbug.dev/13665): Figure out a better way to assert this!
  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());

  uint32_t channels = format.channels();
  uint32_t frames_per_second = format.frames_per_second();
  fuchsia::media::AudioSampleFormat sample_format = format.sample_format();

  // Sanity check arguments.
  if (channels > std::numeric_limits<uint16_t>::max()) {
    FX_LOGS(ERROR) << "Bad channel count: " << channels;
    return ZX_ERR_INVALID_ARGS;
  }

  // TODO(fxbug.dev/13666): sanity check the min_ring_buffer_duration.

  // Check our known format list for compatibility.
  if (!IsFormatInSupported(format.stream_type(), formats_)) {
    FX_LOGS(ERROR) << "No compatible format found when setting format to " << frames_per_second
                   << " Hz " << channels << " Ch Fmt 0x" << std::hex
                   << static_cast<uint32_t>(sample_format);
    return ZX_ERR_INVALID_ARGS;
  }

  // We must be in Unconfigured state to change formats.
  // TODO(fxbug.dev/13667): Also permit this if we are in Configured state.
  if (state_ != State::Unconfigured) {
    FX_LOGS(ERROR) << "Bad state while attempting to configure for " << frames_per_second << " Hz "
                   << channels << " Ch Fmt 0x" << std::hex << static_cast<uint32_t>(sample_format)
                   << " (state = " << static_cast<uint32_t>(state_) << ")";
    return ZX_ERR_BAD_STATE;
  }

  bool is_input = owner_->is_input();
  if (!ValidatePcmSupportedFormats(formats_, is_input)) {
    return ZX_ERR_INTERNAL;
  }

  // Retrieve the relevant ChannelSet; stop looking through all formats/sets when we find a match.
  bool found_channel_set_match = false;
  std::vector<ChannelAttributes> channel_config;
  uint32_t max_rate = 0;
  for (auto& format : formats_) {
    max_rate = std::max(*std::max_element(format.frame_rates().begin(), format.frame_rates().end()),
                        max_rate);
  }
  for (auto& format : formats_) {
    for (auto& channel_set : format.channel_sets()) {
      auto& chan_set_attribs = channel_set.attributes();
      if (chan_set_attribs.size() != channels) {
        continue;
      }
      for (size_t channel_index = 0u; channel_index < chan_set_attribs.size(); ++channel_index) {
        // If a frequency range doesn't specify min or max, assume it extends to the boundary.
        channel_config.push_back({chan_set_attribs[channel_index].has_min_frequency()
                                      ? chan_set_attribs[channel_index].min_frequency()
                                      : 0u,
                                  chan_set_attribs[channel_index].has_max_frequency()
                                      ? chan_set_attribs[channel_index].max_frequency()
                                      : (max_rate / 2)});
      }
      found_channel_set_match = true;
      break;
    }
    if (found_channel_set_match) {
      break;
    }
  }

  // Record the details of our intended target format
  min_ring_buffer_duration_ = min_ring_buffer_duration;
  {
    std::lock_guard<std::mutex> lock(configured_format_lock_);
    configured_format_ = {format};
    configured_channel_config_.swap(channel_config);
  }

  if constexpr (IdlePolicy::kLogChannelFrequencyRanges) {
    if (channels != configured_channel_config_.size()) {
      FX_LOGS(WARNING) << "Logic error, retrieved a channel_config of incorrect length (wanted "
                       << channels << ", got " << configured_channel_config_.size();
      return ZX_ERR_INTERNAL;
    }
    for (size_t channel_index = 0u; channel_index < channels; ++channel_index) {
      FX_LOGS(INFO) << "Final configured_channel_config_[" << channel_index << "] is ("
                    << configured_channel_config_[channel_index].min_frequency << ", "
                    << configured_channel_config_[channel_index].max_frequency << ") for "
                    << (is_input ? " Input" : "Output");
    }
  }

  zx::channel local_channel;
  zx::channel remote_channel;
  zx_status_t status = zx::channel::create(0u, &local_channel, &remote_channel);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Bad status creating channel: " << status;
    return ZX_ERR_BAD_STATE;
  }
  fidl::InterfaceRequest<fuchsia::hardware::audio::RingBuffer> request = {};
  request.set_channel(std::move(remote_channel));

  DriverSampleFormat driver_format = {};
  if (!AudioSampleFormatToDriverSampleFormat(format.stream_type().sample_format, &driver_format)) {
    FX_LOGS(ERROR) << "Failed to convert Fmt 0x" << std::hex << static_cast<uint32_t>(sample_format)
                   << " to driver format.";
    return ZX_ERR_INVALID_ARGS;
  }

  fuchsia::hardware::audio::Format fidl_format = {};
  fuchsia::hardware::audio::PcmFormat pcm = {};
  pcm.number_of_channels = channels;
  pcm.bytes_per_sample = format.bytes_per_frame() / channels;
  pcm.valid_bits_per_sample = format.valid_bits_per_channel();
  pcm.frame_rate = frames_per_second;
  pcm.sample_format = driver_format.sample_format;
  fidl_format.set_pcm_format(std::move(pcm));

  if (!stream_config_fidl_.is_bound()) {
    FX_LOGS(ERROR) << "Stream channel lost";
    return ZX_ERR_INTERNAL;
  }

  stream_config_fidl_->CreateRingBuffer(std::move(fidl_format), std::move(request));
  // No need for timeout, there is no reply to this FIDL message.

  ring_buffer_fidl_ =
      fidl::InterfaceHandle<fuchsia::hardware::audio::RingBuffer>(std::move(local_channel)).Bind();
  if (!ring_buffer_fidl_.is_bound()) {
    FX_LOGS(ERROR) << "Failed to get stream channel";
    return ZX_ERR_INTERNAL;
  }
  ring_buffer_fidl_.set_error_handler([this](zx_status_t status) -> void {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
    ShutdownSelf("Ring buffer channel closed unexpectedly", status);
  });

  // Change state, setup our command timeout.
  state_ = State::Configuring_GettingFifoDepth;
  configuration_deadline_ = async::Now(owner_->mix_domain().dispatcher()) + kDefaultLongCmdTimeout;
  SetupCommandTimeout();

  ring_buffer_fidl_->GetProperties([this](fuchsia::hardware::audio::RingBufferProperties props) {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
    external_delay_ = zx::nsec(props.has_external_delay() ? props.external_delay() : 0);
    turn_on_delay_ = zx::nsec(props.has_turn_on_delay() ? props.turn_on_delay() : 0);
    uint32_t fifo_depth_bytes = props.has_fifo_depth() ? props.fifo_depth() : 0;

    auto format = GetFormat();
    auto bytes_per_frame = format->bytes_per_frame();
    auto frames_per_second = format->frames_per_second();

    fifo_depth_frames_ = (fifo_depth_bytes + bytes_per_frame - 1) / bytes_per_frame;
    fifo_depth_duration_ =
        zx::nsec(TimelineRate(ZX_SEC(1), frames_per_second).Scale(fifo_depth_frames_));

    FX_LOGS(DEBUG) << "Received external_delay " << std::setw(5) << external_delay_.to_usecs()
                   << " usec (" << (owner_->is_input() ? " Input" : "Output") << ")";
    FX_LOGS(DEBUG) << "Received turn_on_delay  " << std::setw(5) << turn_on_delay_.to_usecs()
                   << " usec (" << (owner_->is_input() ? " Input" : "Output") << ")";
    FX_LOGS(DEBUG) << "Received fifo_depth_dur " << std::setw(5) << fifo_depth_duration_.to_usecs()
                   << " usec (" << (owner_->is_input() ? " Input" : "Output") << ") or "
                   << fifo_depth_frames_ << " frames (" << fifo_depth_bytes << " bytes)";

    // Figure out how many frames we need in our ring buffer.
    TimelineRate bytes_per_nanosecond(bytes_per_frame * frames_per_second, ZX_SEC(1));
    int64_t min_frames_64 = bytes_per_nanosecond.Scale(min_ring_buffer_duration_.to_nsecs());
    int64_t overhead = static_cast<int64_t>(fifo_depth_bytes) + bytes_per_frame - 1;
    bool overflow = ((min_frames_64 == TimelineRate::kOverflow) ||
                     (min_frames_64 > (std::numeric_limits<int64_t>::max() - overhead)));

    if (!overflow) {
      min_frames_64 += overhead;
      min_frames_64 /= bytes_per_frame;
      overflow = min_frames_64 > std::numeric_limits<uint32_t>::max();
    }

    if (overflow) {
      FX_LOGS(ERROR) << "Overflow while attempting to compute ring buffer size in frames.";
      FX_LOGS(ERROR) << "duration        : " << min_ring_buffer_duration_.get();
      FX_LOGS(ERROR) << "bytes per frame : " << bytes_per_frame;
      FX_LOGS(ERROR) << "frames per sec  : " << frames_per_second;
      FX_LOGS(ERROR) << "fifo depth      : " << fifo_depth_bytes;
      return;
    }

    state_ = State::Configuring_GettingRingBuffer;

    auto num_notifications_per_ring =
        ((clock_domain_ == fuchsia::hardware::audio::CLOCK_DOMAIN_MONOTONIC)) ? 0 : 2;
    ring_buffer_fidl_->GetVmo(
        static_cast<uint32_t>(min_frames_64), num_notifications_per_ring,
        [this](fuchsia::hardware::audio::RingBuffer_GetVmo_Result result) {
          OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
          {
            std::lock_guard<std::mutex> lock(ring_buffer_state_lock_);
            auto format = GetFormat();
            if (owner_->is_input()) {
              readable_ring_buffer_ = BaseRingBuffer::CreateReadableHardwareBuffer(
                  *format, versioned_ref_time_to_frac_presentation_frame_, reference_clock(),
                  std::move(result.response().ring_buffer), result.response().num_frames, [this]() {
                    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
                    auto t = reference_clock().Read();
                    return Fixed::FromRaw(ref_time_to_frac_safe_read_or_write_frame_.Apply(t.get()))
                        .Floor();
                  });
            } else {
              writable_ring_buffer_ = BaseRingBuffer::CreateWritableHardwareBuffer(
                  *format, versioned_ref_time_to_frac_presentation_frame_, reference_clock(),
                  std::move(result.response().ring_buffer), result.response().num_frames, [this]() {
                    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
                    auto t = reference_clock().Read();
                    return Fixed::FromRaw(ref_time_to_frac_safe_read_or_write_frame_.Apply(t.get()))
                        .Floor();
                  });
            }
            if (!readable_ring_buffer_ && !writable_ring_buffer_) {
              ShutdownSelf("Failed to allocate and map driver ring buffer", ZX_ERR_NO_MEMORY);
              return;
            }
            FX_DCHECK(!versioned_ref_time_to_frac_presentation_frame_->get().first.invertible());

            ring_buffer_size_bytes_ = format->bytes_per_frame() * result.response().num_frames;
            running_pos_bytes_ = 0;
            frac_frames_per_byte_ = TimelineRate(Fixed(1).raw_value(), format->bytes_per_frame());
          }

          // We are now Configured. Let our owner know about this important milestone.
          state_ = State::Configured;
          configuration_deadline_ = zx::time::infinite();
          SetupCommandTimeout();
          owner_->OnDriverConfigComplete();

          RequestNextPlugStateChange();

          if (clock_domain_ != AudioClock::kMonotonicDomain) {
            RequestNextClockRecoveryUpdate();
          }
        });
  });

  return ZX_OK;
}

void AudioDriver::RequestNextPlugStateChange() {
  stream_config_fidl_->WatchPlugState([this](fuchsia::hardware::audio::PlugState state) {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
    // Wardware reporting hardwired but notifies unplugged.
    if (pd_hardwired_ && !state.plugged()) {
      FX_LOGS(WARNING) << "Stream reports hardwired yet notifies unplugged, notifying as plugged";
      ReportPlugStateChange(true, zx::time(state.plug_state_time()));
      return;
    }
    ReportPlugStateChange(state.plugged(), zx::time(state.plug_state_time()));
    RequestNextPlugStateChange();
  });
}

// This position notification will be used to synthesize a clock for this audio device.
void AudioDriver::ClockRecoveryUpdate(fuchsia::hardware::audio::RingBufferPositionInfo info) {
  TRACE_DURATION("audio", "AudioDriver::ClockRecoveryUpdate");
  if (clock_domain_ == AudioClock::kMonotonicDomain) {
    return;
  }

  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());

  FX_CHECK(state_ == State::Started)
      << "ClockRecovery update while in state " << static_cast<uint32_t>(state_) << " -- should be "
      << static_cast<uint32_t>(State::Started);

  auto actual_mono_time = zx::time(info.timestamp);
  FX_CHECK(actual_mono_time >= mono_start_time_) << "Position notification while not started";

  // Based on (wraparound) ring positions, we maintain a long-running byte position
  auto prev_ring_position = running_pos_bytes_ % ring_buffer_size_bytes_;
  running_pos_bytes_ -= prev_ring_position;
  running_pos_bytes_ += info.position;
  // If previous position >= this new position, we must have wrapped around
  // The only exception: the first position notification (comparing to default initialized values)
  if (prev_ring_position >= info.position && actual_mono_time > mono_start_time_) {
    running_pos_bytes_ += ring_buffer_size_bytes_;
  }

  auto curr_pos_frac_frames = frac_frames_per_byte_.Scale(running_pos_bytes_);
  auto curr_ref_time = ref_time_to_frac_presentation_frame_.ApplyInverse(curr_pos_frac_frames);
  auto predicted_mono_time = audio_clock_->MonotonicTimeFromReferenceTime(zx::time(curr_ref_time));

  auto curr_error = predicted_mono_time - actual_mono_time;

  if constexpr (kPositionNotificationDisplayInterval > 0) {
    if (position_notification_count_ % kPositionNotificationDisplayInterval == 0) {
      FX_LOGS(INFO) << static_cast<void*>(this) << (owner_->is_output() ? " Output" : " Input ")
                    << " notification #" << position_notification_count_ << " [" << info.timestamp
                    << ", " << std::setw(6) << info.position << "] run_pos_bytes "
                    << running_pos_bytes_ << ", run_time "
                    << (actual_mono_time - mono_start_time_).get() << ", predicted_mono "
                    << predicted_mono_time.get() << ", curr_err " << curr_error.get();
    }
  }

  recovered_clock_->TuneForError(actual_mono_time, curr_error);

  // Maintain a running count of position notifications since START.
  ++position_notification_count_;

  RequestNextClockRecoveryUpdate();
}

void AudioDriver::RequestNextClockRecoveryUpdate() {
  FX_CHECK(clock_domain_ != AudioClock::kMonotonicDomain);

  ring_buffer_fidl_->WatchClockRecoveryPositionInfo(
      [this](fuchsia::hardware::audio::RingBufferPositionInfo info) { ClockRecoveryUpdate(info); });
}

zx_status_t AudioDriver::Start() {
  TRACE_DURATION("audio", "AudioDriver::Start");
  // TODO(fxbug.dev/13665): Figure out a better way to assert this!
  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());

  // In order to start, we must be in the Configured state.
  //
  // Note: Attempting to start while already started is considered an error because (since we are
  // already started) we will never deliver the OnDriverStartComplete callback. It would be
  // confusing to call it directly from here -- before the user's call to Start even returned.
  if (state_ != State::Configured) {
    FX_LOGS(ERROR) << "Bad state while attempting start (state = " << static_cast<uint32_t>(state_)
                   << ")";
    return ZX_ERR_BAD_STATE;
  }

  // Change state, setup our command timeout and we are finished.
  state_ = State::Starting;
  configuration_deadline_ = async::Now(owner_->mix_domain().dispatcher()) + kDefaultShortCmdTimeout;
  SetupCommandTimeout();

  ring_buffer_fidl_->Start([this](int64_t start_time) {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
    if (state_ != State::Starting) {
      FX_LOGS(ERROR) << "Received unexpected start response while in state "
                     << static_cast<uint32_t>(state_);
      return;
    }

    mono_start_time_ = zx::time(start_time);
    ref_start_time_ = reference_clock().ReferenceTimeFromMonotonicTime(mono_start_time_);

    auto format = GetFormat();
    auto frac_fps = TimelineRate(Fixed(format->frames_per_second()).raw_value(), zx::sec(1).get());

    if (owner_->is_output()) {
      // Abstractly, we can think of the hardware buffer as an infinitely
      // long sequence of frames, where the hardware maintains three pointers
      // into this sequence:
      //
      //        |<--- external delay --->|<--- FIFO depth --->|
      //      +-+------------------------+-+------------------+-+
      //  ... |P|                        |F|                  |W| ...
      //      +-+------------------------+-+------------------+-+
      //
      // At P, the frame is being presented to the speaker.
      // At F, the frame is at the head of the FIFO.
      // At W, the frame is about to be enqueued into the FIFO.
      //
      // At ref_start_time_, F points at frame 0. As time advances one frame,
      // each pointer shifts to the right by one frame. We define functions to
      // locate W and P at a given time T:
      //
      //   ref_pts_to_frame(T) = P
      //   ref_time_to_frac_safe_read_or_write_frame(T) = W
      //
      // W is the lowest-numbered frame that may be written to the hardware buffer,
      // aka the "first safe" write position.
      ref_time_to_frac_presentation_frame_ = TimelineFunction(
          0,                                          // first frame
          (ref_start_time_ + external_delay_).get(),  // first frame presented after external delay
          frac_fps                                    // fps in fractional frames
      );
      ref_time_to_frac_safe_read_or_write_frame_ = TimelineFunction(
          Fixed(fifo_depth_frames_).raw_value(),  // first safe frame is one FIFO depth after start
          ref_start_time_.get(),                  // start time
          frac_fps                                // fps in fractional frames
      );
    } else {
      // The capture buffer works in a similar way, with three analogous pointers:
      //
      //        |<--- FIFO depth --->|<--- external delay --->|
      //      +-+------------------+-+------------------------+-+
      //  ... |R|                  |F|                        |C| ...
      //      +-+------------------+-+------------------------+-+
      //
      // At C, the frame is being captured by the microphone.
      // At F, the frame is at the tail of the FIFO.
      // At R, the frame is just outside the FIFO.
      //
      // As above, F points at frame 0 at ref_start_time_, pointers shift to the right
      // as time advances, and we define functions to locate C and R:
      //
      //   ref_pts_to_frame(T) = C
      //   ref_time_to_frac_safe_read_or_write_frame(T) = R
      //
      // R is the highest-numbered frame that may be read from the hardware buffer,
      // aka the "last safe" read position.
      ref_time_to_frac_presentation_frame_ = TimelineFunction(
          0,                                          // first frame
          (ref_start_time_ - external_delay_).get(),  // first frame presented external delay ago
          frac_fps                                    // fps in fractional frames
      );
      ref_time_to_frac_safe_read_or_write_frame_ = TimelineFunction(
          -Fixed(fifo_depth_frames_).raw_value(),  // first safe frame is one FIFO before start
          ref_start_time_.get(),                   // start time
          frac_fps                                 // fps in fractional frames
      );
    }

    versioned_ref_time_to_frac_presentation_frame_->Update(ref_time_to_frac_presentation_frame_);
    if (clock_domain_ != AudioClock::kMonotonicDomain) {
      recovered_clock_->ResetRateAdjustment(mono_start_time_);
    }

    // We are now Started. Let our owner know about this important milestone.
    state_ = State::Started;
    configuration_deadline_ = zx::time::infinite();
    SetupCommandTimeout();
    owner_->OnDriverStartComplete();
  });
  SetupCommandTimeout();
  return ZX_OK;
}

zx_status_t AudioDriver::Stop() {
  TRACE_DURATION("audio", "AudioDriver::Stop");
  // TODO(fxbug.dev/13665): Figure out a better way to assert this!
  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());

  // In order to stop, we must be in the Started state.
  // TODO(fxbug.dev/13668): make Stop idempotent. Allow Stop when Configured/Stopping; disallow if
  // Shutdown; consider what to do if Uninitialized/MissingDriverInfo/Unconfigured/Configuring. Most
  // importantly, if driver is Starting, queue the request until Start completes (as we cannot
  // cancel driver commands). Finally, handle multiple Stop calls to be in-flight concurrently.
  if (state_ != State::Started) {
    FX_LOGS(ERROR) << "Bad state while attempting stop (state = " << static_cast<uint32_t>(state_)
                   << ")";
    return ZX_ERR_BAD_STATE;
  }

  // Invalidate our timeline transformation here. To outside observers, we are now stopped.
  versioned_ref_time_to_frac_presentation_frame_->Update({});

  // We are now in the Stopping state.
  state_ = State::Stopping;
  configuration_deadline_ = async::Now(owner_->mix_domain().dispatcher()) + kDefaultShortCmdTimeout;
  SetupCommandTimeout();

  ring_buffer_fidl_->Stop([this]() {
    OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());
    // We are now stopped and in Configured state. Let our owner know about this important
    // milestone.
    state_ = State::Configured;
    configuration_deadline_ = zx::time::infinite();
    SetupCommandTimeout();
    owner_->OnDriverStopComplete();
  });

  return ZX_OK;
}

zx_status_t AudioDriver::SetPlugDetectEnabled(bool enabled) {
  TRACE_DURATION("audio", "AudioDriver::SetPlugDetectEnabled");

  // This method is a no-op since under the FIDL API plug detect is always enabled if supported.
  return ZX_OK;
}

void AudioDriver::ShutdownSelf(const char* reason, zx_status_t status) {
  TRACE_DURATION("audio", "AudioDriver::ShutdownSelf");
  if (state_ == State::Shutdown) {
    return;
  }

  // Always log: this should occur rarely, hence it should not spam.
  FX_PLOGS(INFO, status) << (owner_->is_input() ? " Input" : "Output") << " shutting down '"
                         << reason << "'";

  // Our owner will call our Cleanup function within this call.
  owner_->ShutdownSelf();
  state_ = State::Shutdown;
}

void AudioDriver::SetupCommandTimeout() {
  TRACE_DURATION("audio", "AudioDriver::SetupCommandTimeout");

  // If we have received a late response, report it now.
  if (driver_last_timeout_ != zx::time::infinite()) {
    auto delay = async::Now(owner_->mix_domain().dispatcher()) - driver_last_timeout_;
    driver_last_timeout_ = zx::time::infinite();
    FX_DCHECK(timeout_handler_);
    timeout_handler_(delay);
  }

  zx::time deadline;

  deadline = fetch_driver_info_deadline_;
  deadline = std::min(deadline, configuration_deadline_);

  if (cmd_timeout_.last_deadline() != deadline) {
    if (deadline != zx::time::infinite()) {
      cmd_timeout_.PostForTime(owner_->mix_domain().dispatcher(), deadline);
    } else {
      cmd_timeout_.Cancel();
    }
  }
}

void AudioDriver::ReportPlugStateChange(bool plugged, zx::time plug_time) {
  TRACE_DURATION("audio", "AudioDriver::ReportPlugStateChange");
  {
    std::lock_guard<std::mutex> lock(plugged_lock_);
    plugged_ = plugged;
    plug_time_ = plug_time;
  }

  // Under the FIDL API plug detect is always enabled.
  owner_->OnDriverPlugStateChange(plugged, plug_time);
}

zx_status_t AudioDriver::OnDriverInfoFetched(uint32_t info) {
  TRACE_DURATION("audio", "AudioDriver::OnDriverInfoFetched");
  // We should never fetch the same info twice.
  if (fetched_driver_info_ & info) {
    ShutdownSelf("Duplicate driver info fetch\n", ZX_ERR_BAD_STATE);
    return ZX_ERR_BAD_STATE;
  }

  // Record the new piece of info we just fetched.
  FX_DCHECK(state_ == State::MissingDriverInfo);
  fetched_driver_info_ |= info;

  // Have we finished fetching our initial driver info? If so, cancel the timeout, transition to
  // Unconfigured state, and let our owner know that we have finished.
  if ((fetched_driver_info_ & kDriverInfoHasAll) == kDriverInfoHasAll) {
    // Now that we have our clock domain, we can establish our audio device clock
    SetUpClocks();

    // We are done. Clear the fetch driver info timeout and let our owner know.
    fetch_driver_info_deadline_ = zx::time::infinite();
    state_ = State::Unconfigured;
    SetupCommandTimeout();
    owner_->OnDriverInfoFetched();
  }

  return ZX_OK;
}

void AudioDriver::SetUpClocks() {
  if (clock_domain_ == AudioClock::kMonotonicDomain) {
    // If in the monotonic domain, we'll fall back to a non-adjustable clone of CLOCK_MONOTONIC.
    audio_clock_ = owner_->clock_factory()->CreateDeviceFixed(audio::clock::CloneOfMonotonic(),
                                                              AudioClock::kMonotonicDomain);
    return;
  }

  // This clock begins as a clone of MONOTONIC, but because the hardware is NOT in the monotonic
  // clock domain, this clock must eventually diverge. We tune this clock based on notifications
  // provided by the audio driver, which correlate DMA position with CLOCK_MONOTONIC time.
  // TODO(fxbug.dev/60027): Recovered clocks should be per-domain not per-driver.
  auto adjustable_clock = audio::clock::AdjustableCloneOfMonotonic();
  recovered_clock_ =
      owner_->clock_factory()->CreateDeviceAdjustable(std::move(adjustable_clock), clock_domain_);

  auto read_only_clock_result = recovered_clock_->DuplicateClockReadOnly();
  if (read_only_clock_result.is_error()) {
    FX_LOGS(ERROR) << "DuplicateClockReadOnly failed, will not recover a device clock!";
    return;
  }

  // TODO(fxbug.dev/46648): If this clock domain is discovered to be hardware-tunable, this should
  // be DeviceAdjustable, not DeviceFixed, to articulate that it has hardware controls.
  auto clone = owner_->clock_factory()->CreateDeviceFixed(read_only_clock_result.take_value(),
                                                          clock_domain_);

  audio_clock_ = std::move(clone);
}

zx_status_t AudioDriver::SetGain(const AudioDeviceSettings::GainState& gain_state,
                                 audio_set_gain_flags_t set_flags) {
  // We ignore set_flags since the FIDL API requires updates to all field of
  // fuchsia::hardware::audio::GainState.
  return SetGain(gain_state);
}

zx_status_t AudioDriver::SetGain(const AudioDeviceSettings::GainState& gain_state) {
  TRACE_DURATION("audio", "AudioDriver::SetGain");

  fuchsia::hardware::audio::GainState gain_state2 = {};
  if (gain_state.muted) {
    gain_state2.set_muted(true);
  }
  if (gain_state.agc_enabled) {
    gain_state2.set_agc_enabled(true);
  }
  gain_state2.set_gain_db(gain_state.gain_db);
  stream_config_fidl_->SetGain(std::move(gain_state2));
  return ZX_OK;
}

zx_status_t AudioDriver::SelectBestFormat(uint32_t* frames_per_second_inout,
                                          uint32_t* channels_inout,
                                          fuchsia::media::AudioSampleFormat* sample_format_inout) {
  return media::audio::SelectBestFormat(formats_, frames_per_second_inout, channels_inout,
                                        sample_format_inout);
}

void AudioDriver::DriverCommandTimedOut() {
  FX_LOGS(WARNING) << "Unexpected driver timeout";
  driver_last_timeout_ = async::Now(owner_->mix_domain().dispatcher());
}

zx_status_t AudioDriver::SetActiveChannels(uint64_t chan_bit_mask) {
  OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());

  if (state_ != State::Started) {
    FX_LOGS(ERROR) << "Unexpected SetActiveChannels request while in state "
                   << static_cast<uint32_t>(state_);
    return ZX_ERR_BAD_STATE;
  }

  if (set_active_channels_err_ != ZX_OK) {
    if constexpr (IdlePolicy::kLogSetActiveChannelsCalls) {
      FX_LOGS(INFO) << "ring_buffer_fidl->SetActiveChannels(0x" << std::hex << chan_bit_mask
                    << ") NOT called by AudioDriver because of previous set_active_channels_err_ "
                    << std::dec << set_active_channels_err_;
    }
    return set_active_channels_err_;
  }

  if constexpr (IdlePolicy::kLogSetActiveChannelsCalls) {
    FX_LOGS(INFO) << "ring_buffer_fidl->SetActiveChannels(0x" << std::hex << chan_bit_mask
                  << ") called by AudioDriver";
  }

  // We choose not to use any watchdog timer for this command. If the driver works with other
  // methods but not this one, then it will by default keep all channels active.

  ring_buffer_fidl_->SetActiveChannels(
      chan_bit_mask,
      [this, chan_bit_mask](fuchsia::hardware::audio::RingBuffer_SetActiveChannels_Result result) {
        OBTAIN_EXECUTION_DOMAIN_TOKEN(token, &owner_->mix_domain());

        if (result.is_err()) {
          set_active_channels_err_ = result.err();
          FX_LOGS(WARNING) << "ring_buffer_fidl->SetActiveChannels(0x" << std::hex << chan_bit_mask
                           << ") received error " << std::dec << set_active_channels_err_;
          return;
        }
        int64_t set_active_channels_time = result.response().set_time;

        if constexpr (IdlePolicy::kLogSetActiveChannelsCalls) {
          FX_LOGS(INFO) << "ring_buffer_fidl->SetActiveChannels(0x" << std::hex << chan_bit_mask
                        << ") received callback with set_time " << std::dec
                        << set_active_channels_time;
        } else {
          (void)chan_bit_mask;  // avoid "unused lambda capture" compiler complaint
        }

        // TODO(fxbug.dev/82423): assuming this might change the clients' minimum lead time, here we
        // should potentially kick off a notification -- including the set_active_channels_time.
      });

  return ZX_OK;
}

}  // namespace media::audio
