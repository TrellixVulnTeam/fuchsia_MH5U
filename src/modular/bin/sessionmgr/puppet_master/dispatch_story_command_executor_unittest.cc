// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/bin/sessionmgr/puppet_master/dispatch_story_command_executor.h"

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/fidl/cpp/string.h>
#include <lib/gtest/real_loop_fixture.h>

#include <map>
#include <memory>

#include <gtest/gtest.h>

#include "src/modular/lib/async/cpp/operation.h"

namespace modular {
namespace {

class TestCommandRunner : public CommandRunner {
 public:
  using ExecuteFunc = fit::function<fuchsia::modular::ExecuteStatus(
      fidl::StringPtr, fuchsia::modular::StoryCommand)>;
  TestCommandRunner(ExecuteFunc func, bool delay_done = false)
      : func_(std::move(func)), delay_done_(delay_done) {}
  ~TestCommandRunner() override = default;

  void Execute(fidl::StringPtr story_id, StoryStorage* const story_storage,
               fuchsia::modular::StoryCommand command,
               fit::function<void(fuchsia::modular::ExecuteResult)> done) override {
    // Post the task on the dispatcher loop to simulate a long-running task.
    async::PostTask(async_get_default_dispatcher(), [this, story_id, command = std::move(command),
                                                     done = std::move(done)]() mutable {
      auto status = func_(story_id, std::move(command));
      fuchsia::modular::ExecuteResult result;
      result.status = status;
      if (delay_done_) {
        async::PostTask(
            async_get_default_dispatcher(),
            [result = std::move(result), done = std::move(done)]() { done(std::move(result)); });
      } else {
        done(std::move(result));
      }
    });
  }

  ExecuteFunc func_;
  bool delay_done_;
};

class DispatchStoryCommandExecutorTest : public gtest::RealLoopFixture {
 protected:
  void SetUp() override {
    gtest::RealLoopFixture::SetUp();
    session_storage_ = std::make_unique<SessionStorage>();
  }

  void Reset() {
    executor_ = std::make_unique<DispatchStoryCommandExecutor>(session_storage_.get(),
                                                               std::move(command_runners_));
  }

  void AddCommandRunner(fuchsia::modular::StoryCommand::Tag tag,
                        TestCommandRunner::ExecuteFunc func, bool delay_done = false) {
    command_runners_.emplace(tag, new TestCommandRunner(std::move(func), delay_done));
  }

