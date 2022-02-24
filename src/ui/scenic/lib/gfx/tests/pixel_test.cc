// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/gfx/tests/pixel_test.h"

#include <fuchsia/ui/gfx/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>
#include <zircon/status.h>

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace gfx {

namespace {

// 15s is not enough time for some bots to launch Scenic, see fxbug.dev/61751.
constexpr zx::duration kScreenshotTimeout = zx::sec(15), kPresentTimeout = zx::sec(15),
                       kIndirectPresentTimeout = zx::sec(90);

// These tests need Scenic and RootPresenter at minimum, which expand to the
// dependencies below. Using |gtest::TestWithEnvironmentFixture|, we use
// |fuchsia.sys.Environment| and |fuchsia.sys.Loader| from the system (declared
// in our *.cmx sandbox) and launch these other services in the environment we
// create in our test fixture.
//
// Another way to do this would be to whitelist these services in our sandbox
// and inject/start them via the |fuchsia.test| facet. However that has the
// disadvantage that it uses one instance of those services across all tests in
// the binary, making each test not hermetic wrt. the others. A trade-off is
// that the |gtest::TestWithEnvironmentFixture| method is more verbose.
const std::map<std::string, std::string> LocalServices() {
  return {{"fuchsia.hardware.display.Provider",
           "fuchsia-pkg://fuchsia.com/fake-hardware-display-controller-provider#meta/hdcp.cmx"},
          {"fuchsia.tracing.provider.Registry",
           "fuchsia-pkg://fuchsia.com/trace_manager#meta/trace_manager.cmx"},
          {"fuchsia.ui.policy.Presenter",
           "fuchsia-pkg://fuchsia.com/root_presenter#meta/root_presenter.cmx"},
          {"fuchsia.ui.composition.Allocator", "fuchsia-pkg://fuchsia.com/scenic#meta/scenic.cmx"},
          // TODO(fxbug.dev/82655): Remove this after migrating to RealmBuilder.
          {"fuchsia.ui.lifecycle.LifecycleController",
           "fuchsia-pkg://fuchsia.com/scenic#meta/scenic.cmx"},
          {"fuchsia.ui.scenic.Scenic", "fuchsia-pkg://fuchsia.com/scenic#meta/scenic.cmx"},
          {"fuchsia.ui.annotation.Registry", "fuchsia-pkg://fuchsia.com/scenic#meta/scenic.cmx"},
          {"fuchsia.ui.shortcut.Manager",
           "fuchsia-pkg://fuchsia.com/shortcut#meta/shortcut_manager.cmx"}};
}

// Allow these global services.
const std::vector<std::string> GlobalServices() {
  return {"fuchsia.vulkan.loader.Loader", "fuchsia.sysmem.Allocator"};
}

}  // namespace

TestSession::TestSession(fuchsia::ui::scenic::Scenic* scenic,
                         const DisplayDimensions& display_dimensions)
    : session(scenic),
      display_dimensions(display_dimensions),
      compositor(&session),
      layer_stack(&session),
      layer(&session),
      renderer(&session),
      scene(&session),
      ambient_light(&session) {
  compositor.SetLayerStack(layer_stack);
  layer_stack.AddLayer(layer);
  layer.SetSize(display_dimensions.width, display_dimensions.height);
  layer.SetRenderer(renderer);
  scene.AddLight(ambient_light);
  ambient_light.SetColor(1.f, 1.f, 1.f);
}

PixelTest::PixelTest(const std::string& environment_label)
    : environment_label_(environment_label) {}

void PixelTest::SetUp() {
  TestWithEnvironmentFixture::SetUp();

  environment_ = CreateNewEnclosingEnvironment(environment_label_, CreateServices());
  WaitForEnclosingEnvToStart(environment_.get());

  // Connects to scenic lifecycle controller in order to shutdown scenic at the end of the test.
  // This ensures the correct ordering of shutdown under CFv1: first scenic, then the fake display
  // controller.
  //
  // TODO(fxbug.dev/82655): Remove this after migrating to RealmBuilder.
  environment_->ConnectToService<fuchsia::ui::lifecycle::LifecycleController>(
      scenic_lifecycle_controller_.NewRequest());

  environment_->ConnectToService(scenic_.NewRequest());
  scenic_.set_error_handler([](zx_status_t status) {
    FAIL() << "Lost connection to Scenic: " << zx_status_get_string(status);
  });

  environment_->ConnectToService(annotation_registry_.NewRequest());
  annotation_registry_.set_error_handler([](zx_status_t status) {
    FAIL() << "Lost connection to Annotation Registry: " << zx_status_get_string(status);
  });
}

void PixelTest::TearDown() {
  // Avoid spurious errors since we are about to kill scenic.
  //
  // TODO(fxbug.dev/82655): Remove this after migrating to RealmBuilder.
  annotation_registry_.set_error_handler(nullptr);
  scenic_.set_error_handler(nullptr);

  zx_status_t terminate_status = scenic_lifecycle_controller_->Terminate();
  FX_CHECK(terminate_status == ZX_OK)
      << "Failed to terminate Scenic with status: " << zx_status_get_string(terminate_status);
}

std::unique_ptr<sys::testing::EnvironmentServices> PixelTest::CreateServices() {
  auto services = TestWithEnvironmentFixture::CreateServices();

  for (const auto& entry : LocalServices()) {
    services->AddServiceWithLaunchInfo({.url = entry.second}, entry.first);
  }

  for (const auto& entry : GlobalServices()) {
    services->AllowParentService(entry);
  }

  return services;
}

scenic::Screenshot PixelTest::TakeScreenshot() {
  fuchsia::ui::scenic::ScreenshotData screenshot_out;
  scenic_->TakeScreenshot(
      [this, &screenshot_out](fuchsia::ui::scenic::ScreenshotData screenshot, bool status) {
        EXPECT_TRUE(status) << "Failed to take screenshot";
        screenshot_out = std::move(screenshot);
        QuitLoop();
      });
  EXPECT_FALSE(RunLoopWithTimeout(kScreenshotTimeout)) << "Timed out waiting for screenshot.";
  return scenic::Screenshot(screenshot_out);
}

fuchsia::ui::views::ViewToken PixelTest::CreatePresentationViewToken(bool clobber) {
  FX_CHECK(environment_) << "Environment has not been initialized.";

  auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();

  auto presenter = environment_->ConnectToService<fuchsia::ui::policy::Presenter>();
  presenter.set_error_handler(
      [](zx_status_t status) { FAIL() << "presenter: " << zx_status_get_string(status); });
  if (clobber) {
    presenter->PresentOrReplaceView(std::move(view_holder_token), nullptr);
  } else {
    presenter->PresentView(std::move(view_holder_token), nullptr);
  }

  return std::move(view_token);
}

scenic::ViewContext PixelTest::CreatePresentationContext(bool clobber) {
  FX_CHECK(scenic()) << "Scenic is not connected.";

  return {
      .session_and_listener_request = scenic::CreateScenicSessionPtrAndListenerRequest(scenic()),
      .view_token = CreatePresentationViewToken(clobber),
  };
}

void PixelTest::RunUntilIndirectPresent(scenic::TestView* view) {
  // Typical sequence of events:
  // 1. We set up a view bound as a |SessionListener|.
  // 2. The view sends its initial |Present| to get itself connected, without
  //    a callback.
  // 3. We call |RunUntilIndirectPresent| which sets a present callback on our
  //    |TestView|.
  // 4. |RunUntilIndirectPresent| runs the message loop, which allows the view to
  //    receive a Scenic event telling us our metrics.
  // 5. In response, the view sets up the scene graph with the test scene.
  // 6. The view calls |Present| with the callback set in |RunUntilIndirectPresent|.
  // 7. The still-running message loop eventually dispatches the present
  //    callback, which quits the loop.

  view->set_present_callback([this](auto) { QuitLoop(); });
  ASSERT_FALSE(RunLoopWithTimeout(kIndirectPresentTimeout));
}

void PixelTest::Present(scenic::Session* session, zx::time present_time) {
  session->Present(present_time, [this](auto) { QuitLoop(); });
  ASSERT_FALSE(RunLoopWithTimeout(kPresentTimeout));
}

DisplayDimensions PixelTest::GetDisplayDimensions() {
  DisplayDimensions display_dimensions;
  scenic_->GetDisplayInfo([this, &display_dimensions](fuchsia::ui::gfx::DisplayInfo display_info) {
    display_dimensions = {.width = static_cast<float>(display_info.width_in_px),
                          .height = static_cast<float>(display_info.height_in_px)};
    QuitLoop();
  });
  RunLoop();
  return display_dimensions;
}

std::unique_ptr<TestSession> PixelTest::SetUpTestSession() {
  auto test_session = std::make_unique<TestSession>(scenic(), GetDisplayDimensions());
  test_session->session.set_error_handler([](auto) { FAIL() << "Session terminated."; });
  return test_session;
}

}  // namespace gfx
