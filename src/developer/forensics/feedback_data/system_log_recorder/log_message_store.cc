// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/forensics/feedback_data/system_log_recorder/log_message_store.h"

#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>

#include "src/developer/forensics/feedback_data/constants.h"
#include "src/developer/forensics/utils/log_format.h"
#include "src/lib/fxl/strings/join_strings.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace forensics {
namespace feedback_data {
namespace system_log_recorder {
namespace {

const std::string kDroppedFormatStr = "!!! DROPPED %lu MESSAGES !!!\n";

std::string MakeRepeatedWarning(const size_t message_count) {
  if (message_count == 2) {
    return kRepeatedOnceFormatStr;
  } else {
    return fxl::StringPrintf(kRepeatedFormatStr, message_count - 1);
  }
}

std::string FormatError(const std::string& error) {
  return fxl::StringPrintf("!!! LOG PARSING ERROR: %s !!!", error.c_str());
}

}  // namespace

LogMessageStore::LogMessageStore(StorageSize max_block_capacity, StorageSize max_buffer_capacity,
                                 std::unique_ptr<Encoder> encoder)
    : mtx_(),
      buffer_(),
      buffer_stats_(max_buffer_capacity),
      block_stats_(max_block_capacity),
      encoder_(std::move(encoder)) {
  FX_CHECK(max_block_capacity >= max_buffer_capacity);
}

void LogMessageStore::AddToBuffer(const std::string& str) {
  const std::string encoded = encoder_->Encode(str);
  buffer_.push_back(encoded);
  block_stats_.Use(StorageSize::Bytes(encoded.size()));
  buffer_stats_.Use(StorageSize::Bytes(encoded.size()));
}

bool LogMessageStore::Add(::fpromise::result<fuchsia::logger::LogMessage, std::string> log) {
  TRACE_DURATION("feedback:io", "LogMessageStore::Add");

  std::lock_guard<std::mutex> lk(mtx_);

  const auto& log_msg = (log.is_ok()) ? log.value().msg : log.error();

  // 1. Early return if the incoming message is the same as last time.
  if (last_pushed_message_ == log_msg) {
    last_pushed_message_count_++;
    return true;
  }
  // received new message, clear repeat variables.
  last_pushed_message_ = "";
  repeat_buffer_count_ = 0;

  // 2. Push the repeated message if any.
  if (last_pushed_message_count_ > 1) {
    const std::string repeated_msg = MakeRepeatedWarning(last_pushed_message_count_);
    // We always add the repeated message to the buffer, even if it means going over bound as we
    // control its (small) size.
    AddToBuffer(repeated_msg);
  }
  last_pushed_message_count_ = 0;

  // 3. Early return on full buffer when there is a rate limit.
  if (buffer_rate_limit_ && buffer_stats_.IsFull()) {
    ++num_messages_dropped_;
    return false;
  }

  // 4. Serialize incoming message.
  const std::string str = (log.is_ok()) ? Format(log.value()) : FormatError(log.error());

  // 5. Push the incoming message if below the limit or there is no rate limit; otherwise drop it.
  if (!buffer_rate_limit_ || buffer_stats_.CanUse(StorageSize::Bytes(str.size()))) {
    AddToBuffer(str);
    last_pushed_message_ = log_msg;
    last_pushed_message_count_ = 1;
    return true;
  } else {
    // We will drop the rest of the incoming messages until the next Consume(). This avoids trying
    // to squeeze in a shorter message that will wrongfully appear before the DROPPED message.
    buffer_stats_.MakeFull();
    ++num_messages_dropped_;
    return false;
  }
}

std::string LogMessageStore::Consume(bool* end_of_block) {
  FX_CHECK(end_of_block != nullptr);
  TRACE_DURATION("feedback:io", "LogMessageStore::Consume");

  std::lock_guard<std::mutex> lk(mtx_);

  // Optionally log whether the last message was repeated. Stop logging if this warning message has
  // been logged consecutively for more than kRepeatedBuffers times.
  if (last_pushed_message_count_ > 1 && repeat_buffer_count_ < kMaxRepeatedBuffers) {
    AddToBuffer(MakeRepeatedWarning(last_pushed_message_count_));
    last_pushed_message_count_ = 1;
    repeat_buffer_count_++;
  }

  // Optionally log whether some messages were dropped.
  if (num_messages_dropped_ > 0) {
    AddToBuffer(fxl::StringPrintf(kDroppedFormatStr.c_str(), num_messages_dropped_));
    last_pushed_message_ = "";
    last_pushed_message_count_ = 0;
  }

  // Optionally log important message at the end.
  if (to_append_.has_value()) {
    AddToBuffer(to_append_.value());
    to_append_ = std::nullopt;
    // We are changing the last message so we want to reset the last message pushed.
    last_pushed_message_ = "";
    last_pushed_message_count_ = 0;
  }

  // We assume all messages end with a newline character.
  std::string str = fxl::JoinStrings(buffer_);

  buffer_.clear();
  buffer_stats_.Reset();
  num_messages_dropped_ = 0;

  // Reset the encoder at the end of a block.
  if (block_stats_.IsFull()) {
    block_stats_.Reset();
    encoder_->Reset();
    // We reset the last message pushed and its count so that we don't have a block starting with a
    // repeated message without the actual message.
    last_pushed_message_ = "";
    last_pushed_message_count_ = 0;
    *end_of_block = true;
  } else {
    *end_of_block = false;
  }

  return str;
}

void LogMessageStore::AppendToEnd(const std::string& str) { to_append_ = str; }

}  // namespace system_log_recorder
}  // namespace feedback_data
}  // namespace forensics
