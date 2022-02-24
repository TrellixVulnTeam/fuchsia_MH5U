// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/session/cpp/fidl.h>
#include <fuchsia/modular/testing/cpp/fidl.h>
#include <fuchsia/testing/modular/cpp/fidl.h>

#include <gmock/gmock.h>
#include <sdk/lib/modular/testing/cpp/fake_agent.h>

#include "src/modular/lib/modular_test_harness/cpp/fake_module.h"
#include "src/modular/lib/modular_test_harness/cpp/fake_session_shell.h"
#include "src/modular/lib/modular_test_harness/cpp/launch_counter.h"
#include "src/modular/lib/modular_test_harness/cpp/test_harness_fixture.h"

namespace {

class AgentSessionRestartTest : public modular_testing::TestHarnessFixture {
 protected:
  AgentSessionRestartTest() = default;

  static modular_testing::TestHarnessBuilder::InterceptOptions AddSandboxServices(
      const std::vector<std::string>& service_names,
      modular_testing::TestHarnessBuilder::InterceptOptions options) {
    for (const auto& service_name : service_names) {
      options.sandbox_services.push_back(service_name);
    }
    return options;
  }
};

// Session agents are restarted on crash.
TEST_F(AgentSessionRestartTest, AgentCanRestartSession) {
  modular_testing::LaunchCounter agent_launch_counter;
  modular_testing::LaunchCounter session_launch_counter;

  auto agent = modular_testing::FakeAgent::CreateWithDefaultOptions();
  auto session_shell = modular_testing::FakeSessionShell::CreateWithDefaultOptions();

  fuchsia::modular::testing::TestHarnessSpec spec;
  spec.mutable_sessionmgr_config()->mutable_session_agents()->push_back(agent->url());

  modular_testing::TestHarnessBuilder builder(std::move(spec));
  builder.InterceptSessionShell(
      session_launch_counter.WrapInterceptOptions(session_shell->BuildInterceptOptions()));
  builder.InterceptComponent(agent_launch_counter.WrapInterceptOptions(AddSandboxServices(
      {fuchsia::modular::SessionRestartController::Name_}, agent->BuildInterceptOptions())));
  builder.BuildAndRun(test_harness());

  // Use the session shell's startup to indicate that the runtime is up.
  RunLoopUntil([&] { return session_shell->is_running() && agent->is_running(); });

  // Issue a restart command from the Agent.
  auto session_restart_controller =
      agent->component_context()->svc()->Connect<fuchsia::modular::SessionRestartController>();
  session_restart_controller->Restart();

  // Wait for the session shell and agent to restart.
  RunLoopUntil([&] {
    return agent_launch_counter.launch_count() >= 2 && session_launch_counter.launch_count() >= 2;
  });
}

}  // namespace
