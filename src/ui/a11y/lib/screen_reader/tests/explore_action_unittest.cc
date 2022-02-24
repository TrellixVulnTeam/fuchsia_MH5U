// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/screen_reader/explore_action.h"

#include <fuchsia/accessibility/cpp/fidl.h>
#include <fuchsia/accessibility/semantics/cpp/fidl.h>

#include "src/ui/a11y/bin/a11y_manager/tests/util/util.h"
#include "src/ui/a11y/lib/screen_reader/tests/mocks/mock_screen_reader_context.h"
#include "src/ui/a11y/lib/screen_reader/tests/screen_reader_action_test_fixture.h"
#include "src/ui/a11y/lib/semantics/tests/mocks/mock_semantic_provider.h"
#include "src/ui/a11y/lib/semantics/tests/mocks/mock_semantics_source.h"
#include "src/ui/a11y/lib/util/util.h"

namespace accessibility_test {
namespace {

using fuchsia::accessibility::semantics::Attributes;
using fuchsia::accessibility::semantics::Node;
using fuchsia::accessibility::semantics::Role;

// Arbitrary number to set a local coordinate when sending for hit testing.
static const int kLocalCoordForTesting = 10;

class ExploreActionTest : public ScreenReaderActionTest {
 public:
  ExploreActionTest() = default;
  ~ExploreActionTest() override = default;

