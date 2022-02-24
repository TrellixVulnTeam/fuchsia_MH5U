// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FIDL_FIDL_READER_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FIDL_FIDL_READER_H_

#include <fuchsia/media/playback/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/cpp/wait.h>
#include <lib/zx/socket.h>

#include <atomic>
#include <memory>

#include "src/media/playback/mediaplayer/demux/reader.h"
#include "src/media/playback/mediaplayer/util/incident.h"

namespace media_player {

// Reads raw data from a SeekingReader service.
class FidlReader : public Reader, public std::enable_shared_from_this<FidlReader> {
 public:
  // Creates an FidlReader. Must be called on a fidl thread.
  static std::shared_ptr<Reader> Create(
      fidl::InterfaceHandle<fuchsia::media::playback::SeekingReader> seeking_reader) {
    return std::shared_ptr<Reader>(new FidlReader(std::move(seeking_reader)));
  }

  ~FidlReader() override;

  // Reader implementation.
  void Describe(DescribeCallback callback) override;

  void ReadAt(size_t position, uint8_t* buffer, size_t bytes_to_read,
              ReadAtCallback callback) override;

 private:
  FidlReader(fidl::InterfaceHandle<fuchsia::media::playback::SeekingReader> seeking_reader);

  // Continues a ReadAt operation on the thread on which this reader was
  // constructed (a fidl thread).
  void ContinueReadAt();

  // Reads from socket_ into read_at_buffer_.
  void ReadFromSocket();

  // Completes a ReadAt operation by calling the read_at_callback_.
  void CompleteReadAt(zx_status_t result, size_t bytes_read = 0);

  // Shuts down the consumer handle and calls CompleteReadAt.
  void FailReadAt(zx_status_t status);

  fuchsia::media::playback::SeekingReaderPtr seeking_reader_;
  zx_status_t status_ = ZX_OK;
  size_t size_ = kUnknownSize;
  bool can_seek_ = false;
  async_dispatcher_t* dispatcher_;
  Incident ready_;

  std::atomic_bool read_in_progress_;
  size_t read_at_position_;
  uint8_t* read_at_buffer_;
  size_t read_at_bytes_to_read_;
  size_t read_at_bytes_remaining_;
  ReadAtCallback read_at_callback_;
  zx::socket socket_;
  size_t socket_position_ = kUnknownSize;
  std::unique_ptr<async::Wait> waiter_;
};

}  // namespace media_player

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_FIDL_FIDL_READER_H_
