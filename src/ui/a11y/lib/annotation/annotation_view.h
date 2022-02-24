// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_ANNOTATION_ANNOTATION_VIEW_H_
#define SRC_UI_A11Y_LIB_ANNOTATION_ANNOTATION_VIEW_H_

#include <fuchsia/ui/annotation/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/ui/scenic/cpp/commands.h>
#include <lib/ui/scenic/cpp/view_token_pair.h>

#include <memory>
#include <optional>

namespace a11y {

class AnnotationViewInterface {
 public:
  using ViewPropertiesChangedCallback = fit::function<void()>;
  using ViewAttachedCallback = fit::function<void()>;
  using ViewDetachedCallback = fit::function<void()>;

  AnnotationViewInterface() = default;
  virtual ~AnnotationViewInterface() = default;

  virtual void InitializeView(fuchsia::ui::views::ViewRef client_view_ref) = 0;

  // Draws four rectangles corresponding to the top, bottom, left, and right edges the specified
  // bounding box. scale_vector and translation_vector describe the transform from the view's
  // coordinate space to the space the bounding box is in.
  virtual void DrawHighlight(const fuchsia::ui::gfx::BoundingBox& bounding_box,
                             const std::array<float, 3>& scale_vector,
                             const std::array<float, 3>& translation_vector,
                             bool is_magnification_highlight) = 0;

  // Clears all annotations in the client view.
  virtual void ClearAllAnnotations() = 0;

  // Clears only focus highlights.
  virtual void ClearFocusHighlights() = 0;

  // Clears only magnification viewport highlights.
  virtual void ClearMagnificationHighlights() = 0;
};

// The AnnotationView class enables the fuchsia accessibility manager to draw annotations over
// client views.
class AnnotationView : public fuchsia::ui::scenic::SessionListener, public AnnotationViewInterface {
 public:
  // Stores state of annotation view.
  struct AnnotationViewState {
    // True after annotation view has been registered via the scenic annotation registry API.
    bool annotation_view_registered = false;

    // True after the annotation view's node tree has been set up.
    bool tree_initialized = false;

    // True if annotations are currently attached to client view, and false otherwise.
    bool view_content_attached = false;

    // True if magnification highlights are currently attached to client view,
    // and false otherwise.
    bool magnification_content_attached = false;
  };

  explicit AnnotationView(sys::ComponentContext* component_context,
                          ViewPropertiesChangedCallback view_properties_changed_callback,
                          ViewAttachedCallback view_attached_callback,
                          ViewDetachedCallback view_detached_callback);

  ~AnnotationView() override = default;

  // |AnnotationViewInterface|
  void InitializeView(fuchsia::ui::views::ViewRef client_view_ref) override;

  // |AnnotationViewInterface|
  void DrawHighlight(const fuchsia::ui::gfx::BoundingBox& bounding_box,
                     const std::array<float, 3>& scale_vector,
                     const std::array<float, 3>& translation_vector,
                     bool is_magnification_highlight) override;

  // |AnnotationViewInterface|
  void ClearAllAnnotations() override;

  // |AnnotationViewInterface|
  void ClearFocusHighlights() override;

  // |AnnotationViewInterface|
  void ClearMagnificationHighlights() override;

  zx_koid_t koid() { return client_view_koid_; }

  // Width of the four rectangles that constitute the boundaries of the highlight.
  static constexpr float kHighlightEdgeThickness = 5.f;

  // IDs for resources common to all screen reader annotations.
  static constexpr uint32_t kAnnotationViewId = 1;
  static constexpr uint32_t kFocusHighlightContentNodeId = 2;
  static constexpr uint32_t kFocusHighlightMaterialId = 3;
  static constexpr uint32_t kFocusHighlightLeftEdgeNodeId = 4;
  static constexpr uint32_t kFocusHighlightRightEdgeNodeId = 5;
  static constexpr uint32_t kFocusHighlightTopEdgeNodeId = 6;
  static constexpr uint32_t kFocusHighlightBottomEdgeNodeId = 7;

