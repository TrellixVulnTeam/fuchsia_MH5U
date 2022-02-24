// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>

#include <memory>

#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"
#include "src/lib/ui/base_view/view_provider_component.h"
#include "src/ui/examples/simplest_embedder/example_presenter.h"
#include "src/ui/examples/simplest_embedder/view.h"
#include "src/zircon/lib/zircon/include/zircon/status.h"

using namespace simplest_embedder;

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line))
    return 1;

  const bool kUseRootPresenter = command_line.HasOption("use_root_presenter");
  const bool kUseExamplePresenter = command_line.HasOption("use_example_presenter");
  if (kUseRootPresenter && kUseExamplePresenter) {
    FX_LOGS(ERROR) << "Cannot set both --use_root_presenter and --use_example_presenter";
    return 1;
  }

  // If the user asked us to use a Presenter, then do so.  Otherwise, just
  // provide our view as a service.
  if (kUseRootPresenter) {
    FX_LOGS(INFO) << "Using root presenter.";
    FX_LOGS(INFO) << "To quit: Tap the background and hit the ESC key.";

    // We need to attach ourselves to a Presenter. To do this, we create a
    // pair of tokens, and use one to create a View locally (which we attach
    // the rest of our UI to), and one which we pass to a Presenter to create
    // a ViewHolder to embed us.
    //
    // In the Peridot layer of Fuchsia, the device_runner both launches the
    // device shell, and connects it to the root presenter.  Here, we create
    // two eventpair handles, one of which will be passed to the root presenter
    // and the other to the View.
    auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();

    // Create a startup context for ourselves and use it to connect to
    // environment services.
    auto component_context = sys::ComponentContext::CreateAndServeOutgoingDirectory();
    fuchsia::ui::scenic::ScenicPtr scenic =
        component_context->svc()->Connect<fuchsia::ui::scenic::Scenic>();
    scenic.set_error_handler([&loop](zx_status_t status) {
      FX_LOGS(ERROR) << "Lost connection to Scenic with error " << zx_status_get_string(status)
                     << ".";
      loop.Quit();
    });

    // Create a View which will launch shadertoy and attach shadertoy's View to
    // itself.
    scenic::ViewContext view_context = {
        .session_and_listener_request =
            scenic::CreateScenicSessionPtrAndListenerRequest(scenic.get()),
        .view_token = std::move(view_token),
        .component_context = component_context.get(),
    };
    auto view = std::make_unique<ShadertoyEmbedderView>(std::move(view_context), &loop);
    view->LaunchShadertoyClient();

    // Display the newly-created view using root_presenter.
    fidl::InterfacePtr<fuchsia::ui::policy::Presentation> presentation;
    fuchsia::ui::policy::PresenterPtr root_presenter =
        component_context->svc()->Connect<fuchsia::ui::policy::Presenter>();
    root_presenter->PresentView(std::move(view_holder_token), presentation.NewRequest());

    loop.Run();
  } else if (kUseExamplePresenter) {
    FX_LOGS(INFO) << "Using example presenter.";

    // We need to attach ourselves to a Presenter. To do this, we create a
    // pair of tokens, and use one to create a View locally (which we attach
    // the rest of our UI to), and one which we pass to a Presenter to create
    // a ViewHolder to embed us.
    //
    // In the Peridot layer of Fuchsia, the device_runner both launches the
    // device shell, and connects it to the root presenter.  Here, we create
    // two eventpair handles, one of which will be passed to our example
    // Presenter and the other to the View.
    //
    // For simplicity, both the presenter and the view run in-process, and the
    // tokens are passed to them via C++ methods.  However, it would work just
    // as well if the presenter/view lived in two other processes, and we
    // passed the tokens to them via FIDL messages.  In Peridot, this is
    // exactly what the device_runner does.
    auto [view_token, view_holder_token] = scenic::ViewTokenPair::New();

    // Create a startup context for ourselves and use it to connect to
    // environment services.
    auto component_context = sys::ComponentContext::CreateAndServeOutgoingDirectory();
    fuchsia::ui::scenic::ScenicPtr scenic =
        component_context->svc()->Connect<fuchsia::ui::scenic::Scenic>();
    scenic.set_error_handler([&loop](zx_status_t status) {
      FX_LOGS(INFO) << "Lost connection to Scenic with error code " << status << ".";
      loop.Quit();
    });

    // Create a View which will launch shadertoy and attach shadertoy's View to
    // itself.
    scenic::ViewContext view_context = {
        .session_and_listener_request =
            scenic::CreateScenicSessionPtrAndListenerRequest(scenic.get()),
        .view_token = std::move(view_token),
        .component_context = component_context.get(),
    };
    auto view = std::make_unique<ShadertoyEmbedderView>(std::move(view_context), &loop);
    view->LaunchShadertoyClient();

    // Display the newly created View using our in-process presenter, which
    // creates a DisplayCompositor directly for screen output.
    // NOTE: The example presenter has an independent session to Scenic even
    // though it resides in the same process as the view.
    auto example_presenter = std::make_unique<ExamplePresenter>(scenic.get());
    example_presenter->PresentView(std::move(view_holder_token), nullptr);

    loop.Run();
  } else {
    // Instead of creating a View directly, provide a component that will do so
    // when asked via FIDL.
    FX_LOGS(INFO) << "Launching view provider service.";
    scenic::ViewProviderComponent component(
        [&loop](scenic::ViewContext context) {
          // Create a View which will launch shadertoy and attach shadertoy's
          // View to itself.
          auto view = std::make_unique<ShadertoyEmbedderView>(std::move(context), &loop);
          view->LaunchShadertoyClient();

          return view;
        },
        &loop);

    loop.Run();
  }

  return 0;
}
