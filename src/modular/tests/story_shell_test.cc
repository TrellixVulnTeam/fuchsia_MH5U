// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/modular/testing/cpp/fidl.h>
#include <fuchsia/ui/app/cpp/fidl.h>
#include <lib/modular/testing/cpp/fake_component.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/ui/scenic/cpp/view_ref_pair.h>
#include <zircon/rights.h>
#include <zircon/syscalls/object.h>
#include <zircon/types.h>

#include <tuple>

#include <gmock/gmock.h>

#include "src/lib/fsl/vmo/strings.h"
#include "src/modular/lib/modular_test_harness/cpp/fake_module.h"
#include "src/modular/lib/modular_test_harness/cpp/fake_session_shell.h"
#include "src/modular/lib/modular_test_harness/cpp/fake_story_shell.h"
#include "src/modular/lib/modular_test_harness/cpp/test_harness_fixture.h"

using testing::ElementsAre;

namespace {

class ViewRefModule : public modular_testing::FakeModule, fuchsia::ui::app::ViewProvider {
 public:
  explicit ViewRefModule(modular_testing::FakeComponent::Args args)
      : modular_testing::FakeModule(args) {}

  fuchsia::ui::views::ViewRef& view_ref() { return view_ref_; }

  bool has_created_view() { return has_created_view_; }

  static std::unique_ptr<ViewRefModule> CreateWithDefaultOptions() {
    return std::make_unique<ViewRefModule>(modular_testing::FakeComponent::Args{
        .url = modular_testing::TestHarnessBuilder::GenerateFakeUrl(),
        .sandbox_services = FakeModule::GetDefaultSandboxServices()});
  }

 private:
  // |modular::testing::FakeComponent|
  void OnCreate(fuchsia::sys::StartupInfo startup_info) override {
    component_context()->outgoing()->AddPublicService<fuchsia::ui::app::ViewProvider>(
        [this](fidl::InterfaceRequest<fuchsia::ui::app::ViewProvider> request) {
          bindings_.AddBinding(this, std::move(request));
        });
  }

  virtual void CreateView(
      ::zx::eventpair token,
      ::fidl::InterfaceRequest<::fuchsia::sys::ServiceProvider> incoming_services,
      ::fidl::InterfaceHandle<::fuchsia::sys::ServiceProvider> outgoing_services) override {}

  virtual void CreateViewWithViewRef(::zx::eventpair token,
                                     ::fuchsia::ui::views::ViewRefControl view_ref_control,
                                     ::fuchsia::ui::views::ViewRef view_ref) override {
    has_created_view_ = true;
    view_ref_ = std::move(view_ref);
  }

  bool has_created_view_ = false;
  fuchsia::ui::views::ViewRef view_ref_;
  fidl::BindingSet<fuchsia::ui::app::ViewProvider> bindings_;
};

class StoryShellTest : public modular_testing::TestHarnessFixture {
 protected:
  StoryShellTest()
      : session_shell_(modular_testing::FakeSessionShell::CreateWithDefaultOptions()),
        story_shell_({.url = modular_testing::TestHarnessBuilder::GenerateFakeUrl(),
                      .sandbox_services = {"fuchsia.modular.StoryShellContext"}}) {}

  void StartSession() { StartSessionWithInterceptedComponent(nullptr); }

  void StartSessionWithInterceptedComponent(modular_testing::FakeComponent* component) {
    modular_testing::TestHarnessBuilder builder;
    builder.InterceptSessionShell(session_shell_->BuildInterceptOptions());
    builder.InterceptStoryShell(story_shell_.BuildInterceptOptions());
    if (component) {
      builder.InterceptComponent(component->BuildInterceptOptions());
    }

    fake_module_url_ = modular_testing::TestHarnessBuilder::GenerateFakeUrl("module");
    builder.InterceptComponent(
        {.url = fake_module_url_,
         .launch_handler =
             [this](fuchsia::sys::StartupInfo startup_info,
                    fidl::InterfaceHandle<fuchsia::modular::testing::InterceptedComponent>
                        intercepted_component) {
               intercepted_modules_.push_back(
                   {std::move(startup_info), std::move(intercepted_component)});
             }});
    builder.BuildAndRun(test_harness());

    fuchsia::modular::testing::ModularService request;
    request.set_puppet_master(puppet_master_.NewRequest());
    test_harness()->ConnectToModularService(std::move(request));

    // Wait for our session shell to start.
    RunLoopUntil([this] { return session_shell_->is_running(); });
  }

  void AddModToStoryWithUrl(std::string story_name, std::string mod_name,
                            std::string parent_mod_name, std::string url) {
    fuchsia::modular::StoryPuppetMasterPtr story_puppet_master;
    puppet_master_->ControlStory(story_name, story_puppet_master.NewRequest());

    fuchsia::modular::AddMod add_mod;
    add_mod.mod_name_transitional = mod_name;
    add_mod.intent.handler = url;
    if (!parent_mod_name.empty()) {
      add_mod.surface_parent_mod_name = {parent_mod_name};
    }

    std::vector<fuchsia::modular::StoryCommand> commands(1);
    commands.at(0).set_add_mod(std::move(add_mod));

    story_puppet_master->Enqueue(std::move(commands));
    bool created = false;
    story_puppet_master->Execute([&](fuchsia::modular::ExecuteResult result) { created = true; });

    // Wait for the story to be created.
    RunLoopUntil([&] { return created; });
  }

