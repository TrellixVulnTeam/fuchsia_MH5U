// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chunk_input_stream.h"

#include <algorithm>

ChunkInputStream::ChunkInputStream(size_t chunk_size,
                                   TimestampExtrapolator&& timestamp_extrapolator,
                                   InputBlockProcessor&& input_block_processor)
    : chunk_size_(chunk_size),
      timestamp_extrapolator_(std::move(timestamp_extrapolator)),
      input_block_processor_(std::move(input_block_processor)) {
  ZX_DEBUG_ASSERT_MSG(chunk_size_ != 0, "A chunk size of zero will never make progress.");
  ZX_DEBUG_ASSERT(input_block_processor_);
  scratch_block_.data.resize(chunk_size_);
}

ChunkInputStream::Status ChunkInputStream::ProcessInputPacket(
    const CodecPacket* input_codec_packet) {
  ZX_DEBUG_ASSERT(input_codec_packet);
  ZX_DEBUG_ASSERT_MSG(!early_terminated_, "This stream was terminated by the user.");

  if (input_codec_packet->has_timestamp_ish()) {
    timestamp_extrapolator_.Inform(BytesSeen(), input_codec_packet->timestamp_ish());
  }

  InputPacket input_packet = {.packet = input_codec_packet};
  if (!scratch_block_.empty()) {
    AppendToScratchBlock(&input_packet);
  }

  if (scratch_block_.full()) {
    Status status;
    if ((status = EmitBlock(scratch_block_.data.data(), chunk_size_)) != kOk) {
      return status;
    }

    scratch_block_.len = 0;
  }

  if (input_packet.bytes_unread() > 0) {
    ZX_DEBUG_ASSERT_MSG(!next_output_timestamp_,
                        "Any stashed timestamp should have been used when "
                        "emitting the scratch block.");

    auto [timestamp, status] = ExtrapolateTimestamp();
    if (status != kOk) {
      return status;
    }
    next_output_timestamp_ = timestamp;
  }

  while (input_packet.bytes_unread() >= chunk_size_) {
    Status status;
    if ((status = EmitBlock(input_packet.data_at_offset(), chunk_size_)) != kOk) {
      return status;
    }

    input_packet.offset += chunk_size_;
  }

  AppendToScratchBlock(&input_packet);
  ZX_DEBUG_ASSERT_MSG(input_packet.bytes_unread() == 0,
                      "We should leave no bytes unread in the input packet.");

  return kOk;
}

ChunkInputStream::Status ChunkInputStream::Flush() {
  ZX_DEBUG_ASSERT_MSG(!early_terminated_, "This stream was terminated by the user.");

  if (scratch_block_.empty_bytes_left() > 0) {
    memset(scratch_block_.empty_start(), 0, scratch_block_.empty_bytes_left());
  }

  auto result = EmitBlock(scratch_block_.data.data(), scratch_block_.len,
                          /*is_end_of_stream=*/true);
  scratch_block_.len = 0;
  return result;
}

void ChunkInputStream::AppendToScratchBlock(InputPacket* input_packet) {
  ZX_DEBUG_ASSERT(input_packet);
  const size_t n = std::min(input_packet->bytes_unread(), scratch_block_.empty_bytes_left());
  if (n == 0) {
    return;
  }

  memcpy(scratch_block_.empty_start(), input_packet->data_at_offset(), n);
  input_packet->offset += n;
  scratch_block_.len += n;
}

ChunkInputStream::Status ChunkInputStream::EmitBlock(const uint8_t* data,
                                                     const size_t non_padding_len,
                                                     const bool is_end_of_stream) {
  stream_index_ += chunk_size_;
  std::optional<uint64_t> timestamp_ish;
  std::swap(timestamp_ish, next_output_timestamp_);

  std::optional<uint64_t> flush_timestamp;
  if (is_end_of_stream) {
    auto [timestamp, status] = ExtrapolateTimestamp();
    if (status != kOk) {
      return status;
    }
    flush_timestamp = timestamp;
  }

  InputBlock input_block;
  input_block.data = data;
  input_block.len = chunk_size_;
  input_block.non_padding_len = non_padding_len;
  input_block.is_end_of_stream = is_end_of_stream;
  input_block.timestamp_ish = timestamp_ish;
  input_block.flush_timestamp_ish = flush_timestamp;
  if (input_block_processor_(input_block) == kTerminate) {
    early_terminated_ = true;
    return kUserTerminated;
  }

  return kOk;
}

std::pair<std::optional<uint64_t>, ChunkInputStream::Status>
ChunkInputStream::ExtrapolateTimestamp() {
  std::optional<uint64_t> timestamp;
  if (timestamp_extrapolator_.has_information() &&
      !(timestamp = timestamp_extrapolator_.Extrapolate(stream_index_))) {
    return {std::nullopt, kExtrapolationFailedWithoutTimebase};
  }

  return {timestamp, kOk};
}

size_t ChunkInputStream::BytesSeen() const { return stream_index_ + scratch_block_.len; }
