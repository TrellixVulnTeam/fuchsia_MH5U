// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_FIDL_CODEC_DISPLAY_HANDLE_H_
#define SRC_LIB_FIDL_CODEC_DISPLAY_HANDLE_H_

#include <zircon/syscalls/debug.h>
#include <zircon/types.h>

#include <ostream>

#include "src/lib/fidl_codec/printer.h"

namespace fidl_codec {

// Displays a handle.
// If handle.operation == kNoHandleDisposition, only the info part of zx_handle_disposition_t is
// used and printed.
// Else, the handle comes from the write of an "etc" function (zx_channel_write_etc or write part of
// a zx_channel_call_etc). In that case, the full disposition is used to print the handle.
void DisplayHandle(const zx_handle_disposition_t& handle, PrettyPrinter& printer);

}  // namespace fidl_codec

#endif  // SRC_LIB_FIDL_CODEC_DISPLAY_HANDLE_H_
