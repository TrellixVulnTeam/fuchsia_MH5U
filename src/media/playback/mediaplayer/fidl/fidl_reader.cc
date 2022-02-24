// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/fidl/fidl_reader.h"

#include <fuchsia/media/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/syslog/cpp/macros.h>

#include <limits>
#include <string>

#include "src/media/playback/mediaplayer/fidl/fidl_type_conversions.h"

namespace media_player {

FidlReader::FidlReader(
    fidl::InterfaceHandle<fuchsia::media::playback::SeekingReader> seeking_reader)
    : seeking_reader_(seeking_reader.Bind()),
      dispatcher_(async_get_default_dispatcher()),
      ready_(dispatcher_) {
  FX_DCHECK(dispatcher_);

  read_in_progress_ = false;

  seeking_reader_->Describe([this](zx_status_t status, uint64_t size, bool can_seek) {
    status_ = status;
    if (status_ == ZX_OK) {
      size_ = size;
      can_seek_ = can_seek;
    }
    ready_.Occur();
  });
}

FidlReader::~FidlReader() {}

void FidlReader::Describe(DescribeCallback callback) {
  ready_.When([this, callback = std::move(callback)]() { callback(status_, size_, can_seek_); });
}

void FidlReader::ReadAt(size_t position, uint8_t* buffer, size_t bytes_to_read,
                        ReadAtCallback callback) {
  FX_DCHECK(buffer);
  FX_DCHECK(bytes_to_read);

  FX_DCHECK(!read_in_progress_) << "ReadAt called while previous call still in progress";
  read_in_progress_ = true;
  read_at_position_ = position;
  read_at_buffer_ = buffer;
  read_at_bytes_to_read_ = bytes_to_read;
  read_at_callback_ = std::move(callback);

  // ReadAt may be called on non-fidl threads, so we use the runner.
  async::PostTask(dispatcher_, [weak_this = std::weak_ptr<FidlReader>(shared_from_this())]() {
    auto shared_this = weak_this.lock();
    if (shared_this) {
      shared_this->ContinueReadAt();
    }
  });
}

void FidlReader::ContinueReadAt() {
  ready_.When([this]() {
    if (status_ != ZX_OK) {
      CompleteReadAt(status_);
      return;
    }

    FX_DCHECK(read_at_position_ < size_);

    if (read_at_position_ + read_at_bytes_to_read_ > size_) {
      read_at_bytes_to_read_ = size_ - read_at_position_;
    }

    read_at_bytes_remaining_ = read_at_bytes_to_read_;

    if (read_at_position_ == socket_position_) {
      FX_DCHECK(socket_);
      ReadFromSocket();
      return;
    }

    socket_.reset();
    socket_position_ = kUnknownSize;

    if (!can_seek_ && read_at_position_ != 0) {
      CompleteReadAt(ZX_ERR_INVALID_ARGS);
      return;
    }

    seeking_reader_->ReadAt(read_at_position_, [this](zx_status_t status, zx::socket socket) {
      if (status_ != ZX_OK) {
        CompleteReadAt(status_);
        return;
      }

      socket_ = std::move(socket);
      socket_position_ = read_at_position_;
      ReadFromSocket();
    });
  });
}

void FidlReader::ReadFromSocket() {
  while (true) {
    FX_DCHECK(read_at_bytes_remaining_ < std::numeric_limits<uint32_t>::max());
    size_t byte_count = 0;
    zx_status_t status = socket_.read(0u, read_at_buffer_, read_at_bytes_remaining_, &byte_count);

    if (status == ZX_ERR_SHOULD_WAIT) {
      waiter_ =
          std::make_unique<async::Wait>(socket_.get(), ZX_SOCKET_READABLE | ZX_SOCKET_PEER_CLOSED);

      waiter_->set_handler([this](async_dispatcher_t* dispatcher, async::Wait* wait,
                                  zx_status_t status, const zx_packet_signal_t* signal) {
        if (status != ZX_OK) {
          if (status != ZX_ERR_CANCELED) {
            FX_LOGS(ERROR) << "Wait failed, status " << status;
          }

          FailReadAt(status);
          return;
        }

        ReadFromSocket();
      });

      waiter_->Begin(async_get_default_dispatcher());

      break;
    }

    waiter_ = nullptr;

    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "zx::socket::read failed, status " << status;
      FailReadAt(status);
      break;
    }

    read_at_buffer_ += byte_count;
    read_at_bytes_remaining_ -= byte_count;
    socket_position_ += byte_count;

    if (read_at_bytes_remaining_ == 0) {
      CompleteReadAt(ZX_OK, read_at_bytes_to_read_);
      break;
    }
  }
}

void FidlReader::CompleteReadAt(zx_status_t status, size_t bytes_read) {
  ReadAtCallback read_at_callback;
  read_at_callback_.swap(read_at_callback);
  read_in_progress_ = false;
  read_at_callback(status, bytes_read);
}

void FidlReader::FailReadAt(zx_status_t status) {
  status_ = status;
  socket_.reset();
  socket_position_ = kUnknownSize;
  CompleteReadAt(status_);
}

}  // namespace media_player
