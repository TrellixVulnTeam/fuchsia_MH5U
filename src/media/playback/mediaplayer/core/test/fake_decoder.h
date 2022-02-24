// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_CORE_TEST_FAKE_DECODER_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_CORE_TEST_FAKE_DECODER_H_

#include "src/media/playback/mediaplayer/process/processor.h"

namespace media_player {
namespace test {

class FakeDecoder : public Processor {
 public:
  static std::unique_ptr<StreamType> OutputStreamType(const StreamType& stream_type);

  FakeDecoder(const StreamType& stream_type) : output_stream_type_(OutputStreamType(stream_type)) {}

  ~FakeDecoder() override {}

  const char* label() const override { return "FakeDecoder"; }

  // Processor implementation.
  void ConfigureConnectors() override {
    ConfigureInputToUseLocalMemory(1,    // max_aggregate_payload_size
                                   0);   // max_payload_count
    ConfigureOutputToUseLocalMemory(1,   // max_aggregate_payload_size
                                    0,   // max_payload_count
                                    0);  // max_payload_size
  }

  void FlushInput(bool hold_frame, size_t input_index, fit::closure callback) override {
    callback();
  }

  void FlushOutput(size_t output_index, fit::closure callback) override { callback(); }

  void PutInputPacket(PacketPtr packet, size_t input_index) override { RequestInputPacket(); }

  void RequestOutputPacket() override {}

  void SetInputStreamType(const StreamType& stream_type) override {}

  std::unique_ptr<StreamType> output_stream_type() const override {
    FX_DCHECK(output_stream_type_);
    return output_stream_type_->Clone();
  }

 private:
  std::unique_ptr<StreamType> output_stream_type_;
};

class FakeDecoderFactory : public DecoderFactory {
 public:
  FakeDecoderFactory();

  ~FakeDecoderFactory() override;

  void CreateDecoder(const StreamType& stream_type,
                     fit::function<void(std::shared_ptr<Processor>)> callback) override;
};

}  // namespace test
}  // namespace media_player

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_CORE_TEST_FAKE_DECODER_H_
