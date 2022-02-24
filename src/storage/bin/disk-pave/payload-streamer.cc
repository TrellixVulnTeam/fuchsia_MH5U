// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "payload-streamer.h"

#include <lib/async/default.h>
#include <lib/fidl-async/cpp/bind.h>

namespace disk_pave {

PayloadStreamer::PayloadStreamer(fidl::ServerEnd<fuchsia_paver::PayloadStream> server_end,
                                 fbl::unique_fd payload)
    : payload_(std::move(payload)) {
  fidl::BindSingleInFlightOnly(async_get_default_dispatcher(), std::move(server_end), this);
}

PayloadStreamer::~PayloadStreamer() {
  if (!eof_reached_) {
    // Reads the entire file if it wasn't completely read by the channel.
    // This is necessary due to implementation of streaming protocol which
    // forces entire file to be transferred.
    char buf[8192];
    while (read(payload_.get(), &buf, sizeof(buf)) > 0) {
    }
  }
}

void PayloadStreamer::RegisterVmo(RegisterVmoRequestView request,
                                  RegisterVmoCompleter::Sync& completer) {
  if (vmo_) {
    completer.Reply(ZX_ERR_ALREADY_BOUND);
    return;
  }

  auto status = mapper_.Map(request->vmo, 0, 0, ZX_VM_PERM_READ | ZX_VM_PERM_WRITE);
  if (status != ZX_OK) {
    completer.Reply(status);
    return;
  }

  vmo_ = std::move(request->vmo);
  completer.Reply(ZX_OK);
}

void PayloadStreamer::ReadData(ReadDataRequestView request, ReadDataCompleter::Sync& completer) {
  fuchsia_paver::wire::ReadResult result = {};
  if (!vmo_) {
    result.set_err(ZX_ERR_BAD_STATE);
    completer.Reply(std::move(result));
    return;
  }
  if (eof_reached_) {
    result.set_eof(eof_reached_);
    completer.Reply(std::move(result));
    return;
  }

  ssize_t n = read(payload_.get(), mapper_.start(), mapper_.size());
  if (n == 0) {
    eof_reached_ = true;
    result.set_eof(eof_reached_);
    completer.Reply(std::move(result));
  } else if (n < 0) {
    result.set_err(ZX_ERR_IO);
    completer.Reply(std::move(result));
  } else {
    // completer.Reply must be called from within this else block since otherwise
    // |info| will go out of scope
    fuchsia_paver::wire::ReadInfo info{.offset = 0, .size = static_cast<uint64_t>(n)};
    result.set_info(fidl::ObjectView<fuchsia_paver::wire::ReadInfo>::FromExternal(&info));
    completer.Reply(std::move(result));
  }
}

}  // namespace disk_pave
