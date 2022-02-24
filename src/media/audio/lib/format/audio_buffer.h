// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_LIB_FORMAT_AUDIO_BUFFER_H_
#define SRC_MEDIA_AUDIO_LIB_FORMAT_AUDIO_BUFFER_H_

#include <fuchsia/media/cpp/fidl.h>
#include <lib/stdcompat/bit.h>
#include <lib/syslog/cpp/macros.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include <fbl/algorithm.h>

#include "src/lib/fxl/strings/string_printf.h"
#include "src/media/audio/lib/format/constants.h"
#include "src/media/audio/lib/format/format.h"
#include "src/media/audio/lib/format/traits.h"

namespace media::audio {

template <fuchsia::media::AudioSampleFormat SampleFormat>
class AudioBufferSlice;

// A buffer of audio data. Each entry in the vector is a single sample.
template <fuchsia::media::AudioSampleFormat SampleFormat>
class AudioBuffer {
 public:
  using SampleT = typename SampleFormatTraits<SampleFormat>::SampleT;

  // Create an interleaved AudioBuffer, from a vector of 1-channel AudioBufferSlices
  static AudioBuffer Interleave(const std::vector<AudioBufferSlice<SampleFormat>>& channel_slices) {
    FX_CHECK(channel_slices.size());
    auto format = Format::Create<SampleFormat>(static_cast<int32_t>(channel_slices.size()),
                                               channel_slices[0].format().frames_per_second())
                      .take_value();
    auto buffer = AudioBuffer<SampleFormat>(format, channel_slices[0].NumFrames());
    auto buffer_fps = buffer.format().frames_per_second();
    auto buffer_frames = buffer.NumFrames();

    // Write out the interleaved buffer, one channel at a time
    for (auto chan = 0u; chan < channel_slices.size(); ++chan) {
      auto slice = channel_slices[chan];
      FX_CHECK(slice.format().channels() == 1);
      FX_CHECK(slice.format().frames_per_second() == buffer_fps);
      FX_CHECK(slice.NumFrames() == buffer_frames);

      for (auto frame = 0u; frame < buffer_frames; ++frame) {
        buffer.samples()[buffer.SampleIndex(frame, chan)] = slice.SampleAt(frame, 0);
      }
    }
    return buffer;
  }

  AudioBuffer(const Format& f, int64_t num_frames)
      : format_(Format::Create<SampleFormat>(f.channels(), f.frames_per_second()).take_value()),
        samples_(num_frames * f.channels()) {
    FX_CHECK(SampleFormat == f.sample_format());
  }

  AudioBuffer(const TypedFormat<SampleFormat>& f, int64_t num_frames)
      : format_(f), samples_(num_frames * f.channels()) {
    FX_CHECK(SampleFormat == f.sample_format());
  }

  const TypedFormat<SampleFormat>& format() const { return format_; }
  const std::vector<SampleT>& samples() const { return samples_; }
  std::vector<SampleT>& samples() { return samples_; }

  int64_t NumSamples() const { return samples_.size(); }
  int64_t NumFrames() const { return samples_.size() / format_.channels(); }
  int64_t NumBytes() const { return NumFrames() * format_.bytes_per_frame(); }
  int64_t SampleIndex(int64_t frame, int32_t chan) const {
    return frame * format_.channels() + chan;
  }
  SampleT SampleAt(int64_t frame, int32_t chan) const { return samples_[SampleIndex(frame, chan)]; }

  void Append(const AudioBufferSlice<SampleFormat>& slice_to_append) {
    FX_CHECK(format() == slice_to_append.format());

    samples_.insert(samples_.end(), slice_to_append.begin(), slice_to_append.end());
  }

  // For debugging, display a given range of frames in aligned columns. Column width is a power-of-2
  // based on sample width and number of channels. For row 0, display space until the first frame.
  void Display(int64_t start_frame, int64_t end_frame, std::string tag = "") const {
    start_frame = std::clamp(start_frame, 0l, NumFrames());
    end_frame = std::clamp(end_frame, start_frame, NumFrames());

    if (tag.size()) {
      printf("%s\n", tag.c_str());
    }
    printf("  Frames %zd to %zd:", start_frame, end_frame);

    // Frames that fit in a 200-char row (11 for row label, 1 between samps, +1 between frames)...
    size_t frames_per_row =
        (200 - 11) /
        ((format_.channels() * (SampleFormatTraits<SampleFormat>::kCharsPerSample + 1)) + 1);
    // ...rounded _down_ to the closest power-of-2, for quick visual scanning.
    frames_per_row = cpp20::bit_ceil(frames_per_row + 1) / 2;

    for (auto frame = static_cast<int64_t>(
             fbl::round_down(static_cast<size_t>(start_frame), frames_per_row));
         frame < end_frame; ++frame) {
      if (frame % frames_per_row == 0) {
        printf("\n  [%6lu] ", frame);
      } else {
        printf(" ");
      }

      for (auto chan = 0; chan < format_.channels(); ++chan) {
        auto offset = frame * format_.channels() + chan;
        if (frame >= start_frame) {
          printf(" %s", SampleFormatTraits<SampleFormat>::ToString(samples_[offset]).c_str());
        } else {
          printf(" %*s", static_cast<int>(SampleFormatTraits<SampleFormat>::kCharsPerSample), " ");
        }
      }
    }
    printf("\n");
  }

