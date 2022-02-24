// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/focus_chain/focus_chain_manager.h"

#include <lib/fidl/cpp/binding_set.h>
#include <lib/gtest/test_loop_fixture.h>

#include <memory>
#include <vector>

#include "src/ui/a11y/lib/focus_chain/accessibility_focus_chain_listener.h"
#include "src/ui/a11y/lib/screen_reader/focus/tests/mocks/mock_focuser.h"
#include "src/ui/a11y/lib/semantics/tests/mocks/mock_semantics_source.h"
#include "src/ui/a11y/lib/testing/view_ref_helper.h"
#include "src/ui/a11y/lib/util/util.h"
#include "src/ui/a11y/lib/view/tests/mocks/mock_accessibility_view.h"

namespace a11y {
namespace {

class MockAccessibilityFocusChainListener : public AccessibilityFocusChainListener {
 public:
  MockAccessibilityFocusChainListener() : weak_ptr_factory_(this) {}
  ~MockAccessibilityFocusChainListener() override { weak_ptr_factory_.InvalidateWeakPtrs(); }

  // Adds this listener to a AccessibilityFocusChainRegistry.
  void Add(AccessibilityFocusChainRegistry* registry) {
    registry->Register(weak_ptr_factory_.GetWeakPtr());
  }

  // Removes this listener from all registries by invalidating its fxl::WeakPtr.
  void Remove() { weak_ptr_factory_.InvalidateWeakPtrs(); }

  // |AccessibilityFocusChainListener|
  void OnViewFocus(zx_koid_t view_ref_koid) override { view_ref_koid_ = view_ref_koid; }
  zx_koid_t view_ref_koid() const { return view_ref_koid_; }

 private:
  zx_koid_t view_ref_koid_ = 1u;  // Important! set different than ZX_KOID_INVALID.
  fxl::WeakPtrFactory<MockAccessibilityFocusChainListener> weak_ptr_factory_;
};

class FocusChainManagerTest : public gtest::TestLoopFixture {
 public:
  FocusChainManagerTest() : listener_(std::make_unique<MockAccessibilityFocusChainListener>()) {}

 protected:
  void SetUp() override {
    manager_ = std::make_unique<FocusChainManager>(&mock_a11y_view_, &mock_semantics_source_);
  }

  // Test subject.
  std::unique_ptr<FocusChainManager> manager_;

  accessibility_test::MockAccessibilityView mock_a11y_view_;
  accessibility_test::MockSemanticsSource mock_semantics_source_;

  accessibility_test::ViewRefHelper root_view_;
  accessibility_test::ViewRefHelper view_a_;
  accessibility_test::ViewRefHelper view_b_;

