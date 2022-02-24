// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "echo_client_app.h"

namespace echo {

EchoClientApp::EchoClientApp()
    : EchoClientApp(sys::ComponentContext::CreateAndServeOutgoingDirectory()) {}

EchoClientApp::EchoClientApp(std::unique_ptr<sys::ComponentContext> context)
    : context_(std::move(context)) {}

void EchoClientApp::Start(std::string server_url) {
  fidl::InterfaceHandle<fuchsia::io::Directory> directory;
  fuchsia::sys::LaunchInfo launch_info;
  launch_info.url = server_url;
  launch_info.directory_request = directory.NewRequest().TakeChannel();
  fuchsia::sys::LauncherPtr launcher;
  context_->svc()->Connect(launcher.NewRequest());
  launcher->CreateComponent(std::move(launch_info), controller_.NewRequest());

  sys::ServiceDirectory echo_provider(std::move(directory));
  echo_provider.Connect(echo_.NewRequest());
}

}  // namespace echo
