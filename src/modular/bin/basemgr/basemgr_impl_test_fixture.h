// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_BIN_BASEMGR_BASEMGR_IMPL_TEST_FIXTURE_H_
#define SRC_MODULAR_BIN_BASEMGR_BASEMGR_IMPL_TEST_FIXTURE_H_

#include <fuchsia/modular/internal/cpp/fidl_test_base.h>
#include <lib/gtest/real_loop_fixture.h>
#include <lib/sys/cpp/testing/fake_component.h>
#include <lib/sys/cpp/testing/fake_launcher.h>

#include "src/lib/fsl/vmo/strings.h"
#include "src/modular/bin/basemgr/basemgr_impl.h"
#include "src/modular/lib/modular_config/modular_config.h"
#include "src/modular/lib/modular_config/modular_config_accessor.h"
#include "src/modular/lib/modular_config/modular_config_constants.h"
#include "src/modular/lib/pseudo_dir/pseudo_dir_server.h"

namespace modular {

class FakeComponentWithNamespace {
 public:
  using NamespaceMap = std::map<std::string, fidl::InterfaceHandle<fuchsia::io::Directory>>;

  FakeComponentWithNamespace() = default;

  // Adds specified interface to the set of public interfaces.
  //
  // Adds a supported service with the given |service_name|, using the given
  // |interface_request_handler|, which should remain valid for the lifetime of
  // this object.
  //
  // A typical usage may be:
  //
  //   AddPublicService(foobar_bindings_.GetHandler(this));
  template <typename Interface>
  zx_status_t AddPublicService(fidl::InterfaceRequestHandler<Interface> handler,
                               const std::string& service_name = Interface::Name_) {
    return directory_.AddEntry(service_name, std::make_unique<vfs::Service>(std::move(handler)));
  }

  // Registers this component with a FakeLauncher.
  void Register(std::string url, sys::testing::FakeLauncher& fake_launcher,
                async_dispatcher_t* dispatcher = nullptr);

  int launch_count() const { return launch_count_; }
  NamespaceMap& namespace_map() { return namespace_map_; }

 private:
  int launch_count_ = 0;
  vfs::PseudoDir directory_;
  std::vector<fidl::InterfaceRequest<fuchsia::sys::ComponentController>> ctrls_;
  NamespaceMap namespace_map_;
};

class FakeSessionmgr : public fuchsia::modular::internal::testing::Sessionmgr_TestBase {
 public:
  explicit FakeSessionmgr(sys::testing::FakeLauncher& launcher) {
    component_.AddPublicService(bindings_.GetHandler(this));
    component_.Register(modular_config::kSessionmgrUrl, launcher);
  }

  void NotImplemented_(const std::string& name) override {}

  void Initialize(
      std::string session_id,
      fidl::InterfaceHandle<::fuchsia::modular::internal::SessionContext> session_context,
      fuchsia::sys::ServiceList v2_services_for_sessionmgr,
      fidl::InterfaceRequest<::fuchsia::io::Directory> svc_from_v1_sessionmgr,
      fuchsia::ui::views::ViewToken view_token, fuchsia::ui::views::ViewRefControl control_ref,
      fuchsia::ui::views::ViewRef view_ref) override {
    v2_services_for_sessionmgr_ = std::move(v2_services_for_sessionmgr);
    initialized_ = true;
  }

  FakeComponentWithNamespace* component() { return &component_; }
  bool initialized() const { return initialized_; }
  std::optional<fuchsia::sys::ServiceList>& v2_services_for_sessionmgr() {
    return v2_services_for_sessionmgr_;
  }

 private:
  bool initialized_ = false;
  std::optional<fuchsia::sys::ServiceList> v2_services_for_sessionmgr_ = std::nullopt;
  fidl::BindingSet<fuchsia::modular::internal::Sessionmgr> bindings_;
  FakeComponentWithNamespace component_;
};

class BasemgrImplTestFixture : public gtest::RealLoopFixture {
 public:
  BasemgrImplTestFixture() : basemgr_inspector_(&inspector) {}

  void SetUp() override {}

  void CreateBasemgrImpl(fuchsia::modular::session::ModularConfig config) {
    basemgr_impl_ = std::make_unique<BasemgrImpl>(
        ModularConfigAccessor(std::move(config)), outgoing_directory_, &basemgr_inspector_,
        GetLauncher(), std::move(presenter_), std::move(device_administrator_),
        /*child_listener=*/nullptr, std::move(on_shutdown_));
  }

  fuchsia::modular::session::LauncherPtr GetSessionLauncher() {
    fuchsia::modular::session::LauncherPtr session_launcher;
    basemgr_impl_->GetLauncherHandler()(session_launcher.NewRequest());
    return session_launcher;
  }

 protected:
  fuchsia::sys::LauncherPtr GetLauncher() {
    fuchsia::sys::LauncherPtr launcher;
    fake_launcher_.GetHandler()(launcher.NewRequest());
    return launcher;
  }

  static fuchsia::mem::Buffer BufferFromString(std::string_view contents) {
    fuchsia::mem::Buffer config_buf;
    ZX_ASSERT(fsl::VmoFromString(contents, &config_buf));
    return config_buf;
  }

  bool did_shut_down_ = false;
  fit::function<void()> on_shutdown_ = [&]() { did_shut_down_ = true; };
  std::shared_ptr<sys::OutgoingDirectory> outgoing_directory_ =
      std::make_shared<sys::OutgoingDirectory>();
  inspect::Inspector inspector;
  BasemgrInspector basemgr_inspector_;
  fuchsia::ui::policy::PresenterPtr presenter_;
  fuchsia::hardware::power::statecontrol::AdminPtr device_administrator_;
  sys::testing::FakeLauncher fake_launcher_;
  std::unique_ptr<BasemgrImpl> basemgr_impl_;
};

}  // namespace modular

#endif  // SRC_MODULAR_BIN_BASEMGR_BASEMGR_IMPL_TEST_FIXTURE_H_
