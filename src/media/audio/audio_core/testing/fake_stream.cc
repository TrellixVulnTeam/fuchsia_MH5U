// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/testing/fake_stream.h"

#include <lib/syslog/cpp/macros.h>

namespace media::audio::testing {

FakeStream::FakeStream(const Format& format, std::shared_ptr<AudioClockFactory> clock_factory,
                       size_t max_buffer_size, zx::clock clock)
    : ReadableStream(format), audio_clock_(clock_factory->CreateClientFixed(std::move(clock))) {
  if (max_buffer_size == 0) {
    max_buffer_size = zx_system_get_page_size();
  }
  buffer_size_ = max_buffer_size;
  buffer_ = std::make_unique<uint8_t[]>(buffer_size_);
  memset(buffer_.get(), 0, buffer_size_);
}

std::optional<ReadableStream::Buffer> FakeStream::ReadLock(ReadLockContext& ctx, Fixed frame,
                                                           int64_t frame_count) {
  FX_CHECK(static_cast<uint64_t>(frame_count * format().bytes_per_frame()) < buffer_size_);
  return std::make_optional<ReadableStream::Buffer>(frame, frame_count, buffer_.get(), true,
                                                    usage_mask_, gain_db_);
}

ReadableStream::TimelineFunctionSnapshot FakeStream::ref_time_to_frac_presentation_frame() const {
  auto [timeline_function, generation] = timeline_function_->get();
  return {
      .timeline_function = timeline_function,
      .generation = generation,
  };
}

}  // namespace media::audio::testing
