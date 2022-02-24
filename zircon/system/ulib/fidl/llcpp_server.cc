// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fidl/llcpp/server.h>

namespace fidl {

namespace internal {

::fidl::DispatchResult TryDispatch(void* impl, ::fidl::IncomingMessage& msg,
                                   fidl::internal::IncomingTransportContext transport_context,
                                   ::fidl::Transaction* txn, const MethodEntry* begin,
                                   const MethodEntry* end) {
  if (!msg.ok()) {
    txn->InternalError(fidl::UnbindInfo{msg}, fidl::ErrorOrigin::kReceive);
    // |TryDispatch| is used to ad-hoc compose protocols by trying a series of
    // |TryDispatch|. If the message has an error, exit the cascade of dispatch
    // attempts early since it is meaningless to keep trying otherwise.
    return ::fidl::DispatchResult::kFound;
  }
  auto* hdr = msg.header();
  while (begin < end) {
    if (hdr->ordinal == begin->ordinal) {
      zx_status_t decode_status =
          begin->dispatch(impl, std::move(msg), std::move(transport_context), txn);
      if (unlikely(decode_status != ZX_OK)) {
        txn->InternalError(UnbindInfo{fidl::Result::DecodeError(decode_status)},
                           fidl::ErrorOrigin::kReceive);
      }
      return ::fidl::DispatchResult::kFound;
    }
    ++begin;
  }
  return ::fidl::DispatchResult::kNotFound;
}

void Dispatch(void* impl, ::fidl::IncomingMessage& msg,
              fidl::internal::IncomingTransportContext transport_context, ::fidl::Transaction* txn,
              const MethodEntry* begin, const MethodEntry* end) {
  ::fidl::DispatchResult result =
      TryDispatch(impl, msg, std::move(transport_context), txn, begin, end);
  switch (result) {
    case ::fidl::DispatchResult::kNotFound:
      std::move(msg).CloseHandles();
      txn->InternalError(::fidl::UnbindInfo::UnknownOrdinal(), ::fidl::ErrorOrigin::kReceive);
      break;
    case ::fidl::DispatchResult::kFound:
      break;
  }
}

::fidl::Result WeakEventSenderInner::SendEvent(::fidl::OutgoingMessage& message) const {
  if (auto binding = binding_.lock()) {
    message.set_txid(0);
    message.Write(binding->transport());
    if (!message.ok()) {
      HandleSendError(message.error());
      return message.error();
    }
    return fidl::Result::Ok();
  }
  return fidl::Result::Unbound();
}

void WeakEventSenderInner::HandleSendError(fidl::Result error) const {
  if (auto binding = binding_.lock()) {
    binding->HandleError(std::move(binding), {UnbindInfo{error}, ErrorOrigin::kSend});
  }
}

}  // namespace internal

}  // namespace fidl
