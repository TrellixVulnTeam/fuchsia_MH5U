// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_BIN_SESSIONMGR_PUPPET_MASTER_COMMAND_RUNNERS_COMMAND_RUNNER_H_
#define SRC_MODULAR_BIN_SESSIONMGR_PUPPET_MASTER_COMMAND_RUNNERS_COMMAND_RUNNER_H_

#include <fuchsia/modular/cpp/fidl.h>

#include "src/modular/bin/sessionmgr/storage/session_storage.h"

namespace modular {

class CommandRunner {
 public:
  CommandRunner();
  virtual ~CommandRunner();

  virtual void Execute(fidl::StringPtr story_id, StoryStorage* story_storage,
                       fuchsia::modular::StoryCommand command,
                       fit::function<void(fuchsia::modular::ExecuteResult)> done) = 0;
};

}  // namespace modular

#endif  // SRC_MODULAR_BIN_SESSIONMGR_PUPPET_MASTER_COMMAND_RUNNERS_COMMAND_RUNNER_H_
