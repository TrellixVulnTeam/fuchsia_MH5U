// Copyright 2015 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/trace-provider/provider.h>

#include "garnet/bin/ui/benchmarks/image_grid_cpp/image_grid_view.h"
#include "src/lib/ui/base_view/view_provider_component.h"

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  trace::TraceProviderWithFdio trace_provider(loop.dispatcher());

  scenic::ViewProviderComponent component(
      [](scenic::ViewContext view_context) {
        return std::make_unique<image_grid::ImageGridView>(std::move(view_context));
      },
      &loop);

  loop.Run();
  return 0;
}
