// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_IPC_MESSAGE_WRITER_H_
#define SRC_DEVELOPER_DEBUG_IPC_MESSAGE_WRITER_H_

#include <stdint.h>

#include <vector>

#include "src/developer/debug/ipc/protocol.h"

namespace debug_ipc {

struct MsgHeader;

// Provides a simple means to append to a dynamic buffer different types of
// data.
//
// The first 4 bytes of each message is the message size. It's assumed that
// these bytes will be explicitly written to. Normally a message will start
// with a struct which contains space for this explicitly.
class MessageWriter {
 public:
  MessageWriter();
  // The argument is a hint for the initial size of the message.
  explicit MessageWriter(size_t initial_size);
  ~MessageWriter();

  void WriteBytes(const void* data, uint32_t len);
  void WriteInt32(int32_t i);
  void WriteUint32(uint32_t i);
  void WriteInt64(int64_t i);
  void WriteUint64(uint64_t i);
  void WriteString(const std::string& str);
  void WriteBool(bool b);

  void WriteHeader(MsgHeader::Type type, uint32_t transaction_id);

  size_t current_length() const { return buffer_.size(); }

  // Writes the size of the current buffer to the first 4 bytes, and
  // destructively returns the buffer.
  std::vector<char> MessageComplete();

 private:
  std::vector<char> buffer_;
};

}  // namespace debug_ipc

#endif  // SRC_DEVELOPER_DEBUG_IPC_MESSAGE_WRITER_H_
