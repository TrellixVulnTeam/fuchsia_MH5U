// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_BIN_A11Y_MANAGER_TESTS_MOCKS_MOCK_FOCUS_CHAIN_H_
#define SRC_UI_A11Y_BIN_A11Y_MANAGER_TESTS_MOCKS_MOCK_FOCUS_CHAIN_H_

#include <fuchsia/ui/focus/cpp/fidl.h>
#include <fuchsia/ui/views/accessibility/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/testing/component_context_provider.h>

#include "fuchsia/ui/views/cpp/fidl.h"
#include "src/lib/fxl/macros.h"

namespace accessibility_test {

// A mock that implements all services related to the Focus Chain used by A11y Manager.
class MockFocusChain : public fuchsia::ui::focus::FocusChainListenerRegistry,
                       public fuchsia::ui::views::Focuser {
 public:
  explicit MockFocusChain(sys::testing::ComponentContextProvider* context);
  ~MockFocusChain() = default;

  // Returns the registered listener.
  fuchsia::ui::focus::FocusChainListenerPtr& listener() { return listener_; }

  // Returns true if a focuser was registered.
  bool HasRegisteredFocuser() const { return focuser_binding_.is_bound(); }

  // Returns true if RequestFocus() was called.
  bool IsRequestFocusCalled() const { return request_focus_called_; }

  // Returns Koid of ViewRef in focus. Returns ZX_KOID_INVALID if no view is in focus.
  zx_koid_t GetFocusedViewKoid();

 private:
  // |fuchsia.ui.focus.FocusChainListenerRegistry|
  void Register(fidl::InterfaceHandle<fuchsia::ui::focus::FocusChainListener> listener) override;

  // |fuchsia.ui.views.Focuser|
  void RequestFocus(fuchsia::ui::views::ViewRef view_ref, RequestFocusCallback callback) override;

  fidl::BindingSet<fuchsia::ui::focus::FocusChainListenerRegistry> listener_registry_bindings_;

  fidl::Binding<fuchsia::ui::views::Focuser> focuser_binding_;
  fuchsia::ui::focus::FocusChainListenerPtr listener_;

  bool request_focus_called_ = false;
  fuchsia::ui::views::ViewRef focused_view_ref_;

  FXL_DISALLOW_COPY_AND_ASSIGN(MockFocusChain);
};

}  // namespace accessibility_test

#endif  // SRC_UI_A11Y_BIN_A11Y_MANAGER_TESTS_MOCKS_MOCK_FOCUS_CHAIN_H_
