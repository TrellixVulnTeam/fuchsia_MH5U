// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/ffmpeg/ffmpeg_decoder_factory.h"

#include <lib/syslog/cpp/macros.h>

#include "src/media/playback/mediaplayer/ffmpeg/av_codec_context.h"
#include "src/media/playback/mediaplayer/ffmpeg/ffmpeg_audio_decoder.h"
#include "src/media/playback/mediaplayer/ffmpeg/ffmpeg_video_decoder.h"

namespace media_player {

// static
std::unique_ptr<DecoderFactory> FfmpegDecoderFactory::Create(ServiceProvider* service_provider) {
  return std::make_unique<FfmpegDecoderFactory>();
}

FfmpegDecoderFactory::FfmpegDecoderFactory() {}

FfmpegDecoderFactory::~FfmpegDecoderFactory() {}

void FfmpegDecoderFactory::CreateDecoder(const StreamType& stream_type,
                                         fit::function<void(std::shared_ptr<Processor>)> callback) {
  FX_DCHECK(callback);

  AvCodecContextPtr av_codec_context(AvCodecContext::Create(stream_type));
  if (!av_codec_context) {
    FX_LOGS(ERROR) << "couldn't create codec context";
    callback(nullptr);
    return;
  }

  const AVCodec* ffmpeg_decoder = avcodec_find_decoder(av_codec_context->codec_id);
  if (ffmpeg_decoder == nullptr) {
    // Couldn't find a decoder.
    callback(nullptr);
    return;
  }

  int r = avcodec_open2(av_codec_context.get(), ffmpeg_decoder, nullptr);
  if (r < 0) {
    FX_LOGS(ERROR) << "couldn't open the decoder " << r;
    callback(nullptr);
    return;
  }

  switch (av_codec_context->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
      callback(FfmpegAudioDecoder::Create(std::move(av_codec_context)));
      break;
    case AVMEDIA_TYPE_VIDEO:
      callback(FfmpegVideoDecoder::Create(std::move(av_codec_context)));
      break;
    default:
      FX_LOGS(ERROR) << "unsupported codec type " << av_codec_context->codec_type;
      callback(nullptr);
      break;
  }
}

}  // namespace media_player
