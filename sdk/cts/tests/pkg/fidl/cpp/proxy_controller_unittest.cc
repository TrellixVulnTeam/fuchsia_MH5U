// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/fidl/cpp/internal/proxy_controller.h"

#include <lib/fidl/cpp/message_buffer.h>
#include <lib/fidl/cpp/message_builder.h>
#include <lib/fit/defer.h>
#include <lib/zx/channel.h>

#include <zxtest/zxtest.h>

#include "lib/fidl/cpp/string.h"
#include "lib/fidl/txn_header.h"
#include "testing/fidl/async_loop_for_test.h"
#include "testing/fidl/fidl_types.h"

namespace fidl {
namespace internal {
namespace {

TEST(ProxyController, Trivial) { ProxyController controller; }

TEST(ProxyController, Send) {
  zx::channel h1, h2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));

  fidl::test::AsyncLoopForTest loop;

  ProxyController controller;
  EXPECT_EQ(ZX_OK, controller.reader().Bind(std::move(h1)));

  MessageEncoder encoder(5u);
  StringPtr string("hello!");
  fidl::Encode(&encoder, &string, encoder.Alloc(sizeof(fidl_string_t)));

  zx_status_t status = std::numeric_limits<zx_status_t>::max();
  controller.reader().set_error_handler([&status](zx_status_t s) { status = s; });
  controller.Send(&unbounded_nonnullable_string_message_type, encoder.GetMessage(), nullptr);
  EXPECT_EQ(std::numeric_limits<zx_status_t>::max(), status);

  IncomingMessageBuffer buffer;
  HLCPPIncomingMessage message = buffer.CreateEmptyIncomingMessage();
  EXPECT_EQ(ZX_OK, message.Read(h2.get(), 0));
  EXPECT_EQ(0u, message.txid());
  EXPECT_EQ(5u, message.ordinal());

  fidl_string_t* view = message.GetBodyViewAs<fidl_string_t>();
  EXPECT_EQ(6u, view->size);
}

TEST(ProxyController, Callback) {
  zx::channel h1, h2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));

  fidl::test::AsyncLoopForTest loop;

  ProxyController controller;
  EXPECT_EQ(ZX_OK, controller.reader().Bind(std::move(h1)));

  MessageEncoder encoder(3u);
  StringPtr string("hello!");
  fidl::Encode(&encoder, &string, encoder.Alloc(sizeof(fidl_string_t)));

  int callback_count = 0;
  auto handler = std::make_unique<SingleUseMessageHandler>(
      [&callback_count](HLCPPIncomingMessage&& message) {
        ++callback_count;
        EXPECT_EQ(42u, message.ordinal());
        return ZX_OK;
      },
      &zero_arg_message_type);

  zx_status_t status = std::numeric_limits<zx_status_t>::max();
  controller.reader().set_error_handler([&status](zx_status_t s) { status = s; });
  controller.Send(&unbounded_nonnullable_string_message_type, encoder.GetMessage(),
                  std::move(handler));
  EXPECT_EQ(std::numeric_limits<zx_status_t>::max(), status);

  EXPECT_EQ(0, callback_count);
  loop.RunUntilIdle();
  EXPECT_EQ(0, callback_count);

  IncomingMessageBuffer buffer;
  HLCPPIncomingMessage message = buffer.CreateEmptyIncomingMessage();
  EXPECT_EQ(ZX_OK, message.Read(h2.get(), 0));
  EXPECT_NE(0u, message.txid());
  EXPECT_EQ(3u, message.ordinal());

  zx_txid_t txid = message.txid();
  fidl_message_header_t header = {};
  fidl_init_txn_header(&header, txid, 42u);

  EXPECT_EQ(ZX_OK, h2.write(0, &header, sizeof(fidl_message_header_t), nullptr, 0));

  EXPECT_EQ(0, callback_count);
  loop.RunUntilIdle();
  EXPECT_EQ(1, callback_count);
}

TEST(ProxyController, BadSend) {
  zx::channel h1, h2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));

  fidl::test::AsyncLoopForTest loop;

  ProxyController controller;
  EXPECT_EQ(ZX_OK, controller.reader().Bind(std::move(h1)));

  MessageEncoder encoder(3u);
  // Bad message format.

  int error_count = 0;
  controller.reader().set_error_handler([&error_count](zx_status_t status) {
    EXPECT_EQ(ZX_ERR_PEER_CLOSED, status);
    ++error_count;
  });

  EXPECT_EQ(0, error_count);
  zx_status_t status = std::numeric_limits<zx_status_t>::max();
  controller.reader().set_error_handler([&status](zx_status_t s) { status = s; });
  controller.Send(&unbounded_nonnullable_string_message_type, encoder.GetMessage(), nullptr);
  EXPECT_EQ(ZX_ERR_BUFFER_TOO_SMALL, status);
  EXPECT_EQ(0, error_count);
  loop.RunUntilIdle();
  EXPECT_EQ(0, error_count);

  EXPECT_EQ(ZX_HANDLE_INVALID, controller.reader().channel());
}

