// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_SCREEN_READER_TESTS_MOCKS_MOCK_SEMANTIC_LISTENER_H_
#define SRC_UI_A11Y_LIB_SCREEN_READER_TESTS_MOCKS_MOCK_SEMANTIC_LISTENER_H_

#include <fuchsia/accessibility/semantics/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/macros.h>

#include "src/lib/fxl/macros.h"

namespace accessibility_test {
using fuchsia::accessibility::semantics::Action;
using fuchsia::accessibility::semantics::Hit;
using fuchsia::accessibility::semantics::Node;
using fuchsia::accessibility::semantics::SemanticListener;
using fuchsia::accessibility::semantics::SemanticsManagerPtr;
using fuchsia::accessibility::semantics::SemanticTreePtr;

class MockSemanticListener : public SemanticListener {
 public:
  // Mock for Semantic Action Listener, which will be
  // responsible for performing Hit test.
  //
  // On initialization, MockSemanticListener tries to connect to
  // |fuchsia::accessibility::SemanticsManager| service in |context_| and
  // registers with it's view_ref, binding and interface request.
  explicit MockSemanticListener(sys::ComponentContext* context,
                                fuchsia::ui::views::ViewRef view_ref);
  ~MockSemanticListener() override = default;

  void UpdateSemanticNodes(std::vector<Node> nodes);
  void DeleteSemanticNodes(std::vector<uint32_t> node_ids);
  void CommitUpdates();

  void SetHitTestingResult(Hit* hit) { hit->Clone(&hit_test_result_); }

  // |fuchsia::accessibility::semantics::SemanticListener|
  void OnAccessibilityActionRequested(
      uint32_t node_id, Action action,
      SemanticListener::OnAccessibilityActionRequestedCallback callback) override {}

  // |fuchsia::accessibility::semantics::SemanticListener|
  void HitTest(::fuchsia::math::PointF local_point, HitTestCallback callback) override;

  // |fuchsia::accessibility::semantics::SemanticListener|
  void OnSemanticsModeChanged(bool enabled, OnSemanticsModeChangedCallback callback) override {}

  sys::ComponentContext* context_;
  SemanticsManagerPtr manager_;
  SemanticTreePtr tree_ptr_;
  fidl::BindingSet<SemanticListener> bindings_;
  fuchsia::ui::views::ViewRef view_ref_;
  Hit hit_test_result_;

  FXL_DISALLOW_COPY_AND_ASSIGN(MockSemanticListener);
};

}  // namespace accessibility_test

#endif  // SRC_UI_A11Y_LIB_SCREEN_READER_TESTS_MOCKS_MOCK_SEMANTIC_LISTENER_H_
