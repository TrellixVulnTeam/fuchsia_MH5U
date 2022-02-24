// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_BIN_ROOT_PRESENTER_TESTS_FAKES_FAKE_SCENIC_H_
#define SRC_UI_BIN_ROOT_PRESENTER_TESTS_FAKES_FAKE_SCENIC_H_

#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <fuchsia/ui/scenic/cpp/fidl_test_base.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/syslog/cpp/macros.h>

#include "src/ui/bin/root_presenter/tests/fakes/fake_focuser.h"
#include "src/ui/bin/root_presenter/tests/fakes/fake_session.h"

namespace root_presenter {
namespace testing {

class FakeScenic : public fuchsia::ui::scenic::testing::Scenic_TestBase {
 public:
  FakeScenic();
  ~FakeScenic() override;

  std::vector<std::unique_ptr<FakeSession>>& fakeSessions() { return fake_sessions_; }

  void NotImplemented_(const std::string& name) final {}

  fidl::InterfaceRequestHandler<fuchsia::ui::scenic::Scenic> GetHandler(
      async_dispatcher_t* dispatcher = nullptr) {
    return [this, dispatcher](fidl::InterfaceRequest<fuchsia::ui::scenic::Scenic> request) {
      bindings_.AddBinding(this, std::move(request), dispatcher);
    };
  }

  // Scenic implementation.
  void CreateSession(fidl::InterfaceRequest<fuchsia::ui::scenic::Session> session,
                     fidl::InterfaceHandle<fuchsia::ui::scenic::SessionListener> listener) override;
  void CreateSession2(fidl::InterfaceRequest<fuchsia::ui::scenic::Session> session,
                      fidl::InterfaceHandle<fuchsia::ui::scenic::SessionListener> listener,
                      fidl::InterfaceRequest<fuchsia::ui::views::Focuser> view_focuser) override;

  void GetDisplayOwnershipEvent(
      fuchsia::ui::scenic::Scenic::GetDisplayOwnershipEventCallback callback) override;

 private:
  fidl::BindingSet<fuchsia::ui::scenic::Scenic> bindings_;
  std::vector<std::unique_ptr<FakeFocuser>> fake_focusers_;
  std::vector<std::unique_ptr<FakeSession>> fake_sessions_;
};

}  // namespace testing
}  // namespace root_presenter

#endif  // SRC_UI_BIN_ROOT_PRESENTER_TESTS_FAKES_FAKE_SCENIC_H_