TEST(ProxyController, BadSendNoErrorHandlerClosesChannel) {
  zx::channel h1, h2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));
  zx_handle_t h1_raw = h1.get();

  fidl::test::AsyncLoopForTest loop;

  ProxyController controller;
  EXPECT_EQ(ZX_OK, controller.reader().Bind(std::move(h1)));

  EXPECT_EQ(ZX_OK, zx_object_get_info(h1_raw, ZX_INFO_HANDLE_VALID, nullptr, 0, nullptr, nullptr));

  uint8_t bytes[1000] = {};
  fidl::HLCPPOutgoingMessage outgoing_msg(BytePart(bytes, sizeof(bytes)),
                                          fidl::HandleDispositionPart());
  controller.Send(&unbounded_nonnullable_string_message_type, std::move(outgoing_msg), nullptr);

  EXPECT_EQ(ZX_ERR_BAD_HANDLE,
            zx_object_get_info(h1_raw, ZX_INFO_HANDLE_VALID, nullptr, 0, nullptr, nullptr));
}

TEST(ProxyController, BadReply) {
  zx::channel h1, h2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));

  fidl::test::AsyncLoopForTest loop;

  ProxyController controller;
  EXPECT_EQ(ZX_OK, controller.reader().Bind(std::move(h1)));

  zx_status_t expected_error = ZX_ERR_NOT_SUPPORTED;
  int error_count = 0;
  controller.reader().set_error_handler([&expected_error, &error_count](zx_status_t status) {
    EXPECT_EQ(expected_error, status);
    ++error_count;
  });

  fidl_message_header_t header = {};
  fidl_init_txn_header(&header, 0, 42u);

  EXPECT_EQ(ZX_OK, h2.write(0, &header, sizeof(fidl_message_header_t), nullptr, 0));

  EXPECT_EQ(0, error_count);
  loop.RunUntilIdle();
  EXPECT_EQ(1, error_count);

  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));
  EXPECT_EQ(ZX_OK, controller.reader().Bind(std::move(h1)));

  header.txid = 42u;

  expected_error = ZX_ERR_NOT_FOUND;
  EXPECT_EQ(ZX_OK, h2.write(0, &header, sizeof(fidl_message_header_t), nullptr, 0));

  EXPECT_EQ(1, error_count);
  loop.RunUntilIdle();
  EXPECT_EQ(2, error_count);
}

TEST(ProxyController, ShortReply) {
  zx::channel h1, h2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));

  fidl::test::AsyncLoopForTest loop;

  ProxyController controller;
  EXPECT_EQ(ZX_OK, controller.reader().Bind(std::move(h1)));

  int expected_error = ZX_ERR_NOT_SUPPORTED;
  int error_count = 0;
  controller.reader().set_error_handler([&expected_error, &error_count](zx_status_t status) {
    EXPECT_EQ(expected_error, status);
    ++error_count;
  });

  fidl_message_header_t header = {};
  fidl_init_txn_header(&header, 0, 42u);

  EXPECT_EQ(ZX_OK, h2.write(0, &header, sizeof(fidl_message_header_t), nullptr, 0));

  EXPECT_EQ(0, error_count);
  loop.RunUntilIdle();
  EXPECT_EQ(1, error_count);

  expected_error = ZX_ERR_INVALID_ARGS;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));
  EXPECT_EQ(ZX_OK, controller.reader().Bind(std::move(h1)));

  header.txid = 42u;

  EXPECT_EQ(ZX_OK, h2.write(0, "a", 1, nullptr, 0));

  EXPECT_EQ(1, error_count);
  loop.RunUntilIdle();
  EXPECT_EQ(2, error_count);
}

TEST(ProxyController, Move) {
  zx::channel h1, h2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));

  fidl::test::AsyncLoopForTest loop;

  ProxyController controller1;
  EXPECT_EQ(ZX_OK, controller1.reader().Bind(std::move(h1)));

  MessageEncoder encoder(3u);
  StringPtr string("hello!");
  fidl::Encode(&encoder, &string, encoder.Alloc(sizeof(fidl_string_t)));

  int callback_count = 0;
  auto handler = std::make_unique<SingleUseMessageHandler>(
      [&callback_count](HLCPPIncomingMessage&& message) {
        ++callback_count;
        EXPECT_EQ(42u, message.ordinal());
        return ZX_OK;
      },
      &zero_arg_message_type);

  zx_status_t status = std::numeric_limits<zx_status_t>::max();
  controller1.reader().set_error_handler([&status](zx_status_t s) { status = s; });
  controller1.Send(&unbounded_nonnullable_string_message_type, encoder.GetMessage(),
                   std::move(handler));
  EXPECT_EQ(std::numeric_limits<zx_status_t>::max(), status);

  EXPECT_EQ(0, callback_count);
  loop.RunUntilIdle();
  EXPECT_EQ(0, callback_count);

  IncomingMessageBuffer buffer;
  HLCPPIncomingMessage message = buffer.CreateEmptyIncomingMessage();
  EXPECT_EQ(ZX_OK, message.Read(h2.get(), 0));
  EXPECT_NE(0u, message.txid());
  EXPECT_EQ(3u, message.ordinal());

  ProxyController controller2 = std::move(controller1);
  EXPECT_FALSE(controller1.reader().is_bound());
  EXPECT_TRUE(controller2.reader().is_bound());

  zx_txid_t txid = message.txid();
  fidl_message_header_t header = {};
  fidl_init_txn_header(&header, txid, 42u);

  EXPECT_EQ(ZX_OK, h2.write(0, &header, sizeof(fidl_message_header_t), nullptr, 0));

  EXPECT_EQ(0, callback_count);
  loop.RunUntilIdle();
  EXPECT_EQ(1, callback_count);
}

