// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/fidl/cpp/client.h>
#include <lib/fidl/llcpp/connect_service.h>

#include <array>

#include <zxtest/zxtest.h>

#include "test_messages.h"

namespace {

class TestProtocol {
 public:
  using Transport = fidl::internal::ChannelTransport;
  TestProtocol() = delete;
};

}  // namespace

// Fake client implementations.
namespace fidl {

template <>
class AsyncEventHandler<TestProtocol> : public fidl::internal::AsyncEventHandler {};

namespace internal {

template <>
class WireWeakAsyncClientImpl<TestProtocol> : public fidl::internal::ClientImplBase {
 public:
  using ClientImplBase::ClientImplBase;

  void SomeWireMethod() {
    GoodMessage msg;
    fidl::Result result = _client_base()->MakeSyncCallWith(
        [&](std::shared_ptr<fidl::internal::AnyTransport> transport) {
          zx_status_t status = zx_channel_write_etc(
              transport->get<fidl::internal::ChannelTransport>()->get(), 0,
              static_cast<void*>(msg.message().bytes().data()), msg.message().bytes().actual(),
              msg.message().handles().data(), msg.message().handles().actual());
          EXPECT_OK(status);
          return fidl::Result::Ok();
        });
    EXPECT_OK(result.status());
  }
};

template <>
class NaturalEventDispatcher<TestProtocol>
    : public IncomingEventDispatcher<fidl::AsyncEventHandler<TestProtocol>> {
 public:
  using IncomingEventDispatcher<fidl::AsyncEventHandler<TestProtocol>>::IncomingEventDispatcher;

 private:
  std::optional<UnbindInfo> DispatchEvent(
      fidl::IncomingMessage& msg, internal::IncomingTransportContext transport_context) override {
    ZX_PANIC("Not used in this test");
  }
};

template <>
class NaturalClientImpl<TestProtocol> : public NaturalClientBase {
 public:
  using NaturalClientBase::NaturalClientBase;

  void SomeNaturalMethod() const {
    GoodMessage msg;
    fidl::Result result = messenger().OneWay(msg.type(), msg.message());
    EXPECT_OK(result.status());
  }
};

}  // namespace internal
}  // namespace fidl

namespace {

class ClientFixture : public zxtest::Test {
 public:
  ClientFixture() : loop_(&kAsyncLoopConfigNeverAttachToThread) {}

  void SetUp() override {
    zx::status endpoints = fidl::CreateEndpoints<TestProtocol>();
    ASSERT_OK(endpoints.status_value());
    endpoints_ = std::move(*endpoints);
  }

  async::Loop& loop() { return loop_; }

  fidl::Endpoints<TestProtocol>& endpoints() { return endpoints_; }

  fidl::IncomingMessage ReadFromServer() {
    return fidl::MessageRead(endpoints_.server.channel(),
                             fidl::BufferSpan(read_buffer_.data(), read_buffer_.size()), nullptr,
                             nullptr, 0);
  }

 private:
  async::Loop loop_;
  fidl::Endpoints<TestProtocol> endpoints_;
  FIDL_ALIGNDECL std::array<uint8_t, ZX_CHANNEL_MAX_MSG_BYTES> read_buffer_;
};

class Client : public ClientFixture {};
class SharedClient : public ClientFixture {};

TEST_F(Client, DefaultConstruction) {
  fidl::Client<TestProtocol> client;
  EXPECT_FALSE(client.is_valid());
}

TEST_F(SharedClient, DefaultConstruction) {
  fidl::SharedClient<TestProtocol> client;
  EXPECT_FALSE(client.is_valid());
}

TEST_F(Client, InvalidAccess) {
  fidl::Client<TestProtocol> client;
  ASSERT_DEATH([&] { client->SomeNaturalMethod(); });
  ASSERT_DEATH([&] { client.wire()->SomeWireMethod(); });
}

TEST_F(SharedClient, InvalidAccess) {
  fidl::SharedClient<TestProtocol> client;
  ASSERT_DEATH([&] { client->SomeNaturalMethod(); });
  ASSERT_DEATH([&] { client.wire()->SomeWireMethod(); });
  ASSERT_DEATH([&] { client.AsyncTeardown(); });
}

TEST_F(Client, Move) {
  fidl::Client<TestProtocol> client;
  client.Bind(std::move(endpoints().client), loop().dispatcher());
  EXPECT_TRUE(client.is_valid());

  fidl::Client<TestProtocol> client2 = std::move(client);
  EXPECT_FALSE(client.is_valid());
  EXPECT_TRUE(client2.is_valid());
  ASSERT_DEATH([&] { client->SomeNaturalMethod(); });
}

TEST_F(SharedClient, Move) {
  fidl::SharedClient<TestProtocol> client;
  client.Bind(std::move(endpoints().client), loop().dispatcher());
  EXPECT_TRUE(client.is_valid());

  fidl::SharedClient<TestProtocol> client2 = std::move(client);
  EXPECT_FALSE(client.is_valid());
  EXPECT_TRUE(client2.is_valid());
  ASSERT_DEATH([&] { client->SomeNaturalMethod(); });
}

TEST_F(SharedClient, Clone) {
  fidl::SharedClient<TestProtocol> client;
  client.Bind(std::move(endpoints().client), loop().dispatcher());
  EXPECT_TRUE(client.is_valid());

  fidl::SharedClient<TestProtocol> client2 = client.Clone();
  EXPECT_TRUE(client.is_valid());
  EXPECT_TRUE(client2.is_valid());
  ASSERT_NO_DEATH([&] { client->SomeNaturalMethod(); });
  ASSERT_NO_DEATH([&] { client2->SomeNaturalMethod(); });
}

TEST_F(Client, WireCall) {
  fidl::Client client(std::move(endpoints().client), loop().dispatcher());
  client.wire()->SomeWireMethod();
  auto msg = ReadFromServer();
  EXPECT_OK(msg.status());
}

TEST_F(SharedClient, WireCall) {
  fidl::SharedClient client(std::move(endpoints().client), loop().dispatcher());
  client.wire()->SomeWireMethod();
  auto msg = ReadFromServer();
  EXPECT_OK(msg.status());
}

TEST_F(Client, NaturalCall) {
  fidl::Client client(std::move(endpoints().client), loop().dispatcher());
  client->SomeNaturalMethod();
  auto msg = ReadFromServer();
  EXPECT_OK(msg.status());
}

TEST_F(SharedClient, NaturalCall) {
  fidl::SharedClient client(std::move(endpoints().client), loop().dispatcher());
  client->SomeNaturalMethod();
  auto msg = ReadFromServer();
  EXPECT_OK(msg.status());
}

}  // namespace
