// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FIDL_FIDL_TYPE_CONVERSIONS_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FIDL_FIDL_TYPE_CONVERSIONS_H_

#include <fuchsia/media/cpp/fidl.h>
#include <fuchsia/media/playback/cpp/fidl.h>
#include <fuchsia/mediacodec/cpp/fidl.h>

#include <cstdint>

#include "lib/fidl/cpp/type_converter.h"
#include "src/lib/fsl/types/type_converters.h"
#include "src/media/playback/mediaplayer/graph/metadata.h"
#include "src/media/playback/mediaplayer/graph/result.h"
#include "src/media/playback/mediaplayer/graph/types/audio_stream_type.h"
#include "src/media/playback/mediaplayer/graph/types/stream_type.h"
#include "src/media/playback/mediaplayer/graph/types/video_stream_type.h"

namespace fidl {

template <>
struct TypeConverter<media_player::AudioStreamType::SampleFormat,
                     fuchsia::media::AudioSampleFormat> {
  static media_player::AudioStreamType::SampleFormat Convert(
      fuchsia::media::AudioSampleFormat audio_sample_format);
};

template <>
struct TypeConverter<media_player::VideoStreamType::PixelFormat, fuchsia::images::PixelFormat> {
  static media_player::VideoStreamType::PixelFormat Convert(
      fuchsia::images::PixelFormat pixel_format);
};

template <>
struct TypeConverter<media_player::VideoStreamType::ColorSpace, fuchsia::media::ColorSpace> {
  static media_player::VideoStreamType::ColorSpace Convert(fuchsia::media::ColorSpace color_space);
};

template <>
struct TypeConverter<fuchsia::media::AudioSampleFormat,
                     media_player::AudioStreamType::SampleFormat> {
  static fuchsia::media::AudioSampleFormat Convert(
      media_player::AudioStreamType::SampleFormat sample_format);
};

template <>
struct TypeConverter<fuchsia::images::PixelFormat, media_player::VideoStreamType::PixelFormat> {
  static fuchsia::images::PixelFormat Convert(
      media_player::VideoStreamType::PixelFormat pixel_format);
};

template <>
struct TypeConverter<fuchsia::media::ColorSpace, media_player::VideoStreamType::ColorSpace> {
  static fuchsia::media::ColorSpace Convert(media_player::VideoStreamType::ColorSpace color_space);
};

template <>
struct TypeConverter<fuchsia::sysmem::PixelFormat, media_player::VideoStreamType::PixelFormat> {
  static fuchsia::sysmem::PixelFormat Convert(
      media_player::VideoStreamType::PixelFormat pixel_format);
};

template <>
struct TypeConverter<fuchsia::sysmem::ColorSpace, media_player::VideoStreamType::ColorSpace> {
  static fuchsia::sysmem::ColorSpace Convert(media_player::VideoStreamType::ColorSpace color_space);
};

template <>
struct TypeConverter<fuchsia::media::StreamType, media_player::StreamType> {
  static fuchsia::media::StreamType Convert(const media_player::StreamType& input);
};

template <>
struct TypeConverter<std::unique_ptr<media_player::StreamType>, fuchsia::media::StreamType> {
  static std::unique_ptr<media_player::StreamType> Convert(const fuchsia::media::StreamType& input);
};

template <>
struct TypeConverter<fuchsia::sysmem::ImageFormat_2, media_player::VideoStreamType> {
  static fuchsia::sysmem::ImageFormat_2 Convert(const media_player::VideoStreamType& input);
};

template <>
struct TypeConverter<fuchsia::media::Metadata, media_player::Metadata> {
  static fuchsia::media::Metadata Convert(const media_player::Metadata& input);
};

template <>
struct TypeConverter<media_player::Metadata, fuchsia::media::Metadata> {
  static media_player::Metadata Convert(const fuchsia::media::Metadata& input);
};

template <>
struct TypeConverter<VectorPtr<uint8_t>, const media_player::Bytes*> {
  static VectorPtr<uint8_t> Convert(const media_player::Bytes* input);
};

template <>
struct TypeConverter<std::unique_ptr<media_player::Bytes>, VectorPtr<uint8_t>> {
  static std::unique_ptr<media_player::Bytes> Convert(const VectorPtr<uint8_t>& input);
};

template <>
struct TypeConverter<fuchsia::media::FormatDetailsPtr, media_player::StreamType> {
  static fuchsia::media::FormatDetailsPtr Convert(const media_player::StreamType& input);
};

template <>
struct TypeConverter<std::unique_ptr<media_player::StreamType>, fuchsia::media::FormatDetails> {
  static std::unique_ptr<media_player::StreamType> Convert(
      const fuchsia::media::FormatDetails& input);
};

// Generic |std::unique_ptr| to |std::unique_ptr| conversion.
template <typename T, typename U>
struct TypeConverter<std::unique_ptr<T>, std::unique_ptr<U>> {
  static std::unique_ptr<T> Convert(const std::unique_ptr<U>& input) {
    if (!input) {
      return nullptr;
    }

    return std::make_unique<T>(To<T>(*input));
  }
};

}  // namespace fidl

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FIDL_FIDL_TYPE_CONVERSIONS_H_