TEST(ProxyController, Reset) {
  zx::channel h1, h2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));

  fidl::test::AsyncLoopForTest loop;

  ProxyController controller;
  EXPECT_EQ(ZX_OK, controller.reader().Bind(std::move(h1)));

  MessageEncoder encoder(3u);
  StringPtr string("hello!");
  fidl::Encode(&encoder, &string, encoder.Alloc(sizeof(fidl_string_t)));

  int callback_count = 0;
  auto handler = std::make_unique<SingleUseMessageHandler>(
      [&callback_count](HLCPPIncomingMessage&& message) {
        ++callback_count;
        EXPECT_EQ(42u, message.ordinal());
        return ZX_OK;
      },
      &zero_arg_message_type);

  zx_status_t status = std::numeric_limits<zx_status_t>::max();
  controller.reader().set_error_handler([&status](zx_status_t s) { status = s; });
  controller.Send(&unbounded_nonnullable_string_message_type, encoder.GetMessage(),
                  std::move(handler));
  EXPECT_EQ(std::numeric_limits<zx_status_t>::max(), status);

  EXPECT_EQ(0, callback_count);
  loop.RunUntilIdle();
  EXPECT_EQ(0, callback_count);

  IncomingMessageBuffer buffer;
  HLCPPIncomingMessage message = buffer.CreateEmptyIncomingMessage();
  EXPECT_EQ(ZX_OK, message.Read(h2.get(), 0));
  EXPECT_NE(0u, message.txid());
  EXPECT_EQ(3u, message.ordinal());

  controller.Reset();
  EXPECT_FALSE(controller.reader().is_bound());

  zx_txid_t txid = message.txid();
  fidl_message_header_t header = {};
  fidl_init_txn_header(&header, txid, 42u);

  EXPECT_EQ(ZX_ERR_PEER_CLOSED, h2.write(0, &header, sizeof(fidl_message_header_t), nullptr, 0));

  EXPECT_EQ(0, callback_count);
  loop.RunUntilIdle();
  EXPECT_EQ(0, callback_count);
}

TEST(ProxyController, ReentrantDestructor) {
  zx::channel h1, h2;
  EXPECT_EQ(ZX_OK, zx::channel::create(0, &h1, &h2));

  fidl::test::AsyncLoopForTest loop;

  ProxyController controller;
  EXPECT_EQ(ZX_OK, controller.reader().Bind(std::move(h1)));

  MessageEncoder encoder(3u);
  StringPtr string("hello!");
  fidl::Encode(&encoder, &string, encoder.Alloc(sizeof(fidl_string_t)));

  int destructor_count = 0;
  auto defer = fit::defer([&destructor_count, &controller]() {
    ++destructor_count;
    EXPECT_EQ(destructor_count, 1);

    MessageEncoder encoder(3u);
    StringPtr string("world!");
    fidl::Encode(&encoder, &string, encoder.Alloc(sizeof(fidl_string_t)));
    auto callback_handler = std::make_unique<SingleUseMessageHandler>(
        [](HLCPPIncomingMessage&& message) { return ZX_OK; }, &zero_arg_message_type);
    zx_status_t status = std::numeric_limits<zx_status_t>::max();
    controller.reader().set_error_handler([&status](zx_status_t s) { status = s; });
    controller.Send(&unbounded_nonnullable_string_message_type, encoder.GetMessage(),
                    std::move(callback_handler));
    EXPECT_EQ(ZX_ERR_BAD_HANDLE, status);

    controller.Reset();
  });
  auto handler = std::make_unique<SingleUseMessageHandler>(
      [defer = std::move(defer)](HLCPPIncomingMessage&& message) { return ZX_OK; },
      &zero_arg_message_type);

  zx_status_t status = std::numeric_limits<zx_status_t>::max();
  controller.reader().set_error_handler([&status](zx_status_t s) { status = s; });
  controller.Send(&unbounded_nonnullable_string_message_type, encoder.GetMessage(),
                  std::move(handler));
  EXPECT_EQ(std::numeric_limits<zx_status_t>::max(), status);

  loop.RunUntilIdle();

  EXPECT_EQ(destructor_count, 0);
  controller.Reset();
  EXPECT_EQ(destructor_count, 1);
  EXPECT_FALSE(controller.reader().is_bound());

  loop.RunUntilIdle();
  EXPECT_EQ(destructor_count, 1);
}

}  // namespace
}  // namespace internal
}  // namespace fidl
