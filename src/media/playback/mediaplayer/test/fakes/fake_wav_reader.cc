// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/test/fakes/fake_wav_reader.h"

#include <lib/async/default.h>
#include <lib/zx/socket.h>

namespace media_player {

FakeWavReader::FakeWavReader() : binding_(this) { WriteHeader(); }

void FakeWavReader::WriteHeader() {
  header_.clear();

  // Master chunk.
  WriteHeader4CC("RIFF");
  WriteHeaderUint32(size_ - kChunkSizeDeficit);
  WriteHeader4CC("WAVE");  // Format
  FX_DCHECK(header_.size() == kMasterChunkHeaderSize);

  // Format subchunk.
  WriteHeader4CC("fmt ");
  WriteHeaderUint32(kFormatChunkSize - kChunkSizeDeficit);
  WriteHeaderUint16(kAudioEncoding);
  WriteHeaderUint16(kSamplesPerFrame);
  WriteHeaderUint32(kFramesPerSecond);
  // Byte rate.
  WriteHeaderUint32(kFramesPerSecond * kSamplesPerFrame * kBitsPerSample / 8);
  // Block alignment (frame size in bytes).
  WriteHeaderUint16(kSamplesPerFrame * kBitsPerSample / 8);
  WriteHeaderUint16(kBitsPerSample);
  FX_DCHECK(header_.size() == kMasterChunkHeaderSize + kFormatChunkSize);

  // Data subchunk.
  WriteHeader4CC("data");
  WriteHeaderUint32(size_ - kMasterChunkHeaderSize - kFormatChunkSize - kChunkSizeDeficit);
  FX_DCHECK(header_.size() == kMasterChunkHeaderSize + kFormatChunkSize + kDataChunkHeaderSize);
}

FakeWavReader::~FakeWavReader() {}

void FakeWavReader::Bind(fidl::InterfaceRequest<fuchsia::media::playback::SeekingReader> request) {
  binding_.Bind(std::move(request));
}

void FakeWavReader::Describe(DescribeCallback callback) { callback(ZX_OK, size_, true); }

void FakeWavReader::ReadAt(uint64_t position, ReadAtCallback callback) {
  if (socket_) {
    socket_.reset();
  }

  zx::socket other_socket;
  zx_status_t status = zx::socket::create(0u, &socket_, &other_socket);
  FX_DCHECK(status == ZX_OK);
  callback(ZX_OK, std::move(other_socket));

  position_ = position;

  WriteToSocket();
}

void FakeWavReader::WriteToSocket() {
  while (true) {
    uint8_t byte = GetByte(position_);
    size_t byte_count;

    zx_status_t status = socket_.write(0u, &byte, 1u, &byte_count);
    if (status == ZX_OK) {
      FX_DCHECK(byte_count == 1);
      ++position_;
      continue;
    }

    if (status == ZX_ERR_SHOULD_WAIT) {
      waiter_ =
          std::make_unique<async::Wait>(socket_.get(), ZX_SOCKET_WRITABLE | ZX_SOCKET_PEER_CLOSED);

      waiter_->set_handler([this](async_dispatcher_t* dispatcher, async::Wait* wait,
                                  zx_status_t status, const zx_packet_signal_t* signal) {
        if (status == ZX_ERR_CANCELED) {
          // Run loop has aborted...the app is shutting down.
          return;
        }

        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "AsyncWait failed " << status;
          socket_.reset();
          return;
        }

        WriteToSocket();
      });

      waiter_->Begin(async_get_default_dispatcher());
      return;
    }

    if (status == ZX_ERR_PEER_CLOSED) {
      // Consumer end was closed. This is normal behavior, depending on what
      // the consumer is up to.
      socket_.reset();
      return;
    }

    FX_DCHECK(false) << "zx::socket::write failed, status " << status;
  }
}

void FakeWavReader::WriteHeader4CC(const std::string& value) {
  FX_DCHECK(value.size() == 4);
  header_.push_back(static_cast<uint8_t>(value[0]));
  header_.push_back(static_cast<uint8_t>(value[1]));
  header_.push_back(static_cast<uint8_t>(value[2]));
  header_.push_back(static_cast<uint8_t>(value[3]));
}

void FakeWavReader::WriteHeaderUint16(uint16_t value) {
  header_.push_back(static_cast<uint8_t>(value));
  header_.push_back(static_cast<uint8_t>(value >> 8));
}

void FakeWavReader::WriteHeaderUint32(uint32_t value) {
  header_.push_back(static_cast<uint8_t>(value));
  header_.push_back(static_cast<uint8_t>(value >> 8));
  header_.push_back(static_cast<uint8_t>(value >> 16));
  header_.push_back(static_cast<uint8_t>(value >> 24));
}

uint8_t FakeWavReader::GetByte(size_t position) {
  if (position < header_.size()) {
    // Header.
    return header_[position];
  }

  // Unpleasant sound.
  return static_cast<uint8_t>(position ^ (position >> 8));
}

}  // namespace media_player
