// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_VOLUME_IMAGE_UTILS_LZ4_DECOMPRESSOR_H_
#define SRC_STORAGE_VOLUME_IMAGE_UTILS_LZ4_DECOMPRESSOR_H_

#include <lib/fpromise/result.h>
#include <lib/stdcompat/span.h>

#include <string>
#include <vector>

#include <lz4/lz4frame.h>

#include "src/storage/volume_image/options.h"
#include "src/storage/volume_image/utils/decompressor.h"

namespace storage::volume_image {

// This class provides an implementation of |Decompressor| backed by LZ4 Decompression algorithm.
//
// This class is move construcable only.
class Lz4Decompressor final : public Decompressor {
 public:
  // Default size for the decompression buffer exposed to the decompression handler.
  static constexpr uint64_t kDecompressionBufferSize = static_cast<uint64_t>(64 * (1u << 10));

  // Returns a |Lz4Decompressor| on success.
  //
  // On failure, returns a string describing the error.
  static fpromise::result<Lz4Decompressor, std::string> Create(
      const CompressionOptions& options,
      uint64_t decompression_buffer_size = kDecompressionBufferSize);

  explicit Lz4Decompressor(uint64_t decompression_buffer_size = kDecompressionBufferSize) {
    decompression_buffer_.resize(decompression_buffer_size, 0);
  }
  Lz4Decompressor(const Lz4Decompressor&) = delete;
  Lz4Decompressor(Lz4Decompressor&&) noexcept = default;
  Lz4Decompressor& operator=(const Lz4Decompressor&) = delete;
  Lz4Decompressor& operator=(Lz4Decompressor&&) = delete;
  ~Lz4Decompressor() final;

  // Returns |fpromise::ok| on success. Setting |handler| for consuming symbols emitted during
  // decompression.
  //
  // On failure, returns a string decribing the error condition.
  fpromise::result<void, std::string> Prepare(Handler handler) final;

  // Returns |fpromise::ok| on success. When data has been fully decompressed, will return |true|,
  // otherwise will return |false|.
  //
  // On failure, returns a string decribing the error condition.
  fpromise::result<DecompressResult, std::string> Decompress(
      cpp20::span<const uint8_t> compressed_data) final;

  // Returns |fpromise::ok| on success. At this point all remaining symbols for the decompressed
  // representation will be emitted.
  //
  // On failure, returns a string describing the error condition.
  fpromise::result<void, std::string> Finalize() final;

  // Provide size hint of the expected compressed content size.
  void ProvideSizeHint(size_t size_hint);

 private:
  // Describes the possible states of the compressor.
  enum class State {
    // The Decompressor was created with valid options, yet it has not been prepared.
    kInitalized,
    // The compressor, has been prepared, and is ready for compressing data.
    kPrepared,
    // The compressor has at least decompressed a piece of data.
    kDecompressed,
    // The compressor finished compressing, and has deallocated the required structures.
    kFinalized,
  };

  // LZ4 decompression context, that handles the LZ4 internals.
  LZ4F_decompressionContext_t context_ = nullptr;

  // Current state of the compressor.
  State state_ = State::kInitalized;

  // Internal buffer used for storing decompressed data.
  std::vector<uint8_t> decompression_buffer_;

  // Provides a callable for handling compressed representation symbols.
  Handler handler_;
};

}  // namespace storage::volume_image

#endif  // SRC_STORAGE_VOLUME_IMAGE_UTILS_LZ4_DECOMPRESSOR_H_
