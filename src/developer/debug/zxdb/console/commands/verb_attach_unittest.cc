// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/console/commands/verb_attach.h"

#include <gtest/gtest.h>

#include "src/developer/debug/zxdb/console/commands/attach_command_test.h"

namespace zxdb {

namespace {

class VerbAttach : public AttachCommandTest {};

// This should match ZX_MAX_NAME_LEN, but we don't want to include zircon headers here.
constexpr size_t kZirconMaxNameLength = 32;

}  // namespace

TEST_F(VerbAttach, Bad) {
  // Missing argument.
  console().ProcessInputLine("attach");
  auto event = console().GetOutputEvent();
  EXPECT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ("Wrong number of arguments to attach.", event.output.AsString());

  // Can't attach to a process by filter.
  console().ProcessInputLine("process attach foo");
  event = console().GetOutputEvent();
  EXPECT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ("Attaching by process name (a non-numeric argument)\nonly supports the \"job\" noun.",
            event.output.AsString());
}

TEST_F(VerbAttach, Koid) {
  constexpr uint64_t kKoid = 7890u;
  const char kCommand[] = "attach 7890";
  console().ProcessInputLine(kCommand);

  // This should create a new process context and give "process 2" because the default console test
  // harness makes a mock running process #1 by default.
  ASSERT_TRUE(attach_remote_api()->last_attach);
  ASSERT_EQ(kKoid, attach_remote_api()->last_attach->request.koid);
  debug_ipc::AttachReply reply;
  reply.status = debug::Status();
  reply.koid = kKoid;
  reply.name = "some process";
  attach_remote_api()->last_attach->cb(Err(), reply);
  EXPECT_TRUE(attach_remote_api()->filters.empty());

  auto event = console().GetOutputEvent();
  EXPECT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ("Attached Process 2 state=Running koid=7890 name=\"some process\"",
            event.output.AsString());

  // Attaching to the same process again should give an error.
  console().ProcessInputLine(kCommand);
  event = console().GetOutputEvent();
  EXPECT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ("Process 7890 is already being debugged.", event.output.AsString());
}

TEST_F(VerbAttach, Filter) {
  // Note: the commands in this test issue a warning because there's no attached job. This warning
  // is currently implemented to be output as a separate output event which we ignore separately to
  // avoid having to hardcode the entire warning text in this test. If the implementation changes
  // how this is output, this test may need to change somewhat.

  // Normal filter case.
  console().ProcessInputLine("attach foo");
  console().GetOutputEvent();  // Eat warning.
  auto event = console().GetOutputEvent();
  EXPECT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ(
      "Waiting for process matching \"foo\".\n"
      "Type \"filter\" to see the current filters.",
      event.output.AsString());

  // Extra long filter case.
  const std::string kSuperLongName = "super_long_name_with_over_32_characters";
  console().ProcessInputLine("attach " + kSuperLongName);
  console().GetOutputEvent();  // Eat warning.
  event = console().GetOutputEvent();
  EXPECT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ("The filter is trimmed to " + std::to_string(kZirconMaxNameLength) +
                " characters because it's the maximum length for a process name in Zircon.",
            event.output.AsString());
  event = console().GetOutputEvent();
  EXPECT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ("Waiting for process matching \"" + kSuperLongName.substr(0, kZirconMaxNameLength) +
                "\".\n"
                "Type \"filter\" to see the current filters.",
            event.output.AsString());

  // Don't allow attaching by wildcard without a job. This one doesn't have the job warning since
  // it's an error case.
  console().ProcessInputLine("attach *");
  event = console().GetOutputEvent();
  EXPECT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ("Use a specific job (\"job 3 attach *\") when attaching to all processes.",
            event.output.AsString());

  // Wildcard within a job is OK.
  console().ProcessInputLine("job 1 attach *");
  console().GetOutputEvent();  // Eat warning.
  event = console().GetOutputEvent();
  EXPECT_EQ(MockConsole::OutputEvent::Type::kOutput, event.type);
  EXPECT_EQ(
      "Waiting for process matching \"*\".\n"
      "Type \"filter\" to see the current filters.",
      event.output.AsString());
}

}  // namespace zxdb