 private:
  friend class AudioBufferSlice<SampleFormat>;

  TypedFormat<SampleFormat> format_;
  std::vector<SampleT> samples_;
};

// A slice of an AudioBuffer.
// Maintains (but does not own) a pointer to the parent AudioBuffer.
template <fuchsia::media::AudioSampleFormat SampleFormat>
class AudioBufferSlice {
 public:
  using SampleT = typename SampleFormatTraits<SampleFormat>::SampleT;

  AudioBufferSlice() : buf_(nullptr), start_frame_(0), end_frame_(0) {}

  AudioBufferSlice(const AudioBuffer<SampleFormat>* b)
      : buf_(b), start_frame_(0), end_frame_(b->NumFrames()) {}

  AudioBufferSlice(const AudioBuffer<SampleFormat>* b, int64_t start, int64_t end)
      : buf_(b),
        start_frame_(std::min(start, b->NumFrames())),
        end_frame_(std::min(end, b->NumFrames())) {
    FX_CHECK(start <= end) << "start=" << start << ", end=" << end;
  }

  const AudioBuffer<SampleFormat>* buf() const { return buf_; }
  const TypedFormat<SampleFormat>& format() const {
    FX_CHECK(buf_);
    return buf_->format();
  }
  int64_t start_frame() const { return start_frame_; }
  int64_t end_frame() const { return end_frame_; }
  bool empty() const { return !buf_ || start_frame_ == end_frame_; }

  typename std::vector<SampleT>::const_iterator begin() const {
    return buf_->samples().begin() + start_frame_ * format().channels();
  }
  typename std::vector<SampleT>::const_iterator end() const {
    return buf_->samples().begin() + end_frame_ * format().channels();
  }

  int64_t NumFrames() const { return end_frame_ - start_frame_; }
  int64_t NumSamples() const { return NumFrames() * format().channels(); }
  int64_t NumBytes() const { return NumFrames() * format().bytes_per_frame(); }
  int64_t SampleIndex(int64_t frame, int32_t chan) const {
    FX_CHECK(buf_);
    return buf_->SampleIndex(start_frame_ + frame, chan);
  }
  SampleT SampleAt(int64_t frame, int32_t chan) const {
    FX_CHECK(buf_);
    return buf_->SampleAt(start_frame_ + frame, chan);
  }

  // Return a subslice of this slice.
  AudioBufferSlice<SampleFormat> Subslice(int64_t slice_start, int64_t slice_end) const {
    return AudioBufferSlice<SampleFormat>(buf_, start_frame_ + slice_start,
                                          start_frame_ + slice_end);
  }

  // Return a buffer containing the given channel only.
  AudioBuffer<SampleFormat> GetChannel(int32_t chan) const {
    auto new_format =
        Format::Create({
                           .sample_format = SampleFormat,
                           .channels = 1,
                           .frames_per_second = static_cast<uint32_t>(format().frames_per_second()),
                       })
            .take_value();
    AudioBuffer<SampleFormat> out(new_format, NumFrames());
    for (int64_t frame = 0; frame < NumFrames(); frame++) {
      out.samples()[frame] = SampleAt(frame, chan);
    }
    return out;
  }

  // Return a buffer that contains a clone of this slice.
  AudioBuffer<SampleFormat> Clone() const {
    AudioBuffer<SampleFormat> out(format(), NumFrames());
    for (int64_t frame = 0; frame < NumFrames(); frame++) {
      for (int32_t chan = 0; chan < format().channels(); chan++) {
        out.samples()[out.SampleIndex(frame, chan)] = SampleAt(frame, chan);
      }
    }
    return out;
  }

 private:
  const AudioBuffer<SampleFormat>* buf_;
  int64_t start_frame_;
  int64_t end_frame_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_LIB_FORMAT_AUDIO_BUFFER_H_
