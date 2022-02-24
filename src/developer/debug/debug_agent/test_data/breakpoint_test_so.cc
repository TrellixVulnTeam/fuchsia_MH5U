// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include <atomic>

#include "src/developer/debug/debug_agent/test_data/test_so_symbols.h"

int gWatchpointVariable = 0;

int InsertBreakpointFunction(int c) {
  printf("Should receive breakpoint!\n");
  fflush(stdout);
  return 10 * c;
}

int InsertBreakpointFunction2(int c) {
  printf("Should also receive a breakpoint!\n");
  fflush(stdout);
  return 9000 * c * c;
}

void AnotherFunctionForKicks() {}

void MultithreadedFunctionToBreakOn() {
  // This counter is meant to be a bare-bones example of multi-threaded logic.
  static std::atomic<int> global_counter = 0;
  global_counter++;
}

void WatchpointFunction() {
  printf("gWatchpointVariable address: 0x%p\n", &gWatchpointVariable);
  fflush(stdout);
  gWatchpointVariable++;
}