  void SetUp() override {
    ScreenReaderActionTest::SetUp();

    mock_a11y_focus_manager()->SetA11yFocus(100u, 10000, [](bool result) { EXPECT_TRUE(result); });
    mock_a11y_focus_manager()->ResetExpectations();

    // Creating test node to update.
    Node node = accessibility_test::CreateTestNode(0, "Label A", {1u});
    Node node_2 = accessibility_test::CreateTestNode(1u, "", {});
    mock_semantics_source()->CreateSemanticNode(mock_semantic_provider()->koid(), std::move(node));
    mock_semantics_source()->CreateSemanticNode(mock_semantic_provider()->koid(),
                                                std::move(node_2));
  }
};

TEST_F(ExploreActionTest, SuccessfulExploreActionReadsNode) {
  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = mock_semantic_provider()->koid();
  // Note that x and y are set just for completeness of the data type. the semantic provider is
  // responsible for returning what was the hit based on these numbers.
  gesture_context.current_pointer_locations[0].local_point.x = kLocalCoordForTesting;
  gesture_context.current_pointer_locations[0].local_point.y = kLocalCoordForTesting;

  fuchsia::accessibility::semantics::Hit hit;
  hit.set_node_id(0u);
  mock_semantics_source()->SetHitTestResult(mock_semantic_provider()->koid(), std::move(hit));
  EXPECT_FALSE(mock_a11y_focus_manager()->IsSetA11yFocusCalled());

  explore_action.Run(gesture_context);
  RunLoopUntilIdle();

  // Checks that a new a11y focus was set.
  EXPECT_TRUE(mock_a11y_focus_manager()->IsSetA11yFocusCalled());
  auto focus = mock_a11y_focus_manager()->GetA11yFocus();
  ASSERT_TRUE(focus);
  EXPECT_EQ(focus->node_id, 0u);
  EXPECT_EQ(focus->view_ref_koid, mock_semantic_provider()->koid());
  EXPECT_TRUE(mock_speaker()->ReceivedSpeak());
  ASSERT_EQ(mock_speaker()->node_ids().size(), 1u);
  EXPECT_EQ(mock_speaker()->node_ids()[0], 0u);
}

TEST_F(ExploreActionTest, HitTestFails) {
  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = mock_semantic_provider()->koid();
  // Note that x and y are set just for completeness of the data type. the semantic provider is
  // responsible for returning what was the hit based on these numbers.
  gesture_context.current_pointer_locations[0].local_point.x = 10;
  gesture_context.current_pointer_locations[0].local_point.y = 10;

  // In order for the mock semantics source to return a hit test result, we need
  // to set it explicitly before we run the action. By leaving it unset, we
  // ensure that the hit test will not return a result.
  explore_action.Run(gesture_context);
  RunLoopUntilIdle();

  EXPECT_FALSE(mock_a11y_focus_manager()->IsSetA11yFocusCalled());
  EXPECT_FALSE(mock_speaker()->ReceivedSpeak());
  ASSERT_TRUE(mock_speaker()->node_ids().empty());
}

TEST_F(ExploreActionTest, SetA11yFocusFails) {
  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = mock_semantic_provider()->koid();
  // Note that x and y are set just for completeness of the data type. the semantic provider is
  // responsible for returning what was the hit based on these numbers.
  gesture_context.current_pointer_locations[0].local_point.x = 10;
  gesture_context.current_pointer_locations[0].local_point.y = 10;

  fuchsia::accessibility::semantics::Hit hit;
  hit.set_node_id(0u);
  mock_semantics_source()->SetHitTestResult(mock_semantic_provider()->koid(), std::move(hit));
  mock_a11y_focus_manager()->set_should_set_a11y_focus_fail(true);

  explore_action.Run(gesture_context);
  RunLoopUntilIdle();

  EXPECT_TRUE(mock_a11y_focus_manager()->IsSetA11yFocusCalled());
  auto focus = mock_a11y_focus_manager()->GetA11yFocus();
  ASSERT_TRUE(focus);
  EXPECT_NE(focus->node_id, 0u);
  EXPECT_NE(focus->view_ref_koid, mock_semantic_provider()->koid());

  EXPECT_FALSE(mock_speaker()->ReceivedSpeak());
  ASSERT_TRUE(mock_speaker()->node_ids().empty());
}

TEST_F(ExploreActionTest, GettingA11yFocusFails) {
  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = mock_semantic_provider()->koid();
  // Note that x and y are set just for completeness of the data type. the semantic provider is
  // responsible for returning what was the hit based on these numbers.
  gesture_context.current_pointer_locations[0].local_point.x = 10;
  gesture_context.current_pointer_locations[0].local_point.y = 10;

  fuchsia::accessibility::semantics::Hit hit;
  hit.set_node_id(0u);
  mock_semantics_source()->SetHitTestResult(mock_semantic_provider()->koid(), std::move(hit));
  mock_a11y_focus_manager()->set_should_get_a11y_focus_fail(true);

  explore_action.Run(gesture_context);
  RunLoopUntilIdle();

  EXPECT_TRUE(mock_a11y_focus_manager()->IsSetA11yFocusCalled());
  // We need to inspect the focus that was set by us, so flip the mock behavior.
  mock_a11y_focus_manager()->set_should_get_a11y_focus_fail(false);

  auto focus = mock_a11y_focus_manager()->GetA11yFocus();
  ASSERT_TRUE(focus);
  EXPECT_EQ(focus->node_id, 0u);
  EXPECT_EQ(focus->view_ref_koid, mock_semantic_provider()->koid());

  EXPECT_FALSE(mock_speaker()->ReceivedSpeak());
  ASSERT_TRUE(mock_speaker()->node_ids().empty());
}

TEST_F(ExploreActionTest, HitTestNodeIDResultIsNotPresentInTheTree) {
  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = mock_semantic_provider()->koid();
  // Note that x and y are set just for completeness of the data type. the semantic provider is
  // responsible for returning what was the hit based on these numbers.
  gesture_context.current_pointer_locations[0].local_point.x = 10;
  gesture_context.current_pointer_locations[0].local_point.y = 10;

  fuchsia::accessibility::semantics::Hit hit;
  hit.set_node_id(100u);
  mock_semantics_source()->SetHitTestResult(mock_semantic_provider()->koid(), std::move(hit));

  explore_action.Run(gesture_context);
  RunLoopUntilIdle();

  EXPECT_FALSE(mock_a11y_focus_manager()->IsSetA11yFocusCalled());
  EXPECT_FALSE(mock_speaker()->ReceivedSpeak());
}

TEST_F(ExploreActionTest, HitTestNodeNotDescribable) {
  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = mock_semantic_provider()->koid();
  // Note that x and y are set just for completeness of the data type. the semantic provider is
  // responsible for returning what was the hit based on these numbers.
  gesture_context.current_pointer_locations[0].local_point.x = 10;
  gesture_context.current_pointer_locations[0].local_point.y = 10;

  // Set hit test result.
  fuchsia::accessibility::semantics::Hit hit;
  hit.set_node_id(1u);
  mock_semantics_source()->SetHitTestResult(mock_semantic_provider()->koid(), std::move(hit));

  explore_action.Run(gesture_context);
  RunLoopUntilIdle();

  EXPECT_TRUE(mock_a11y_focus_manager()->IsSetA11yFocusCalled());
  EXPECT_TRUE(mock_speaker()->ReceivedSpeak());
  ASSERT_FALSE(mock_speaker()->node_ids().empty());
  EXPECT_EQ(mock_speaker()->node_ids()[0], 0u);
}

TEST_F(ExploreActionTest, ContinuousExploreSpeaksNodeWhenA11yFocusIsDifferent) {
  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = mock_semantic_provider()->koid();
  // Note that x and y are set just for completeness of the data type. the semantic provider is
  // responsible for returning what was the hit based on these numbers.
  gesture_context.current_pointer_locations[0].local_point.x = kLocalCoordForTesting;
  gesture_context.current_pointer_locations[0].local_point.y = kLocalCoordForTesting;

  fuchsia::accessibility::semantics::Hit hit;
  hit.set_node_id(0u);
  mock_semantics_source()->SetHitTestResult(mock_semantic_provider()->koid(), std::move(hit));

  mock_screen_reader_context()->set_mode(
      a11y::ScreenReaderContext::ScreenReaderMode::kContinuousExploration);
  explore_action.Run(gesture_context);
  RunLoopUntilIdle();

  // Checks that a new a11y focus was set.
  EXPECT_TRUE(mock_a11y_focus_manager()->IsSetA11yFocusCalled());

  auto focus = mock_a11y_focus_manager()->GetA11yFocus();
  ASSERT_TRUE(focus);
  EXPECT_EQ(focus->node_id, 0u);
  EXPECT_EQ(focus->view_ref_koid, mock_semantic_provider()->koid());

  EXPECT_TRUE(mock_speaker()->ReceivedSpeak());
}

TEST_F(ExploreActionTest, ContinuousExploreDropsWhenA11yFocusIsTheSame) {
  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = mock_semantic_provider()->koid();
  // Note that x and y are set just for completeness of the data type. the semantic provider is
  // responsible for returning what was the hit based on these numbers.
  gesture_context.current_pointer_locations[0].local_point.x = kLocalCoordForTesting;
  gesture_context.current_pointer_locations[0].local_point.y = kLocalCoordForTesting;

  fuchsia::accessibility::semantics::Hit hit;
  hit.set_node_id(0u);
  mock_semantics_source()->SetHitTestResult(mock_semantic_provider()->koid(), std::move(hit));

  mock_screen_reader_context()->set_mode(
      a11y::ScreenReaderContext::ScreenReaderMode::kContinuousExploration);
  mock_a11y_focus_manager()->SetA11yFocus(mock_semantic_provider()->koid(), 0, [](auto...) {});
  explore_action.Run(gesture_context);
  RunLoopUntilIdle();
  EXPECT_FALSE(mock_speaker()->ReceivedSpeak());
  ASSERT_TRUE(mock_speaker()->node_ids().empty());
}

TEST_F(ExploreActionTest, ReadsKeyboardKey) {
  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = mock_semantic_provider()->koid();
  gesture_context.current_pointer_locations[0].local_point.x = kLocalCoordForTesting;
  gesture_context.current_pointer_locations[0].local_point.y = kLocalCoordForTesting;

  fuchsia::accessibility::semantics::Hit hit;
  hit.set_node_id(0u);
  mock_semantics_source()->SetHitTestResult(mock_semantic_provider()->koid(), std::move(hit));

  mock_screen_reader_context()->set_virtual_keyboard_focused(true);

  explore_action.Run(gesture_context);
  RunLoopUntilIdle();

  EXPECT_TRUE(mock_speaker()->ReceivedSpeakLabel());
  ASSERT_EQ(mock_speaker()->node_ids().size(), 1u);
  EXPECT_EQ(mock_speaker()->node_ids()[0], 0u);
}

TEST_F(ExploreActionTest, HitTestsAreSentToKeyboardViewWhenKeyboardIsVisible) {
  const auto second_view_koid =
      mock_semantic_provider()->koid() + 1;  // to be different from first view koid.
  // Simulate a view having a visible keyboard.
  mock_semantics_source()->set_view_with_visible_keyboard(second_view_koid);

  Node node = accessibility_test::CreateTestNode(0, "Keyboard view root node", {});
  mock_semantics_source()->CreateSemanticNode(second_view_koid, std::move(node));

  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = mock_semantic_provider()->koid();

  // Note that the hit test result returns the keyboard view. If, for some reason, the code ever
  // tries to hit test the other view the return of this hit test for the other view would be empty
  // and this test would fail.
  fuchsia::accessibility::semantics::Hit hit;
  hit.set_node_id(0u);
  mock_semantics_source()->SetHitTestResult(second_view_koid, std::move(hit));

  explore_action.Run(gesture_context);
  RunLoopUntilIdle();

  // Checks that a new a11y focus was set, focusing the view with the keyboard.
  EXPECT_TRUE(mock_a11y_focus_manager()->IsSetA11yFocusCalled());
  auto focus = mock_a11y_focus_manager()->GetA11yFocus();
  ASSERT_TRUE(focus);
  EXPECT_EQ(focus->node_id, 0u);
  EXPECT_EQ(focus->view_ref_koid, second_view_koid);
}

TEST_F(ExploreActionTest, UpdatesNavigationContext) {
  Node root_node = CreateTestNode(0u, "root");
  root_node.set_child_ids({1u});

  Node child_node = CreateTestNode(1u, "child");
  child_node.set_child_ids({2u});

  Node table_node = CreateTestNode(2u, "table");
  table_node.set_role(fuchsia::accessibility::semantics::Role::TABLE);
  auto* table_attributes = table_node.mutable_attributes()->mutable_table_attributes();
  table_attributes->set_number_of_rows(3u);
  table_attributes->set_number_of_columns(4u);
  table_node.set_child_ids({3u});
  EXPECT_TRUE(table_node.has_attributes());
  EXPECT_TRUE(table_node.attributes().has_table_attributes());

  Node cell_node = CreateTestNode(3u, "cell");
  cell_node.set_role(fuchsia::accessibility::semantics::Role::CELL);
  auto* cell_attributes = table_node.mutable_attributes()->mutable_table_cell_attributes();
  cell_attributes->set_row_index(1u);
  cell_attributes->set_column_index(2u);

  mock_semantics_source()->CreateSemanticNode(mock_semantic_provider()->koid(),
                                              std::move(root_node));
  mock_semantics_source()->CreateSemanticNode(mock_semantic_provider()->koid(),
                                              std::move(child_node));
  mock_semantics_source()->CreateSemanticNode(mock_semantic_provider()->koid(),
                                              std::move(table_node));
  mock_semantics_source()->CreateSemanticNode(mock_semantic_provider()->koid(),
                                              std::move(cell_node));

  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = mock_semantic_provider()->koid();

  fuchsia::accessibility::semantics::Hit hit;
  hit.set_node_id(3u);
  mock_semantics_source()->SetHitTestResult(mock_semantic_provider()->koid(), std::move(hit));

  explore_action.Run(gesture_context);
  RunLoopUntilIdle();

  const auto& navigation_context = mock_screen_reader_context()->current_navigation_context();
  EXPECT_TRUE(navigation_context.current_container.has_value());
  EXPECT_EQ(*navigation_context.current_container, 2u);
}

TEST_F(ExploreActionTest, UserExitsTableInSeparateView) {
  // Create table in view 1.
  {
    Node root_node = CreateTestNode(0u, "root 1");
    root_node.set_child_ids({1u});

    Node child_node = CreateTestNode(1u, "child");
    child_node.set_child_ids({2u});

    Node table_node = CreateTestNode(2u, "table");
    table_node.set_role(fuchsia::accessibility::semantics::Role::TABLE);
    auto* table_attributes = table_node.mutable_attributes()->mutable_table_attributes();
    table_attributes->set_number_of_rows(3u);
    table_attributes->set_number_of_columns(4u);
    table_node.set_child_ids({3u});
    EXPECT_TRUE(table_node.has_attributes());
    EXPECT_TRUE(table_node.attributes().has_table_attributes());

    Node cell_node = CreateTestNode(3u, "cell");
    cell_node.set_role(fuchsia::accessibility::semantics::Role::CELL);
    auto* cell_attributes = table_node.mutable_attributes()->mutable_table_cell_attributes();
    cell_attributes->set_row_index(1u);
    cell_attributes->set_column_index(2u);

    mock_semantics_source()->CreateSemanticNode(mock_semantic_provider()->koid(),
                                                std::move(root_node));
    mock_semantics_source()->CreateSemanticNode(mock_semantic_provider()->koid(),
                                                std::move(child_node));
    mock_semantics_source()->CreateSemanticNode(mock_semantic_provider()->koid(),
                                                std::move(table_node));
    mock_semantics_source()->CreateSemanticNode(mock_semantic_provider()->koid(),
                                                std::move(cell_node));
  }

  // Create a root node in a separate view.
  MockSemanticProvider semantic_provider_2(nullptr, nullptr);
  {
    Node root_node = CreateTestNode(0u, "root 2");
    mock_semantics_source()->CreateSemanticNode(semantic_provider_2.koid(), std::move(root_node));
  }

  // Set the current navigation context to have node 2 as its container.
  a11y::ScreenReaderContext::NavigationContext navigation_context;
  navigation_context.current_container = 2u;
  navigation_context.view_ref_koid = mock_semantic_provider()->koid();

  // Set the hit result to the root of view 2.
  fuchsia::accessibility::semantics::Hit hit;
  hit.set_node_id(0u);
  mock_semantics_source()->SetHitTestResult(semantic_provider_2.koid(), std::move(hit));

  // Run the action using a gesture context in view 2.
  a11y::ExploreAction explore_action(action_context(), mock_screen_reader_context());
  a11y::GestureContext gesture_context;
  gesture_context.view_ref_koid = semantic_provider_2.koid();

  explore_action.Run(gesture_context);
  RunLoopUntilIdle();

  // Verify that previous_container was NOT set in the message context.
  ASSERT_TRUE(mock_a11y_focus_manager()->IsSetA11yFocusCalled());
  EXPECT_EQ(mock_a11y_focus_manager()->GetA11yFocus().value().node_id, 0u);
  EXPECT_EQ(mock_a11y_focus_manager()->GetA11yFocus().value().view_ref_koid,
            semantic_provider_2.koid());
  EXPECT_TRUE(mock_speaker()->ReceivedSpeak());
  EXPECT_EQ(mock_speaker()->node_ids().size(), 1u);
  EXPECT_EQ(mock_speaker()->node_ids()[0], 0u);
  ASSERT_FALSE(mock_speaker()->message_contexts()[0].previous_container);
}

}  // namespace
}  // namespace accessibility_test
