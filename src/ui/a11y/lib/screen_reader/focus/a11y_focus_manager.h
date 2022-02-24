// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_SCREEN_READER_FOCUS_A11Y_FOCUS_MANAGER_H_
#define SRC_UI_A11Y_LIB_SCREEN_READER_FOCUS_A11Y_FOCUS_MANAGER_H_

#include <lib/async/cpp/wait.h>
#include <lib/inspect/cpp/inspect.h>

#include <unordered_map>

#include "src/ui/a11y/lib/annotation/focus_highlight_manager.h"
#include "src/ui/a11y/lib/focus_chain/accessibility_focus_chain_listener.h"
#include "src/ui/a11y/lib/focus_chain/accessibility_focus_chain_requester.h"

namespace a11y {

// The A11yFocusManager keeps track of the a11y focus per view that is providing
// semantics.
//
// The a11y focus is defined as the semantic node which is selected in a certain
// view by the screen reader. There is only one active a11y focus, meaning that
// the screen reader cares only about one node per time. If the system changes
// the Focus Chain to a different view, the a11y focus also changes. This
// manager caches all a11y focus in each view as well as the active a11y focus.
//
// The a11y focus can be changed, which may trigger a Focus Chain Update if the active a11y focus is
// moving to another view.
class A11yFocusManager : public AccessibilityFocusChainListener {
 public:
  // Defines which view is currently in a11y focus along with the node_id of the node inside that
  // view.
  struct A11yFocusInfo {
    zx_koid_t view_ref_koid;
    uint32_t node_id;
  };

  // Callback which will be used to notify that an error is encountered while trying to set a11y
  // focus.
  using SetA11yFocusCallback = fit::function<void(bool)>;

  // Callback used to inform when the a11y focus changes.
  using OnA11yFocusUpdatedCallback = fit::function<void(std::optional<A11yFocusInfo>)>;

  // Root node id, which will be used to set the default node_id for a view.
  static constexpr uint32_t kRootNodeId = 0;

  static constexpr char kCurrentlyFocusedKoidInspectNodeName[] = "currently_focused_koid";
  static constexpr char kCurrentlyFocusedNodeIdInspectNodeName[] = "currently_focused_node_id";

  // |focus_chain_requester| and |registry| must outlive this object.
  explicit A11yFocusManager(AccessibilityFocusChainRequester* focus_chain_requester,
                            AccessibilityFocusChainRegistry* registry,
                            FocusHighlightManager* focus_highlight_manager,
                            inspect::Node inspect_node = inspect::Node());
  virtual ~A11yFocusManager();

  // Returns the current a11y focus if it exists.
  virtual std::optional<A11yFocusInfo> GetA11yFocus();

  // Sets the a11y focus to a different node. If the current focused view is different than the one
  // in this request, a focus Chain is first performed to change the focus to the other view.
  // |callback| is invoked when this operation is done, indicating if the request was granted. The
  // request can fail if the View is not providing semantics or if the Focus Chain request was
  // denied.
  virtual void SetA11yFocus(zx_koid_t koid, uint32_t node_id, SetA11yFocusCallback callback);

  // Clears existing a11y focus.
  virtual void ClearA11yFocus();

  // Removes current highlights (if any), and highlights node specified by identifier
  // |{currently_focused_view,
  // focused_node_in_view_map_[currently_focused_view_]}|.
  virtual void UpdateHighlights();

  // Registers a callback that is invoked when the a11y focus is updated. For now, only one callback
  // can be registered at a time.
  virtual void set_on_a11y_focus_updated_callback(
      OnA11yFocusUpdatedCallback on_a11y_focus_updated_callback) {
    on_a11y_focus_updated_callback_ = std::move(on_a11y_focus_updated_callback);
  }

 protected:
  // For mocks only.
  A11yFocusManager();

 private:
  // |AccessibilityFocusChainListener|
  void OnViewFocus(zx_koid_t view_ref_koid) override;

  // Helper function to update inspect info.
  void UpdateInspectProperties();

  // Map for storing node_id which is in a11y focus for every viewref_koid.
  // By default, root-node(node_id = 0) is set for a view in a11y focus.
  std::unordered_map<zx_koid_t /* viewref_koid */, uint32_t /* node_id */>
      focused_node_in_view_map_;

  // Stores the koid of the view which is currently in a11y focus.
  zx_koid_t currently_focused_view_ = ZX_KOID_INVALID;

  // Interface used to request Focus Chain Updates.
  AccessibilityFocusChainRequester* const focus_chain_requester_ = nullptr;

  // Used to manipulate semantic annotations.
  FocusHighlightManager* const focus_highlight_manager_ = nullptr;

  OnA11yFocusUpdatedCallback on_a11y_focus_updated_callback_;

  fxl::WeakPtrFactory<AccessibilityFocusChainListener> weak_ptr_factory_;

  // Inspect node to which to publish debug info.
  inspect::Node inspect_node_;

  // Inspect properties to store current a11y focus.
  inspect::UintProperty inspect_property_current_focus_koid_;
  inspect::UintProperty inspect_property_current_focus_node_id_;
};

}  // namespace a11y

#endif  // SRC_UI_A11Y_LIB_SCREEN_READER_FOCUS_A11Y_FOCUS_MANAGER_H_
