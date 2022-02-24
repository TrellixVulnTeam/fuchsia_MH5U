// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/fidl_codec/message_decoder.h"

#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <test/fidlcodec/examples/cpp/fidl.h>
#include <testing/fidl/frobinator_impl.h>

#include "src/lib/fidl_codec/fidl_codec_test.h"
#include "src/lib/fidl_codec/library_loader.h"
#include "src/lib/fidl_codec/library_loader_test_data.h"
#include "src/lib/fidl_codec/message_decoder.h"
#include "src/lib/fidl_codec/wire_object.h"
#include "src/lib/fidl_codec/wire_parser.h"

using test::fidlcodec::examples::Echo;
using test::fidlcodec::examples::FidlCodecTestInterface;

namespace fidl_codec {

constexpr int kColumns = 80;
constexpr uint64_t kProcessKoid = 0x1234;

class MessageDecoderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loader_ = GetLoader();
    ASSERT_NE(loader_, nullptr);
    display_options_.pretty_print = true;
    display_options_.columns = kColumns;
    decoder_ = std::make_unique<MessageDecoderDispatcher>(loader_, display_options_);
  }

  // Intercepts the caller's method call on a FIDL InterfacePtr and returns the bytes
  // sent over the channel.
  template <class T>
  fidl::HLCPPIncomingMessage InvokeAndIntercept(
      std::function<void(fidl::InterfacePtr<T>&)> invoker) {
    fidl::HLCPPIncomingMessage message = buffer_.CreateEmptyIncomingMessage();
    InterceptRequest<T>(message, invoker);
    return message;
  }

  // Simulates a server sending an epitaph and returns the bytes sent over the channel.
  fidl::HLCPPIncomingMessage InvokeAndReceiveEpitaph(zx_status_t epitaph) {
    fidl::HLCPPIncomingMessage message = buffer_.CreateEmptyIncomingMessage();
    // The protocol doesn't matter, no methods are actually called.
    InterceptEpitaphResponse<FidlCodecTestInterface>(message, epitaph);
    return message;
  }

  // Asserts that the decoded and FIDL message matches the expected display output.
  // `syscall_type` interprets the FIDL message as received or sent.
  void AssertDecoded(const fidl::HLCPPIncomingMessage& message, SyscallFidlType syscall_type,
                     const char* expected) {
    DecodedMessage decoded_message;
    std::stringstream error_stream;
    decoded_message.DecodeMessage(decoder(), process_koid(), ZX_HANDLE_INVALID,
                                  message.bytes().data(), message.bytes().size(), nullptr, 0,
                                  syscall_type, error_stream);
    auto result = std::make_unique<fidl_codec::FidlMessageValue>(
        &decoded_message, error_stream.str(), message.bytes().data(), message.bytes().size(),
        nullptr, 0);

    if (!result->unknown_direction()) {
      // When the direction is known, only one of request/response must be set.
      ASSERT_TRUE((result->decoded_request() == nullptr) ||
                  (result->decoded_response() == nullptr));
    }

    std::stringstream output;
    PrettyPrinter printer(output, decoder()->colors(), /*pretty_print=*/true, /*line_header=*/"",
                          /*max_line_size=*/kColumns,
                          /*header_on_every_line=*/false);
    result->PrettyPrint(nullptr, printer);
    ASSERT_EQ(output.str(), expected) << "expected = " << expected << " actual = " << output.str();
  }

  MessageDecoderDispatcher* decoder() const { return decoder_.get(); }
  uint64_t process_koid() const { return process_koid_; }

 private:
  fidl::IncomingMessageBuffer buffer_;
  LibraryLoader* loader_;
  std::unique_ptr<MessageDecoderDispatcher> decoder_;
  DisplayOptions display_options_;
  uint64_t process_koid_ = kProcessKoid;
};

#define TEST_DECODE_MESSAGE(_interface, _iface, _expected, ...)                  \
  do {                                                                           \
    auto message = InvokeAndIntercept<_interface>(                               \
        [&](fidl::InterfacePtr<_interface>& ptr) { ptr->_iface(__VA_ARGS__); }); \
    AssertDecoded(message, SyscallFidlType::kOutputMessage, _expected);          \
  } while (0)

TEST_F(MessageDecoderTest, TestEmptyLaunched) {
  decoder()->AddLaunchedProcess(process_koid());
  TEST_DECODE_MESSAGE(FidlCodecTestInterface, Empty,
                      "sent request test.fidlcodec.examples/FidlCodecTestInterface.Empty = {}\n");
}

TEST_F(MessageDecoderTest, TestStringLaunched) {
  decoder()->AddLaunchedProcess(process_koid());
  TEST_DECODE_MESSAGE(FidlCodecTestInterface, String,
                      "sent request test.fidlcodec.examples/FidlCodecTestInterface.String = {\n"
                      "  s: string = \"Hello World\"\n"
                      "}\n",
                      "Hello World");
}

TEST_F(MessageDecoderTest, TestStringAttached) {
  TEST_DECODE_MESSAGE(FidlCodecTestInterface, String,
                      "sent request test.fidlcodec.examples/FidlCodecTestInterface.String = {\n"
                      "  s: string = \"Hello World\"\n"
                      "}\n",
                      "Hello World");
}

TEST_F(MessageDecoderTest, TestEchoLaunched) {
  decoder()->AddLaunchedProcess(process_koid());
  TEST_DECODE_MESSAGE(Echo, EchoString,
                      "sent request test.fidlcodec.examples/Echo.EchoString = {\n"
                      "  value: string = \"Hello World\"\n"
                      "}\n",
                      "Hello World", [](const ::fidl::StringPtr&) {});
}

TEST_F(MessageDecoderTest, TestEchoAttached) {
  TEST_DECODE_MESSAGE(Echo, EchoString,
                      "Can't determine request/response. it can be:\n"
                      "  sent request test.fidlcodec.examples/Echo.EchoString = {\n"
                      "    value: string = \"Hello World\"\n"
                      "  }\n"
                      "  sent response test.fidlcodec.examples/Echo.EchoString = {\n"
                      "    response: string = \"Hello World\"\n"
                      "  }\n",
                      "Hello World", [](const ::fidl::StringPtr&) {});
}

TEST_F(MessageDecoderTest, TestEpitaphReceived) {
  auto message = InvokeAndReceiveEpitaph(ZX_ERR_UNAVAILABLE);
  AssertDecoded(message, SyscallFidlType::kInputMessage, "received epitaph ZX_ERR_UNAVAILABLE\n");
}

TEST_F(MessageDecoderTest, TestUnknownEpitaphReceived) {
  auto message = InvokeAndReceiveEpitaph(1990);
  AssertDecoded(message, SyscallFidlType::kInputMessage, "received epitaph status=1990\n");
}

TEST_F(MessageDecoderTest, TestEpitaphSent) {
  auto message = InvokeAndReceiveEpitaph(ZX_ERR_INTERNAL);
  AssertDecoded(message, SyscallFidlType::kOutputMessage, "sent epitaph ZX_ERR_INTERNAL\n");
}

TEST_F(MessageDecoderTest, TestUnknownEpitaphSent) {
  auto message = InvokeAndReceiveEpitaph(1990);
  AssertDecoded(message, SyscallFidlType::kOutputMessage, "sent epitaph status=1990\n");
}

}  // namespace fidl_codec
