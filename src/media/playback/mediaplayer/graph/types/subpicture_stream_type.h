// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_GRAPH_TYPES_SUBPICTURE_STREAM_TYPE_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_GRAPH_TYPES_SUBPICTURE_STREAM_TYPE_H_

#include <memory>

#include "src/media/playback/mediaplayer/graph/types/stream_type.h"

namespace media_player {

// Describes the type of a subpicture stream.
class SubpictureStreamType : public StreamType {
 public:
  static std::unique_ptr<StreamType> Create(std::unique_ptr<Bytes> encryption_parameters,
                                            const std::string& encoding,
                                            std::unique_ptr<Bytes> encoding_parameters) {
    return std::unique_ptr<StreamType>(new SubpictureStreamType(
        std::move(encryption_parameters), encoding, std::move(encoding_parameters)));
  }

  SubpictureStreamType(std::unique_ptr<Bytes> encryption_parameters, const std::string& encoding,
                       std::unique_ptr<Bytes> encoding_parameters);

  ~SubpictureStreamType() override;

  const SubpictureStreamType* subpicture() const override;

  std::unique_ptr<StreamType> Clone() const override;
};

// Describes a set of subpicture stream types.
class SubpictureStreamTypeSet : public StreamTypeSet {
 public:
  static std::unique_ptr<StreamTypeSet> Create(const std::vector<std::string>& encodings) {
    return std::unique_ptr<StreamTypeSet>(new SubpictureStreamTypeSet(encodings));
  }

  SubpictureStreamTypeSet(const std::vector<std::string>& encodings);

  ~SubpictureStreamTypeSet() override;

  const SubpictureStreamTypeSet* subpicture() const override;

  std::unique_ptr<StreamTypeSet> Clone() const override;
};

}  // namespace media_player

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_GRAPH_TYPES_SUBPICTURE_STREAM_TYPE_H_
