// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FFMPEG_AV_FRAME_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FFMPEG_AV_FRAME_H_

#include <memory>

extern "C" {
#include "libavutil/frame.h"
}

namespace media_player {
namespace ffmpeg {

struct AVFrameDeleter {
  inline void operator()(AVFrame* ptr) const { av_frame_free(&ptr); }
};

using AvFramePtr = ::std::unique_ptr<AVFrame, AVFrameDeleter>;

struct AvFrame {
  static AvFramePtr Create() { return AvFramePtr(av_frame_alloc()); }
};

}  // namespace ffmpeg
}  // namespace media_player

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FFMPEG_AV_FRAME_H_