  // IDs for resources common to all magnification annotations.
  static constexpr uint32_t kMagnificationHighlightContentNodeId = 8;
  static constexpr uint32_t kMagnificationHighlightMaterialId = 9;
  static constexpr uint32_t kMagnificationHighlightLeftEdgeNodeId = 10;
  static constexpr uint32_t kMagnificationHighlightRightEdgeNodeId = 11;
  static constexpr uint32_t kMagnificationHighlightTopEdgeNodeId = 12;
  static constexpr uint32_t kMagnificationHighlightBottomEdgeNodeId = 13;

 private:
  // Draws a rectangle to represent one edge of a highlight bounding box.
  void DrawHighlightEdge(std::vector<fuchsia::ui::scenic::Command>* cmds, int parent_node_id,
                         float width, float height, float center_x, float center_y,
                         float elevation);

  // Creates a node to hold one of the four highlight rectangle edges.
  void CreateHighlightEdgeNode(std::vector<fuchsia::ui::scenic::Command>* cmds, int edge_node_id);

  // Creates a node to hold one of the four edges of a magnification highlight.
  void CreateMagnificationEdgeNode(std::vector<fuchsia::ui::scenic::Command>* cmds,
                                   int edge_node_id);

  // Helper function to build a list of commands to enqueue.
  static void PushCommand(std::vector<fuchsia::ui::scenic::Command>* cmds,
                          fuchsia::ui::gfx::Command cmd);

  // Helper method to detach a node from the view.
  void DetachViewContents(uint32_t node_to_detach);

  // Scenic error handler.
  void OnScenicError(std::string error) override {}

  // Scenic event handler.
  void OnScenicEvent(std::vector<fuchsia::ui::scenic::Event> events) override;

  // Helper function to handle gfx events (e.g. switching or resizing view).
  void HandleGfxEvent(const fuchsia::ui::gfx::Event& event);

  // Stores state of annotation view
  AnnotationViewState state_;

  // Scenic session listener.
  fidl::Binding<fuchsia::ui::scenic::SessionListener> session_listener_binding_;

  // Callback invoked when client view properties have changed.
  ViewPropertiesChangedCallback view_properties_changed_callback_;

  // Callback invoked when client view is attached to scene graph.
  ViewAttachedCallback view_attached_callback_;

  // Callback invoked when client view is detached from scene graph.
  ViewDetachedCallback view_detached_callback_;

  // The properties (bounding box etc.) of its "parent" View.
  fuchsia::ui::gfx::ViewProperties parent_view_properties_;

  // Client view koid.
  zx_koid_t client_view_koid_;

  // Scenic session interface.
  fuchsia::ui::scenic::SessionPtr session_;

  // Interface between a11y manager and Scenic annotation registry to register annotation
  // viewholder with Scenic.
  fuchsia::ui::annotation::RegistryPtr annotation_registry_;

  uint32_t next_resource_id_ = 14;
};

class AnnotationViewFactoryInterface {
 public:
  AnnotationViewFactoryInterface() = default;
  virtual ~AnnotationViewFactoryInterface() = default;

  virtual std::unique_ptr<AnnotationViewInterface> CreateAndInitAnnotationView(
      fuchsia::ui::views::ViewRef client_view_ref, sys::ComponentContext* context,
      AnnotationViewInterface::ViewPropertiesChangedCallback view_properties_changed_callback,
      AnnotationViewInterface::ViewAttachedCallback view_attached_callback,
      AnnotationViewInterface::ViewDetachedCallback view_detached_callback) = 0;
};

class AnnotationViewFactory : public AnnotationViewFactoryInterface {
 public:
  AnnotationViewFactory() = default;
  ~AnnotationViewFactory() override = default;

  std::unique_ptr<AnnotationViewInterface> CreateAndInitAnnotationView(
      fuchsia::ui::views::ViewRef client_view_ref, sys::ComponentContext* context,
      AnnotationViewInterface::ViewPropertiesChangedCallback view_properties_changed_callback,
      AnnotationViewInterface::ViewAttachedCallback view_attached_callback,
      AnnotationViewInterface::ViewDetachedCallback view_detached_callback) override;
};

}  // namespace a11y

#endif  // SRC_UI_A11Y_LIB_ANNOTATION_ANNOTATION_VIEW_H_
