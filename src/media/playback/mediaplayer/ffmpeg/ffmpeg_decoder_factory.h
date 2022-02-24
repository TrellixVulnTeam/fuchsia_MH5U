// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FFMPEG_FFMPEG_DECODER_FACTORY_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FFMPEG_FFMPEG_DECODER_FACTORY_H_

#include <memory>

#include "src/media/playback/mediaplayer/process/processor.h"

namespace media_player {

// Factory for ffmpeg decoders.
class FfmpegDecoderFactory : public DecoderFactory {
 public:
  // Creates an ffmmpeg decoder factory.
  static std::unique_ptr<DecoderFactory> Create(ServiceProvider* service_provider);

  FfmpegDecoderFactory();

  ~FfmpegDecoderFactory() override;

  void CreateDecoder(const StreamType& stream_type,
                     fit::function<void(std::shared_ptr<Processor>)> callback) override;
};

}  // namespace media_player

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FFMPEG_FFMPEG_DECODER_FACTORY_H_
