// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/debugdata/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/sys/cpp/service_directory.h>

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  auto env_services = sys::ServiceDirectory::CreateFromNamespace();
  fuchsia::debugdata::DebugDataPtr ptr;
  env_services->Connect(ptr.NewRequest());
  ptr->LoadConfig("some_name", [](zx::vmo /*unused*/) {});

  // run until this is killed by caller.
  loop.Run();
  return 0;
}