  std::unique_ptr<MockAccessibilityFocusChainListener> listener_;
};

TEST_F(FocusChainManagerTest, SendsFocusChain) {
  listener_->Add(manager_.get());
  // Upon registration, check if listener received focus.
  // At this point, focus is set, so the expected value is ZX_KOID_INVALID.
  EXPECT_EQ(listener_->view_ref_koid(), ZX_KOID_INVALID);
  fuchsia::ui::focus::FocusChain focus_chain;
  focus_chain.mutable_focus_chain()->push_back(root_view_.Clone());
  focus_chain.mutable_focus_chain()->push_back(view_a_.Clone());
  manager_->OnFocusChange(std::move(focus_chain), []() {});
  EXPECT_EQ(listener_->view_ref_koid(), view_a_.koid());
}

TEST_F(FocusChainManagerTest, UpdatesFocusChain) {
  fuchsia::ui::focus::FocusChain focus_chain;
  focus_chain.mutable_focus_chain()->push_back(root_view_.Clone());
  focus_chain.mutable_focus_chain()->push_back(view_a_.Clone());
  manager_->OnFocusChange(std::move(focus_chain), []() {});
  listener_->Add(manager_.get());
  // Upon registration, check if listener received focus.
  // The manager has already a view in focus.
  EXPECT_EQ(listener_->view_ref_koid(), view_a_.koid());
  // Sends a second Focus Chain, now pointing to |view_b_|.
  fuchsia::ui::focus::FocusChain focus_chain_update;
  focus_chain_update.mutable_focus_chain()->push_back(root_view_.Clone());
  focus_chain_update.mutable_focus_chain()->push_back(view_b_.Clone());
  manager_->OnFocusChange(std::move(focus_chain_update), []() {});
  EXPECT_EQ(listener_->view_ref_koid(), view_b_.koid());
}

TEST_F(FocusChainManagerTest, InvalidatesFocusChain) {
  listener_->Add(manager_.get());
  fuchsia::ui::focus::FocusChain focus_chain;
  focus_chain.mutable_focus_chain()->push_back(root_view_.Clone());
  focus_chain.mutable_focus_chain()->push_back(view_a_.Clone());
  manager_->OnFocusChange(std::move(focus_chain), []() {});
  EXPECT_EQ(listener_->view_ref_koid(), view_a_.koid());
  // Invalidates the Focus Chain, and checking that listeners received the update.
  root_view_.SendEventPairSignal();
  RunLoopUntilIdle();
  EXPECT_EQ(listener_->view_ref_koid(), ZX_KOID_INVALID);
}

TEST_F(FocusChainManagerTest, RemovesListenerFromListening) {
  listener_->Add(manager_.get());
  fuchsia::ui::focus::FocusChain focus_chain;
  focus_chain.mutable_focus_chain()->push_back(root_view_.Clone());
  focus_chain.mutable_focus_chain()->push_back(view_a_.Clone());
  manager_->OnFocusChange(std::move(focus_chain), []() {});
  EXPECT_EQ(listener_->view_ref_koid(), view_a_.koid());
  // Removes the listener from listening. The next Focus Chain should not go to it then.
  listener_->Remove();
  fuchsia::ui::focus::FocusChain focus_chain_update;
  focus_chain_update.mutable_focus_chain()->push_back(root_view_.Clone());
  focus_chain_update.mutable_focus_chain()->push_back(view_b_.Clone());
  manager_->OnFocusChange(std::move(focus_chain_update), []() {});
  // Note that since the listener is no longer listening, it should still hold the view_ref_koid to
  // the last value it was listening to.
  EXPECT_NE(listener_->view_ref_koid(), view_b_.koid());
  EXPECT_EQ(listener_->view_ref_koid(), view_a_.koid());
}

TEST_F(FocusChainManagerTest, MultipleListeners) {
  listener_->Add(manager_.get());
  auto listener_2 = std::make_unique<MockAccessibilityFocusChainListener>();
  listener_2->Add(manager_.get());
  fuchsia::ui::focus::FocusChain focus_chain;
  focus_chain.mutable_focus_chain()->push_back(root_view_.Clone());
  focus_chain.mutable_focus_chain()->push_back(view_a_.Clone());
  manager_->OnFocusChange(std::move(focus_chain), []() {});
  EXPECT_EQ(listener_->view_ref_koid(), view_a_.koid());
  EXPECT_EQ(listener_2->view_ref_koid(), view_a_.koid());
  // Removes |listener_| from listening. The next Focus Chain should only reach the second listener
  // registered.
  listener_->Remove();
  RunLoopUntilIdle();
  fuchsia::ui::focus::FocusChain focus_chain_update;
  focus_chain_update.mutable_focus_chain()->push_back(root_view_.Clone());
  focus_chain_update.mutable_focus_chain()->push_back(view_b_.Clone());
  manager_->OnFocusChange(std::move(focus_chain_update), []() {});
  // Note that since the listener is no longer listening, it should still hold the view_ref_koid to
  // the last value it was listening to.
  EXPECT_EQ(listener_->view_ref_koid(), view_a_.koid());
  // The registered listener gets the real value.
  EXPECT_EQ(listener_2->view_ref_koid(), view_b_.koid());
}

TEST_F(FocusChainManagerTest, AccessibilityFocusChainRequesterViewHasSemantics) {
  // View is providing semantics, so request is granted.
  mock_semantics_source_.AddViewRef(view_a_.Clone());
  auto* requester = manager_.get();
  bool success = false;
  requester->ChangeFocusToView(view_a_.koid(), [&success](bool result) { success = result; });
  RunLoopUntilIdle();
  auto requested_view_ref = mock_a11y_view_.focused_view_ref();
  ASSERT_TRUE(requested_view_ref.has_value());
  EXPECT_EQ(a11y::GetKoid(std::move(requested_view_ref.value())), view_a_.koid());

  mock_a11y_view_.invoke_focus_callback({});
  EXPECT_TRUE(success);
}

TEST_F(FocusChainManagerTest, AccessibilityFocusChainRequesterViewDoesNotHaveSemantics) {
  // View is not providing semantics, so request is denied.
  auto* requester = manager_.get();
  // Set return value for ViewHasSemantics() to false.
  mock_semantics_source_.set_view_has_semantics(false);
  requester->ChangeFocusToView(view_a_.koid(), [](bool /* unused */) {});
  RunLoopUntilIdle();
  auto requested_view_ref = mock_a11y_view_.focused_view_ref();
  EXPECT_FALSE(requested_view_ref.has_value());
}

TEST_F(FocusChainManagerTest, AccessibilityFocusChainRequesterFocuserDenies) {
  mock_semantics_source_.AddViewRef(view_a_.Clone());
  auto* requester = manager_.get();
  bool success = true;  // Expects false later.
  requester->ChangeFocusToView(view_a_.koid(), [&success](bool result) { success = result; });
  RunLoopUntilIdle();
  auto requested_view_ref = mock_a11y_view_.focused_view_ref();
  ASSERT_TRUE(requested_view_ref.has_value());

  mock_a11y_view_.invoke_focus_callback(
      fuchsia::ui::views::Focuser_RequestFocus_Result::WithErr(fuchsia::ui::views::Error::DENIED));
  EXPECT_FALSE(success);
}

TEST_F(FocusChainManagerTest, AccessibilityFocusChainRequesterViewHasVisibleVirtualkeyboard) {
  mock_semantics_source_.AddViewRef(view_a_.Clone());
  mock_semantics_source_.set_has_visible_keyboard(true);
  auto* requester = manager_.get();
  bool success = false;
  requester->ChangeFocusToView(view_a_.koid(), [&success](bool result) { success = result; });
  RunLoopUntilIdle();
  EXPECT_TRUE(success);
  // The request should be successful, but the focus chain does not update to the view with the
  // virtual keyboard.
  auto requested_view_ref = mock_a11y_view_.focused_view_ref();
  EXPECT_FALSE(requested_view_ref.has_value());
}

}  // namespace
}  // namespace a11y
