// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/modular/bin/sessionmgr/puppet_master/command_runners/add_mod_command_runner.h"

#include <lib/fidl/cpp/clone.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/syslog/cpp/macros.h>

#include <gtest/gtest.h>

#include "src/lib/fsl/types/type_converters.h"
#include "src/lib/fsl/vmo/strings.h"
#include "src/modular/lib/testing/test_with_session_storage.h"

namespace modular {
namespace {

class AddModCommandRunnerTest : public modular_testing::TestWithSessionStorage {
 public:
  void SetUp() override {
    modular_testing::TestWithSessionStorage::SetUp();
    session_storage_ = MakeSessionStorage();
    story_id_ = session_storage_->CreateStory("story", /*annotations=*/{});
    story_storage_ = GetStoryStorage(session_storage_.get(), story_id_);
    runner_ = MakeRunner();
  }

 protected:
  // This method compares intents field by field, where fuchsia::mem::Buffers
  // are compared via their contents.
  bool AreIntentsEqual(const fuchsia::modular::Intent& old_intent,
                       const fuchsia::modular::Intent& new_intent) {
    if (old_intent.handler != new_intent.handler) {
      return false;
    }

    if (old_intent.action != new_intent.action) {
      return false;
    }

    return true;
  }

  std::unique_ptr<AddModCommandRunner> MakeRunner() {
    return std::make_unique<AddModCommandRunner>();
  }

  fuchsia::modular::StoryCommand MakeAddModCommand(const std::string& mod_name,
                                                   const std::string& parent_mod_name,
                                                   float surface_emphasis,
                                                   const fuchsia::modular::Intent& intent) {
    fuchsia::modular::AddMod add_mod;
    add_mod.mod_name_transitional = mod_name;
    if (!parent_mod_name.empty()) {
      add_mod.surface_parent_mod_name.emplace({parent_mod_name});
    }
    add_mod.surface_relation.emphasis = surface_emphasis;
    intent.Clone(&add_mod.intent);
    fuchsia::modular::StoryCommand command;
    command.set_add_mod(std::move(add_mod));
    return command;
  }

  fuchsia::modular::Intent CreateEmptyIntent(const std::string& action,
                                             const std::string& handler = "") {
    fuchsia::modular::Intent intent;
    intent.action = "intent_action";
    if (!handler.empty()) {
      intent.handler = "mod_url";
    }
    return intent;
  }

  std::unique_ptr<AddModCommandRunner> runner_;
  std::unique_ptr<SessionStorage> session_storage_;
  std::shared_ptr<StoryStorage> story_storage_;
  std::string story_id_;
};

TEST_F(AddModCommandRunnerTest, ExecuteIntentWithIntentHandler) {
  // Set up command
  auto intent = CreateEmptyIntent("intent_action", "mod_url");
  auto command = MakeAddModCommand("mod", "parent_mod", 0.5, intent);

  // Run the command and assert results.
  bool done = false;
  runner_->Execute(story_id_, story_storage_.get(), std::move(command),
                   [&](fuchsia::modular::ExecuteResult result) {
                     ASSERT_EQ(fuchsia::modular::ExecuteStatus::OK, result.status);
                     done = true;
                   });
  RunLoopUntil([&] { return done; });

  done = false;
  std::vector<std::string> full_path{"parent_mod", "mod"};
  auto module_data = story_storage_->ReadModuleData(std::move(full_path));
  EXPECT_EQ("mod_url", module_data->module_url());
  EXPECT_EQ(full_path, module_data->module_path());
  EXPECT_FALSE(module_data->module_deleted());
  EXPECT_EQ(fuchsia::modular::ModuleSource::EXTERNAL, module_data->module_source());
  EXPECT_EQ(0.5, module_data->surface_relation().emphasis);
  EXPECT_TRUE(AreIntentsEqual(intent, module_data->intent()));
}

// Explicitly leave surface_parent_mod_name as null when providing the Intent.
// We should tolerate this, and initialize it to a zero-length vector
// internally.
TEST_F(AddModCommandRunnerTest, ExecuteIntentWithIntentHandler_NoParent) {
  auto intent = CreateEmptyIntent("intent_action", "mod_url");
  auto command = MakeAddModCommand("mod", "" /* parent mod is null */, 0.5, intent);

  // Run the command and assert results.
  bool done = false;
  runner_->Execute(story_id_, story_storage_.get(), std::move(command),
                   [&](fuchsia::modular::ExecuteResult result) {
                     ASSERT_EQ(fuchsia::modular::ExecuteStatus::OK, result.status);
                     done = true;
                   });
  RunLoopUntil([&] { return done; });

  std::vector<std::string> full_path{"mod"};
  auto module_data = story_storage_->ReadModuleData(std::move(full_path));
  EXPECT_EQ("mod_url", module_data->module_url());
  EXPECT_EQ(full_path, module_data->module_path());
  EXPECT_FALSE(module_data->module_deleted());
  EXPECT_EQ(fuchsia::modular::ModuleSource::EXTERNAL, module_data->module_source());
  EXPECT_EQ(0.5, module_data->surface_relation().emphasis);
  EXPECT_TRUE(AreIntentsEqual(intent, module_data->intent()));
}

TEST_F(AddModCommandRunnerTest, ExecuteNoModulesFound) {
  fuchsia::modular::Intent intent;
  fuchsia::modular::AddMod add_mod;
  intent.Clone(&add_mod.intent);
  add_mod.mod_name.push_back("mymod");
  add_mod.intent.action = "intent_action";
  fuchsia::modular::StoryCommand command;
  command.set_add_mod(std::move(add_mod));

  bool done{};
  runner_->Execute(story_id_, story_storage_.get(), std::move(command),
                   [&](fuchsia::modular::ExecuteResult result) {
                     EXPECT_EQ(fuchsia::modular::ExecuteStatus::NO_MODULES_FOUND, result.status);
                     EXPECT_EQ("Module resolution via Intent.action is deprecated.",
                               result.error_message);
                     done = true;
                   });
  RunLoopUntil([&] { return done; });
}

TEST_F(AddModCommandRunnerTest, AcceptsModNameTransitional) {
  // Set up command
  auto intent = CreateEmptyIntent("intent_action", "mod_url");
  auto command = MakeAddModCommand("mod", "parent_mod", 0.5, intent);

  // Keep only `mod_name_transitional`
  command.add_mod().mod_name.clear();

  // Add a mod to begin with.
  bool done{};
  runner_->Execute(story_id_, story_storage_.get(), std::move(command),
                   [&](fuchsia::modular::ExecuteResult result) {
                     ASSERT_EQ(fuchsia::modular::ExecuteStatus::OK, result.status);
                     done = true;
                   });
  RunLoopUntil([&] { return done; });
}

}  // namespace
}  // namespace modular
