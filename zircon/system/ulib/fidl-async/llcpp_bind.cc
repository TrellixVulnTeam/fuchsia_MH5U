// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fidl-async/cpp/bind.h>
#include <lib/fidl-async/cpp/channel_transaction.h>
#include <lib/fidl/llcpp/internal/transport_channel.h>
#include <lib/fidl/llcpp/transaction.h>
#include <lib/fidl/trace.h>
#include <lib/fidl/txn_header.h>
#include <lib/zx/channel.h>
#include <zircon/syscalls.h>

#include <type_traits>

namespace fidl {

namespace internal {

SimpleBinding::SimpleBinding(async_dispatcher_t* dispatcher, zx::channel channel,
                             fidl::internal::IncomingMessageDispatcher* interface,
                             AnyOnChannelClosedFn on_channel_closed_fn)
    : async_wait_t({
          .state = ASYNC_STATE_INIT,
          .handler = &MessageHandler,
          .object = channel.release(),
          .trigger = ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
          .options = 0,
      }),
      dispatcher_(dispatcher),
      interface_(interface),
      on_channel_closed_fn_(std::move(on_channel_closed_fn)) {}

SimpleBinding::~SimpleBinding() {
  zx_handle_close(async_wait_t::object);
  if (on_channel_closed_fn_) {
    on_channel_closed_fn_(interface_);
  }
}

void SimpleBinding::MessageHandler(async_dispatcher_t* dispatcher, async_wait_t* wait,
                                   zx_status_t dispatcher_status,
                                   const zx_packet_signal_t* signal) {
  std::unique_ptr<SimpleBinding> binding(static_cast<SimpleBinding*>(wait));
  if (dispatcher_status != ZX_OK) {
    return;
  }

  if (signal->observed & ZX_CHANNEL_READABLE) {
    uint8_t bytes[ZX_CHANNEL_MAX_MSG_BYTES];
    zx_handle_t handles[ZX_CHANNEL_MAX_MSG_HANDLES];
    fidl_channel_handle_metadata_t handle_metadata[ZX_CHANNEL_MAX_MSG_HANDLES];
    for (uint64_t i = 0; i < signal->count; i++) {
      fidl_trace(WillLLCPPAsyncChannelRead);
      fidl::IncomingMessage msg = fidl::MessageRead(
          zx::unowned_channel(wait->object), fidl::BufferSpan(bytes, std::size(bytes)), handles,
          handle_metadata, ZX_CHANNEL_MAX_MSG_HANDLES);
      if (!msg.ok())
        return;
      fidl_trace(DidLLCPPAsyncChannelRead, nullptr /* type */, bytes, msg.byte_actual(),
                 msg.handle_actual());

      auto* hdr = msg.header();
      ChannelTransaction txn(hdr->txid, std::move(binding));
      txn.Dispatch(msg);
      binding = txn.TakeBinding();
      if (!binding) {
        return;
      }
    }

    // Will only get here if every single message was handled synchronously and successfully.
    BeginWait(&binding);
  } else {
    ZX_DEBUG_ASSERT(signal->observed & ZX_CHANNEL_PEER_CLOSED);
  }
}

zx_status_t BeginWait(std::unique_ptr<SimpleBinding>* unique_binding) {
  SimpleBinding* binding = unique_binding->release();
  zx_status_t status;
  if ((status = async_begin_wait(binding->dispatcher_, binding)) != ZX_OK) {
    // Failed to transfer binding ownership to async dispatcher.
    unique_binding->reset(binding);
  }
  return status;
}

zx_status_t BindSingleInFlightOnlyImpl(async_dispatcher_t* dispatcher, zx::channel channel,
                                       fidl::internal::IncomingMessageDispatcher* interface,
                                       AnyOnChannelClosedFn on_channel_closed_fn) {
  auto binding = std::make_unique<SimpleBinding>(dispatcher, std::move(channel), interface,
                                                 std::move(on_channel_closed_fn));
  auto status = BeginWait(&binding);
  return status;
}

}  // namespace internal

}  // namespace fidl
