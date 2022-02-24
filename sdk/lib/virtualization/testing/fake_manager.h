// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_VIRTUALIZATION_TESTING_FAKE_MANAGER_H_
#define LIB_VIRTUALIZATION_TESTING_FAKE_MANAGER_H_

#include <fuchsia/virtualization/cpp/fidl.h>
#include <fuchsia/virtualization/cpp/fidl_test_base.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/virtualization/testing/fake_guest_vsock.h>
#include <lib/virtualization/testing/fake_host_vsock.h>

namespace guest {
namespace testing {

// Provides an implementation of |fuchsia::virtualization::Manager| that
// can create a single Environment/Guest. This is intended to make testing the
// common case of a single component creating a single guest.
class FakeManager : public fuchsia::virtualization::testing::Manager_TestBase,
                    public fuchsia::virtualization::testing::Realm_TestBase,
                    public fuchsia::virtualization::testing::Guest_TestBase {
 public:
  FakeGuestVsock* GuestVsock() { return &guest_vsock_; }

  fidl::InterfaceRequestHandler<fuchsia::virtualization::Manager> GetHandler() {
    return manager_bindings_.GetHandler(this);
  }

 private:
  // |fuchsia::virtualization::Manager|
  void Create(fidl::StringPtr label,
              fidl::InterfaceRequest<fuchsia::virtualization::Realm> env) override;

  // |fuchsia::virtualization::Realm|
  void LaunchInstance(std::string url, fidl::StringPtr label,
                      fuchsia::virtualization::GuestConfig guest_config,
                      fidl::InterfaceRequest<fuchsia::virtualization::Guest> controller,
                      LaunchInstanceCallback callback) override;
  void GetHostVsockEndpoint(
      fidl::InterfaceRequest<fuchsia::virtualization::HostVsockEndpoint> endpoint) override;

  void NotImplemented_(const std::string& name) override;

  FakeHostVsock host_vsock_{&guest_vsock_};
  FakeGuestVsock guest_vsock_{&host_vsock_};
  fidl::BindingSet<fuchsia::virtualization::Manager> manager_bindings_;
  fidl::Binding<fuchsia::virtualization::Realm> realm_binding_{this};
  fidl::Binding<fuchsia::virtualization::Guest> guest_binding_{this};
};

}  // namespace testing
}  // namespace guest

#endif  // LIB_VIRTUALIZATION_TESTING_FAKE_MANAGER_H_