  void AddModToStory(std::string story_name, std::string mod_name,
                     std::string parent_mod_name = "") {
    AddModToStoryWithUrl(story_name, mod_name, parent_mod_name, fake_module_url_);
  }

  void RestartStory(std::string story_name) {
    fuchsia::modular::StoryControllerPtr story_controller;
    session_shell_->story_provider()->GetController(story_name, story_controller.NewRequest());

    bool restarted = false;
    story_controller->Stop([&] {
      story_controller->RequestStart();
      restarted = true;
    });
    RunLoopUntil([&] { return restarted; });
  }

  fuchsia::modular::PuppetMasterPtr puppet_master_;
  std::unique_ptr<modular_testing::FakeSessionShell> session_shell_;
  modular_testing::FakeStoryShell story_shell_;
  std::string fake_module_url_;

  // Stories must have modules in them so the stories created above
  // contain fake intercepted modules. This list holds onto them so that
  // they can be successfully launched and don't die immediately.
  std::vector<std::tuple<fuchsia::sys::StartupInfo,
                         fidl::InterfaceHandle<fuchsia::modular::testing::InterceptedComponent>>>
      intercepted_modules_;
};

TEST_F(StoryShellTest, GetsModuleMetadata) {
  StartSession();

  std::vector<std::string> surface_ids_added;
  story_shell_.set_on_add_surface([&](fuchsia::modular::ViewConnection view_connection,
                                      fuchsia::modular::SurfaceInfo2 surface_info) {
    surface_ids_added.push_back(view_connection.surface_id);
  });

  AddModToStory("story1", "mod1");
  AddModToStory("story1", "mod2", {"mod1"} /* surface relation parent */);
  // Wait for the story shell to be notified of the new modules.
  RunLoopUntil([&] { return surface_ids_added.size() == 2; });
  EXPECT_THAT(surface_ids_added, ElementsAre("mod1", "mod1:mod2"));

  // Stop the story shell and restart it. Expect to see the same mods notified
  // to the story shell in the same order.
  surface_ids_added.clear();
  RestartStory("story1");
  RunLoopUntil([&] { return surface_ids_added.size() == 2; });
  EXPECT_THAT(surface_ids_added, ElementsAre("mod1", "mod1:mod2"));
}

TEST_F(StoryShellTest, GetsViewRef) {
  StartSession();

  std::vector<bool> has_view_refs;
  story_shell_.set_on_add_surface([&](fuchsia::modular::ViewConnection view_connection,
                                      fuchsia::modular::SurfaceInfo2 surface_info) {
    has_view_refs.push_back(surface_info.has_view_ref());
  });

  AddModToStory("story1", "mod1");
  AddModToStory("story1", "mod2", {"mod1"} /* surface relation parent */);
  // Wait for the story shell to be notified of the new modules.
  RunLoopUntil([&] { return has_view_refs.size() == 2; });
  EXPECT_THAT(has_view_refs, ElementsAre(true, true));

  // Stop the story shell and restart it. Expect to see the same mods notified
  // to the story shell in the same order.
  has_view_refs.clear();
  RestartStory("story1");
  RunLoopUntil([&] { return has_view_refs.size() == 2; });
  EXPECT_THAT(has_view_refs, ElementsAre(true, true));
}

TEST_F(StoryShellTest, GetsCorrectViewRef) {
  auto view_ref_module = ViewRefModule::CreateWithDefaultOptions();
  StartSessionWithInterceptedComponent(view_ref_module.get());

  bool have_seen_view_ref = false;
  fuchsia::ui::views::ViewRef seen_view_ref;

  story_shell_.set_on_add_surface([&](fuchsia::modular::ViewConnection view_connection,
                                      fuchsia::modular::SurfaceInfo2 surface_info) {
    seen_view_ref = fidl::Clone(surface_info.view_ref());
    have_seen_view_ref = true;
  });

  AddModToStoryWithUrl("story1", "mod1", "", view_ref_module->url());
  // Wait for the story shell to be notified of the new modules.
  RunLoopUntil([&] { return have_seen_view_ref; });
  RunLoopUntil([&] { return view_ref_module->has_created_view(); });

  // Get info about the |view_ref|.
  zx_info_handle_basic_t seen_view_info;
  zx_status_t seen_view_status =
      zx_object_get_info(seen_view_ref.reference.get(), ZX_INFO_HANDLE_BASIC, &seen_view_info,
                         sizeof(seen_view_info), nullptr, nullptr);
  zx_info_handle_basic_t mod_view_info;
  zx_status_t mod_view_status =
      zx_object_get_info(view_ref_module->view_ref().reference.get(), ZX_INFO_HANDLE_BASIC,
                         &mod_view_info, sizeof(mod_view_info), nullptr, nullptr);
  EXPECT_EQ(mod_view_status, ZX_OK);
  EXPECT_EQ(seen_view_status, ZX_OK);

  EXPECT_EQ(mod_view_info.koid, seen_view_info.koid);
}

}  // namespace
