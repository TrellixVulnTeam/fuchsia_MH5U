// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include "src/virtualization/bin/guest_manager/manager_impl.h"

int main(int argc, char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  ManagerImpl env_mgr(loop.dispatcher());
  loop.Run();
}
