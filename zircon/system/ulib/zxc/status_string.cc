// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/status.h>
#include <zircon/status.h>

const char* zx::status<>::status_string() const { return zx_status_get_string(status_value()); }
