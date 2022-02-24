// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/timekeeper/test_clock.h"

namespace timekeeper {

TestClock::TestClock() = default;

TestClock::~TestClock() = default;

zx_status_t TestClock::GetUtcTime(zx_time_t* time) const {
  *time = current_time_;
  return ZX_OK;
}

zx_time_t TestClock::GetMonotonicTime() const { return current_time_; }

}  // namespace timekeeper
