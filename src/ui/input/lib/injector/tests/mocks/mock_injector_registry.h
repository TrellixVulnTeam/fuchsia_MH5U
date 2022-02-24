// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_INPUT_LIB_INJECTOR_TESTS_MOCKS_MOCK_INJECTOR_REGISTRY_H_
#define SRC_UI_INPUT_LIB_INJECTOR_TESTS_MOCKS_MOCK_INJECTOR_REGISTRY_H_

#include <fuchsia/ui/pointerinjector/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/testing/component_context_provider.h>

#include <unordered_map>

namespace input::test {

class MockInjectorRegistry : public fuchsia::ui::pointerinjector::Registry,
                             public fuchsia::ui::pointerinjector::Device {
 public:
  explicit MockInjectorRegistry(sys::testing::ComponentContextProvider& context_provider);

  // |fuchsia.ui.pointerinjector.Registry|
  void Register(fuchsia::ui::pointerinjector::Config config,
                fidl::InterfaceRequest<fuchsia::ui::pointerinjector::Device> injector,
                RegisterCallback callback) override;

  // |fuchsia::ui::pointerinjector::Device|
  void Inject(std::vector<fuchsia::ui::pointerinjector::Event> events,
              InjectCallback callback) override;

  void FirePendingCallbacks();

  void KillAllBindings();

  uint32_t num_register_calls() { return num_register_calls_; }
  size_t num_registered() { return bindings_.size(); }
  uint32_t num_events_received() const { return num_events_received_; }

 private:
  uint32_t next_id_ = 0;
  uint32_t num_events_received_ = 0;
  uint32_t num_register_calls_ = 0;

  fidl::BindingSet<fuchsia::ui::pointerinjector::Registry> registry_;

  std::vector<InjectCallback> pending_callbacks_;
  std::unordered_map<uint32_t, fidl::Binding<fuchsia::ui::pointerinjector::Device>> bindings_;
};

}  // namespace input::test

#endif  // SRC_UI_INPUT_LIB_INJECTOR_TESTS_MOCKS_MOCK_INJECTOR_REGISTRY_H_
