// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_SEMANTICS_TESTS_SEMANTICS_INTEGRATION_TEST_FIXTURE_V2_H_
#define SRC_UI_A11Y_LIB_SEMANTICS_TESTS_SEMANTICS_INTEGRATION_TEST_FIXTURE_V2_H_

#include <fuchsia/accessibility/semantics/cpp/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/gtest/real_loop_fixture.h>
#include <lib/sys/component/cpp/testing/realm_builder.h>
#include <lib/sys/component/cpp/testing/realm_builder_types.h>
#include <lib/sys/cpp/component_context.h>

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "src/ui/a11y/lib/annotation/tests/mocks/mock_annotation_view.h"
#include "src/ui/a11y/lib/semantics/a11y_semantics_event_manager.h"
#include "src/ui/a11y/lib/view/tests/mocks/mock_accessibility_view.h"
#include "src/ui/a11y/lib/view/tests/mocks/mock_view_injector_factory.h"
#include "src/ui/a11y/lib/view/tests/mocks/mock_view_semantics.h"
#include "src/ui/a11y/lib/view/view_manager.h"

namespace accessibility_test {

// Types imported for the realm_builder library.
using fuchsia::accessibility::semantics::Node;

using component_testing::ChildRef;
using component_testing::LocalComponent;
using component_testing::LocalComponentHandles;
using component_testing::RealmRoot;

using RealmBuilder = component_testing::RealmBuilder;

// Mock component that will proxy SemanticsManager and SemanticTree requests to the ViewManager
// owned by the test fixture.
class SemanticsManagerProxy : public fuchsia::accessibility::semantics::SemanticsManager,
                              public LocalComponent {
 public:
  SemanticsManagerProxy(fuchsia::accessibility::semantics::SemanticsManager* semantics_manager,
                        async_dispatcher_t* dispatcher)
      : dispatcher_(dispatcher), semantics_manager_(semantics_manager) {}
  ~SemanticsManagerProxy() override = default;

  void Start(std::unique_ptr<LocalComponentHandles> mock_handles) override;

  // |fuchsia::accessibility::semantics::SemanticsManager|
  void RegisterViewForSemantics(
      fuchsia::ui::views::ViewRef view_ref,
      fidl::InterfaceHandle<fuchsia::accessibility::semantics::SemanticListener> handle,
      fidl::InterfaceRequest<fuchsia::accessibility::semantics::SemanticTree> semantic_tree_request)
      override;

 private:
  async_dispatcher_t* dispatcher_ = nullptr;
  std::vector<std::unique_ptr<LocalComponentHandles>> mock_handles_{};
  fidl::BindingSet<fuchsia::accessibility::semantics::SemanticsManager> bindings_;
  fuchsia::accessibility::semantics::SemanticsManager* semantics_manager_ = nullptr;
};

class SemanticsIntegrationTestV2 : public gtest::RealLoopFixture {
 public:
  static constexpr auto kSemanticsManager = "semantics_manager";
  static constexpr auto kSemanticsManagerRef = ChildRef{kSemanticsManager};
  static constexpr auto kRootPresenter = "root_presenter";
  static constexpr auto kRootPresenterRef = ChildRef{kRootPresenter};
  static constexpr auto kScenic = "scenic";
  static constexpr auto kScenicRef = ChildRef{kScenic};
  static constexpr auto kMockCobalt = "cobalt";
  static constexpr auto kMockCobaltRef = ChildRef{kMockCobalt};
  static constexpr auto kHdcp = "hdcp";
  static constexpr auto kHdcpRef = ChildRef{kHdcp};
  static constexpr auto kNetstack = "netstack";
  static constexpr auto kNetstackRef = ChildRef{kNetstack};

  SemanticsIntegrationTestV2()
      : context_(sys::ComponentContext::Create()),
        realm_builder_(std::make_unique<RealmBuilder>(RealmBuilder::Create())) {}

  ~SemanticsIntegrationTestV2() override = default;

  void SetUp() override;

  // Subclass should implement this method if it wishes to modify the realm
  // beyond the base setup that this class does.
  virtual void ConfigureRealm(RealmBuilder* realm_builder) {}

 protected:
  sys::ComponentContext* context() { return context_.get(); }
  RealmBuilder* builder() { return realm_builder_.get(); }
  RealmRoot* realm() { return realm_.get(); }
  a11y::ViewManager* view_manager() { return view_manager_.get(); }
  SemanticsManagerProxy* semantics_manager_proxy() { return semantics_manager_proxy_.get(); }
  zx_koid_t view_ref_koid() const { return view_ref_koid_; }

  // Launches the test client by connecting to fuchsia.ui.app.ViewProvider protocol.
  // This method should only be invoked if this protocol has been exposed from
  // the root of the test realm. After establishing a connection, this method
  // listens for the client is_rendering signal and calls |on_is_rendering| when it arrives.
  void LaunchClient(std::string debug_name);

  // Recursively traverses the node hierarchy, rooted at |node|, to find the first descendant
  // with |label|.
  const Node* FindNodeWithLabel(const Node* node, zx_koid_t view_ref_koid, std::string label);

  // Get the transform between the view's local space and the node's local space.
  a11y::SemanticTransform GetTransformForNode(zx_koid_t view_ref_koid, uint32_t node_id);

  // Calculates the point in the view's local space corresponding to the point at the center of the
  // semantic node's bounding box.
  fuchsia::math::PointF CalculateCenterOfSemanticNodeBoundingBoxCoordinate(
      zx_koid_t view_ref_koid, const fuchsia::accessibility::semantics::Node* node);

  // Perform a hit test against the target node and return the node ID of the node (if any) that is
  // hit.
  std::optional<uint32_t> HitTest(zx_koid_t view_ref_koid, fuchsia::math::PointF target);

  // Perform an accessibility action against the target node and return whether or not the action
  // was handled
  bool PerformAccessibilityAction(zx_koid_t view_ref_koid, uint32_t node_id,
                                  fuchsia::accessibility::semantics::Action action);

 private:
  void BuildRealm();

  std::unique_ptr<sys::ComponentContext> context_;
  std::unique_ptr<RealmBuilder> realm_builder_;
  std::unique_ptr<RealmRoot> realm_;
  std::unique_ptr<a11y::ViewManager> view_manager_;
  std::unique_ptr<SemanticsManagerProxy> semantics_manager_proxy_;
  fuchsia::ui::scenic::ScenicPtr scenic_;
  std::unique_ptr<scenic::Session> session_;

  // Test view and client view's ViewHolder.
  std::unique_ptr<scenic::ViewHolder> view_holder_;
  std::unique_ptr<scenic::View> view_;
  zx_koid_t view_ref_koid_;
};

}  // namespace accessibility_test

#endif  // SRC_UI_A11Y_LIB_SEMANTICS_TESTS_SEMANTICS_INTEGRATION_TEST_FIXTURE_V2_H_
