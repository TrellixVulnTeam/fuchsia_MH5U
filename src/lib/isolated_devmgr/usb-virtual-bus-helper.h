// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_ISOLATED_DEVMGR_USB_VIRTUAL_BUS_HELPER_H_
#define SRC_LIB_ISOLATED_DEVMGR_USB_VIRTUAL_BUS_HELPER_H_
#include <zircon/types.h>
namespace usb_virtual_bus_helper {

zx_status_t WaitForAnyFile(int dirfd, int event, const char* name, void* cookie);

zx_status_t WaitForFile(int dirfd, int event, const char* fn, void* name);
}  // namespace usb_virtual_bus_helper
#endif  // SRC_LIB_ISOLATED_DEVMGR_USB_VIRTUAL_BUS_HELPER_H_
