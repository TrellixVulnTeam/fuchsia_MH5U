// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/client/frame_symbol_data_provider.h"

#include <inttypes.h>
#include <lib/syslog/cpp/macros.h>

#include "src/developer/debug/shared/message_loop.h"
#include "src/developer/debug/zxdb/client/call_site_symbol_data_provider.h"
#include "src/developer/debug/zxdb/client/frame.h"
#include "src/developer/debug/zxdb/client/memory_dump.h"
#include "src/developer/debug/zxdb/client/process.h"
#include "src/developer/debug/zxdb/client/session.h"
#include "src/developer/debug/zxdb/client/thread.h"
#include "src/developer/debug/zxdb/common/err.h"
#include "src/lib/fxl/strings/string_printf.h"

namespace zxdb {

namespace {

Err CallFrameDestroyedErr() { return Err("Call frame destroyed."); }

Err RegisterUnavailableErr(debug::RegisterID id) {
  return Err(fxl::StringPrintf("Register %s unavailable.", debug::RegisterIDToString(id)));
}

}  // namespace

FrameSymbolDataProvider::FrameSymbolDataProvider(fxl::WeakPtr<Frame> frame)
    : ProcessSymbolDataProvider(frame ? frame->GetThread()->GetProcess()->GetWeakPtr() : nullptr),
      frame_(frame) {}

FrameSymbolDataProvider::~FrameSymbolDataProvider() = default;

fxl::RefPtr<SymbolDataProvider> FrameSymbolDataProvider::GetEntryDataProvider() const {
  if (!frame_)
    return nullptr;

  Stack& stack = frame_->GetThread()->GetStack();
  auto frame_index_or = stack.IndexForFrame(frame_.get());
  if (!frame_index_or)
    return nullptr;

  size_t prev_frame_index = *frame_index_or + 1;
  if (prev_frame_index >= stack.size())
    return nullptr;
  const Frame* prev_frame = stack[prev_frame_index];

  return fxl::MakeRefCounted<CallSiteSymbolDataProvider>(
      frame_->GetThread()->GetProcess()->GetWeakPtr(), prev_frame->GetLocation(),
      prev_frame->GetSymbolDataProvider());
}

std::optional<containers::array_view<uint8_t>> FrameSymbolDataProvider::GetRegister(
    debug::RegisterID id) {
  FX_DCHECK(id != debug::RegisterID::kUnknown);
  if (!frame_)
    return containers::array_view<uint8_t>();  // Synchronously know we don't have the value.

  debug::RegisterCategory category = debug::RegisterIDToCategory(id);
  FX_DCHECK(category != debug::RegisterCategory::kNone);

  const std::vector<debug::RegisterValue>* regs = frame_->GetRegisterCategorySync(category);
  if (!regs)
    return std::nullopt;  // Not known synchronously.

  // Have this register synchronously (or know we can't have it).
  return debug::GetRegisterData(*regs, id);
}

void FrameSymbolDataProvider::GetRegisterAsync(debug::RegisterID id, GetRegisterCallback cb) {
  if (!frame_) {
    // Frame deleted out from under us.
    debug::MessageLoop::Current()->PostTask(
        FROM_HERE, [id, cb = std::move(cb)]() mutable { cb(RegisterUnavailableErr(id), {}); });
    return;
  }

  debug::RegisterCategory category = debug::RegisterIDToCategory(id);
  FX_DCHECK(category != debug::RegisterCategory::kNone);

  frame_->GetRegisterCategoryAsync(
      category, false,
      [id, cb = std::move(cb)](const Err& err,
                               const std::vector<debug::RegisterValue>& regs) mutable {
        if (err.has_error())
          return cb(err, {});

        auto found_reg_data = debug::GetRegisterData(regs, id);
        if (found_reg_data.empty())
          cb(RegisterUnavailableErr(id), {});
        else
          cb(Err(), std::vector<uint8_t>(found_reg_data.begin(), found_reg_data.end()));
      });
}

void FrameSymbolDataProvider::WriteRegister(debug::RegisterID id, std::vector<uint8_t> data,
                                            WriteCallback cb) {
  if (!frame_) {
    // Frame deleted out from under us.
    debug::MessageLoop::Current()->PostTask(FROM_HERE, [id, cb = std::move(cb)]() mutable {
      cb(Err("The register %s can't be written because the frame was deleted.",
             debug::RegisterIDToString(id)));
    });
    return;
  }

  frame_->WriteRegister(id, std::move(data), std::move(cb));
}

std::optional<uint64_t> FrameSymbolDataProvider::GetFrameBase() {
  if (!frame_)
    return std::nullopt;
  return frame_->GetBasePointer();
}

void FrameSymbolDataProvider::GetFrameBaseAsync(GetFrameBaseCallback cb) {
  if (!frame_) {
    debug::MessageLoop::Current()->PostTask(
        FROM_HERE, [cb = std::move(cb)]() mutable { cb(CallFrameDestroyedErr(), 0); });
    return;
  }

  frame_->GetBasePointerAsync([cb = std::move(cb)](uint64_t value) mutable { cb(Err(), value); });
}

uint64_t FrameSymbolDataProvider::GetCanonicalFrameAddress() const {
  if (!frame_)
    return 0;
  return frame_->GetCanonicalFrameAddress();
}

}  // namespace zxdb
