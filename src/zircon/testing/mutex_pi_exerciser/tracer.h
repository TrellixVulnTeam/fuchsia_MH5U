// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_ZIRCON_TESTING_MUTEX_PI_EXERCISER_TRACER_H_
#define SRC_ZIRCON_TESTING_MUTEX_PI_EXERCISER_TRACER_H_

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/trace-engine/types.h>
#include <lib/trace-provider/provider.h>
#include <stdio.h>

#include <memory>

#include <fbl/macros.h>

class Tracer {
 public:
  Tracer() = default;
  ~Tracer() = default;
  DISALLOW_COPY_ASSIGN_AND_MOVE(Tracer);

  static void Trace(trace_scope_t scope, const char* fmt, ...) __printflike(2, 3);

  zx_status_t Start();

 private:
  std::unique_ptr<async::Loop> loop_;
  std::unique_ptr<trace::TraceProviderWithFdio> trace_provider_;
};

#endif  // SRC_ZIRCON_TESTING_MUTEX_PI_EXERCISER_TRACER_H_
