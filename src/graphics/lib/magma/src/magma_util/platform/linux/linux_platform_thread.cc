// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform_thread.h"

namespace magma {

bool PlatformThreadHelper::SetProfile(PlatformHandle* profile) { return false; }

std::string PlatformThreadHelper::GetCurrentThreadName() { return ""; }

void PlatformThreadHelper::SetCurrentThreadName(const std::string& name) {}

}  // namespace magma