  std::unique_ptr<SessionStorage> session_storage_;
  std::unique_ptr<StoryCommandExecutor> executor_;
  std::map<fuchsia::modular::StoryCommand::Tag, std::unique_ptr<CommandRunner>> command_runners_;
};

TEST_F(DispatchStoryCommandExecutorTest, InvalidStory) {
  Reset();

  std::vector<fuchsia::modular::StoryCommand> commands;
  fuchsia::modular::ExecuteResult result;
  bool done{false};
  executor_->ExecuteCommands("id", std::move(commands), [&](fuchsia::modular::ExecuteResult r) {
    done = true;
    result = std::move(r);
  });

  RunLoopUntil([&]() { return done; });
  EXPECT_EQ(fuchsia::modular::ExecuteStatus::INVALID_STORY_ID, result.status);
}

TEST_F(DispatchStoryCommandExecutorTest, Dispatching) {
  auto expected_story_id = session_storage_->CreateStory("story", {});

  // We expect that each command is dispatched to the command runner for that
  // command.
  int actual_execute_count{0};
  for (auto tag : {fuchsia::modular::StoryCommand::Tag::kAddMod,
                   fuchsia::modular::StoryCommand::Tag::kRemoveMod}) {
    AddCommandRunner(tag, [tag, &actual_execute_count, expected_story_id](
                              fidl::StringPtr story_id, fuchsia::modular::StoryCommand command) {
      ++actual_execute_count;
      EXPECT_EQ(tag, command.Which());
      EXPECT_EQ(expected_story_id, story_id.value());
      return fuchsia::modular::ExecuteStatus::OK;
    });
  }

  Reset();

  std::vector<fuchsia::modular::StoryCommand> commands;
  commands.resize(2);
  commands[0].set_add_mod(fuchsia::modular::AddMod());
  commands[1].set_remove_mod(fuchsia::modular::RemoveMod());

  fuchsia::modular::ExecuteResult result;
  bool done{false};
  executor_->ExecuteCommands(expected_story_id, std::move(commands),
                             [&](fuchsia::modular::ExecuteResult r) {
                               done = true;
                               result = std::move(r);
                             });

  RunLoopUntil([&]() { return done; });
  EXPECT_EQ(fuchsia::modular::ExecuteStatus::OK, result.status);
  EXPECT_EQ(expected_story_id, result.story_id.value());
  EXPECT_EQ(2, actual_execute_count);
}

TEST_F(DispatchStoryCommandExecutorTest, Sequential) {
  auto story_id = session_storage_->CreateStory("story", {});

  // Commands are run sequentially.
  std::vector<std::string> names;
  // We're going to run an fuchsia::modular::AddMod command first, but we'll
  // push the "logic" onto the async loop so that, if the implementation
  // posted all of our CommandRunner logic sequentially on the async loop, it
  // would run after the commands following this one. That's what |delay_done|
  // does.
  AddCommandRunner(
      fuchsia::modular::StoryCommand::Tag::kAddMod,
      [&](fidl::StringPtr story_id, fuchsia::modular::StoryCommand command) {
        names.push_back(command.add_mod().mod_name_transitional.value_or(""));
        return fuchsia::modular::ExecuteStatus::OK;
      },
      true /* delay_done */);
  AddCommandRunner(fuchsia::modular::StoryCommand::Tag::kRemoveMod,
                   [&](fidl::StringPtr story_id, fuchsia::modular::StoryCommand command) {
                     names.push_back(command.remove_mod().mod_name_transitional.value_or(""));
                     return fuchsia::modular::ExecuteStatus::OK;
                   });

  Reset();

  std::vector<fuchsia::modular::StoryCommand> commands;
  commands.resize(2);
  fuchsia::modular::AddMod add_mod;
  add_mod.mod_name_transitional = "one";
  commands[0].set_add_mod(std::move(add_mod));
  fuchsia::modular::RemoveMod remove_mod;
  remove_mod.mod_name_transitional = "two";
  commands[1].set_remove_mod(std::move(remove_mod));

  bool done{false};
  executor_->ExecuteCommands(story_id, std::move(commands),
                             [&](fuchsia::modular::ExecuteResult) { done = true; });
  RunLoopUntil([&]() { return done; });

  EXPECT_EQ(2u, names.size());
  EXPECT_EQ("one", names[0]);
  EXPECT_EQ("two", names[1]);
}

TEST_F(DispatchStoryCommandExecutorTest, ErrorsAbortEarly) {
  auto story_id = session_storage_->CreateStory("story", {});

  // Commands after those that report an error don't run. The reported error
  // code is returned.
  bool second_command_ran{false};
  AddCommandRunner(fuchsia::modular::StoryCommand::Tag::kAddMod,
                   [](fidl::StringPtr story_id, fuchsia::modular::StoryCommand command) {
                     return fuchsia::modular::ExecuteStatus::INVALID_COMMAND;
                   });
  AddCommandRunner(fuchsia::modular::StoryCommand::Tag::kRemoveMod,
                   [&](fidl::StringPtr story_id, fuchsia::modular::StoryCommand command) {
                     second_command_ran = true;
                     return fuchsia::modular::ExecuteStatus::OK;
                   });

  Reset();

  std::vector<fuchsia::modular::StoryCommand> commands;
  commands.resize(2);
  commands[0].set_add_mod(fuchsia::modular::AddMod());
  commands[1].set_remove_mod(fuchsia::modular::RemoveMod());

  bool done{false};
  fuchsia::modular::ExecuteResult result;
  executor_->ExecuteCommands(story_id, std::move(commands), [&](fuchsia::modular::ExecuteResult r) {
    done = true;
    result = std::move(r);
  });
  RunLoopUntil([&]() { return done; });

  EXPECT_EQ(fuchsia::modular::ExecuteStatus::INVALID_COMMAND, result.status);
  EXPECT_FALSE(second_command_ran);
}

}  // namespace
}  // namespace modular
