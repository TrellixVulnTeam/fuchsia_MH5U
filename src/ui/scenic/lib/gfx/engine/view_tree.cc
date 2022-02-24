// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/gfx/engine/view_tree.h"

#include <lib/syslog/cpp/macros.h>
#include <zircon/syscalls/object.h>

#include <sstream>
#include <utility>

#include "src/ui/scenic/lib/gfx/resources/view_holder.h"
#include "src/ui/scenic/lib/utils/helpers.h"
#include "src/ui/scenic/lib/utils/math.h"

namespace scenic_impl::gfx {

namespace {
// Convenience functions.
bool IsValid(zx_koid_t koid) { return koid != ZX_KOID_INVALID; }

std::optional<zx_koid_t> wrap(zx_koid_t koid) {
  return koid == ZX_KOID_INVALID ? std::nullopt : std::optional(koid);
}

// Sets the parents off all nodes in the subtree rooted at |root| to ZX_KOID_INVALID.
void OrphanSubgraph(std::unordered_map<zx_koid_t, view_tree::ViewNode>& nodes, zx_koid_t root) {
  auto& node = nodes.at(root);
  node.parent = ZX_KOID_INVALID;
  for (auto child : node.children) {
    OrphanSubgraph(nodes, child);
  }
  // No need to clear children, since this is only used to mark unconnected nodes.
}

}  // namespace

std::optional<zx_koid_t> ViewTree::ParentOf(zx_koid_t child) const {
  FX_DCHECK(IsTracked(child)) << "invariant";

  if (const auto ptr = std::get_if<AttachNode>(&nodes_.at(child))) {
    return wrap(ptr->parent);
  } else if (const auto ptr = std::get_if<RefNode>(&nodes_.at(child))) {
    return wrap(ptr->parent);
  }

  FX_NOTREACHED() << "impossible";
  return std::nullopt;
}

scheduling::SessionId ViewTree::SessionIdOf(zx_koid_t koid) const {
  if (!IsValid(koid) || !IsTracked(koid)) {
    return 0u;
  }

  if (const auto ptr = std::get_if<AttachNode>(&nodes_.at(koid))) {
    return 0u;
  } else if (const auto ptr = std::get_if<RefNode>(&nodes_.at(koid))) {
    return ptr->session_id;
  }

  FX_NOTREACHED() << "impossible";
  return 0u;
}

EventReporterWeakPtr ViewTree::EventReporterOf(zx_koid_t koid) const {
  if (!IsValid(koid) || !IsTracked(koid) || std::holds_alternative<AttachNode>(nodes_.at(koid))) {
    return EventReporterWeakPtr(/*nullptr*/);
  }

  if (auto ptr = std::get_if<RefNode>(&nodes_.at(koid))) {
    return ptr->event_reporter;
  }

  FX_NOTREACHED() << "impossible";
  return EventReporterWeakPtr(/*nullptr*/);
}

std::optional<zx_koid_t> ViewTree::ConnectedViewRefKoidOf(SessionId session_id) const {
  FX_DCHECK(root_ == ZX_KOID_INVALID ||
            (nodes_.count(root_) != 0 && std::holds_alternative<RefNode>(nodes_.at(root_))));
  // Root Session should act on authority of the scene koid, not the root view.
  if (root_ != ZX_KOID_INVALID && std::get<RefNode>(nodes_.at(root_)).session_id == session_id) {
    return root_;
  }

  const auto range = ref_node_koids_.equal_range(session_id);
  for (auto it = range.first; it != range.second; ++it) {
    const zx_koid_t koid = it->second;
    if (IsConnectedToScene(koid)) {
      return std::optional<zx_koid_t>(koid);
    }
  }
  return std::nullopt;
}

bool ViewTree::IsTracked(zx_koid_t koid) const { return IsValid(koid) && nodes_.count(koid) > 0; }

bool ViewTree::IsDescendant(zx_koid_t descendant_koid, zx_koid_t ancestor_koid) const {
  FX_DCHECK(IsTracked(descendant_koid)) << "precondition";
  FX_DCHECK(IsTracked(ancestor_koid)) << "precondition";

  zx_koid_t parent_koid = ZX_KOID_INVALID;
  if (const auto ptr = std::get_if<AttachNode>(&nodes_.at(descendant_koid))) {
    parent_koid = ptr->parent;
  } else if (const auto ptr = std::get_if<RefNode>(&nodes_.at(descendant_koid))) {
    parent_koid = ptr->parent;
  } else {
    FX_NOTREACHED() << "invariant: child/parent types are known";
    return false;  // Impossible.
  }

  if (!IsTracked(parent_koid))
    return false;  // Does not reach ancestor;

  if (parent_koid == ancestor_koid)
    return true;  // Parent is ancestor. Base case.

  return IsDescendant(parent_koid, ancestor_koid);  // Recursive.
}

bool ViewTree::IsConnectedToScene(zx_koid_t koid) const {
  FX_DCHECK(IsTracked(koid)) << "precondition";

  if (!IsValid(root_))
    return false;  // No connectivity, base case.

  if (koid == root_)
    return true;  // koid is the root and therefore connected.

  return IsDescendant(koid, root_);
}

bool ViewTree::IsRefNode(zx_koid_t koid) const {
  FX_DCHECK(IsTracked(koid)) << "precondition";
  return std::holds_alternative<RefNode>(nodes_.at(koid));
}

bool ViewTree::MayReceiveFocus(zx_koid_t koid) const {
  FX_DCHECK(IsTracked(koid) && IsRefNode(koid)) << "precondition";
  return std::get_if<RefNode>(&nodes_.at(koid))->may_receive_focus();
}

bool ViewTree::IsInputSuppressed(zx_koid_t koid) const {
  FX_DCHECK(IsTracked(koid) && IsRefNode(koid)) << "precondition";
  return std::get_if<RefNode>(&nodes_.at(koid))->is_input_suppressed();
}

std::optional<glm::mat4> ViewTree::GlobalTransformOf(zx_koid_t koid) const {
  if (!IsTracked(koid) || !IsRefNode(koid) || !IsConnectedToScene(koid)) {
    return std::nullopt;
  }

  return std::get_if<RefNode>(&nodes_.at(koid))->global_transform();
}

void ViewTree::HitTestFrom(zx_koid_t starting_view_koid, const escher::ray4& world_space_ray,
                           HitAccumulator<ViewHit>* accumulator, bool semantic_hit_test) const {
  if (!IsTracked(starting_view_koid) || !IsRefNode(starting_view_koid)) {
    FX_LOGS(WARNING) << "Tried to hit test starting from invalid view.";
    return;
  }

  std::get_if<RefNode>(&nodes_.at(starting_view_koid))
      ->hit_test(world_space_ray, accumulator, semantic_hit_test);
}

zx_status_t ViewTree::AddAnnotationViewHolder(zx_koid_t koid, ViewHolderPtr annotation) const {
  if (!IsValid(koid)) {
    return ZX_ERR_INVALID_ARGS;
  }
  if (!IsTracked(koid)) {
    return ZX_ERR_NOT_FOUND;
  }
  if (!IsRefNode(koid)) {
    return ZX_ERR_INVALID_ARGS;
  }

  auto node = std::get_if<RefNode>(&nodes_.at(koid));
  if (!node->add_annotation_view_holder) {
    return ZX_ERR_PEER_CLOSED;
  }
  node->add_annotation_view_holder(std::move(annotation));

  return ZX_OK;
}

bool ViewTree::IsStateValid() const {
  // Map state
  for (const auto& item : nodes_) {
    if (!IsValid(item.first)) {
      FX_LOGS(ERROR) << "Map key is invalid koid.";
      return false;
    }
    if (const auto ptr = std::get_if<AttachNode>(&item.second)) {
      if (IsValid(ptr->parent)) {
        if (!IsTracked(ptr->parent)) {
          FX_LOGS(ERROR) << "Map item's parent is valid but isn't tracked: " << ptr->parent;
          return false;
        }
        if (!std::holds_alternative<RefNode>(nodes_.at(ptr->parent))) {
          FX_LOGS(ERROR) << "Map item's parent should be a RefNode: " << ptr->parent;
          return false;
        }
      }
    } else if (const auto ptr = std::get_if<RefNode>(&item.second)) {
      if (IsValid(ptr->parent)) {
        if (!IsTracked(ptr->parent)) {
          FX_LOGS(ERROR) << "Map item's parent is valid but isn't tracked: " << ptr->parent;
          return false;
        }
        if (!std::holds_alternative<AttachNode>(nodes_.at(ptr->parent))) {
          FX_LOGS(ERROR) << "Map item's parent should be an AttachNode: " << ptr->parent;
          return false;
        }
        // Only one entity must have ptr->parent as a parent.
        size_t parent_count = 0;
        for (const auto& j_item : nodes_) {
          if (const auto j_ptr = std::get_if<AttachNode>(&j_item.second)) {
            if (j_ptr->parent == ptr->parent)
              ++parent_count;
          } else if (const auto j_ptr = std::get_if<RefNode>(&j_item.second)) {
            if (j_ptr->parent == ptr->parent)
              ++parent_count;
          } else {
            FX_NOTREACHED() << "unknown type";
            return false;
          }
        }
        if (parent_count != 1) {
          FX_LOGS(ERROR) << "Map item's parent should have just one child: " << ptr->parent
                         << ", count: " << parent_count;
          return false;
        }
      }
    } else {
      FX_NOTREACHED() << "unknown type";
      return false;
    }
  }

  // SessionId -> RefNode KOID  map state
  for (const auto& item : ref_node_koids_) {
    const SessionId session_id = item.first;
    const zx_koid_t koid = item.second;
    if (session_id == 0u) {
      FX_LOGS(ERROR) << "Map key is invalid SessionId.";
      return false;
    }
    if (!IsValid(koid) || !IsTracked(koid)) {
      FX_LOGS(ERROR) << "Map value isn't a valid and tracked koid.";
      return false;
    }
    const auto ptr = std::get_if<RefNode>(&nodes_.at(koid));
    if (ptr == nullptr) {
      FX_LOGS(ERROR) << "Map item should refer to a RefNode: " << koid;
      return false;
    }
    if (ptr->session_id != session_id) {
      FX_LOGS(ERROR) << "Declared SessionId doesn't match: " << ptr->session_id << ", "
                     << session_id;
      return false;
    }
    // Count of connected KOIDs from this session_id is at most 1.
    int connected_non_root_koid = 0;
    const auto range = ref_node_koids_.equal_range(session_id);
    for (auto it = range.first; it != range.second; ++it) {
      if (it->second != root_ && IsConnectedToScene(it->second)) {
        ++connected_non_root_koid;
      }
    }
    if (connected_non_root_koid > 1) {
      FX_LOGS(ERROR) << "Count of scene-connected ViewRefs for session " << session_id
                     << " exceeds 1. Reference fxbug.dev/24450.";
      // TODO(fxbug.dev/24450): Enable invariant check when one-view-per-session is enforced.
      // return false;
    }
  }

  // Scene state
  if (IsValid(root_)) {
    if (!IsTracked(root_)) {
      FX_LOGS(ERROR) << "Scene is valid but isn't tracked: " << root_;
      return false;
    }
    if (!std::holds_alternative<RefNode>(nodes_.at(root_))) {
      FX_LOGS(ERROR) << "Scene should be a RefNode but isn't: " << root_;
      return false;
    }
  }

  return true;
}

void ViewTree::NewRefNode(ViewTreeNewRefNode new_node) {
  const zx_koid_t koid = utils::ExtractKoid(new_node.view_ref);
  FX_DCHECK(IsValid(koid)) << "precondition";
  FX_DCHECK(!IsTracked(koid)) << "precondition";
  FX_DCHECK(new_node.may_receive_focus) << "precondition";           // Callback exists.
  FX_DCHECK(new_node.is_input_suppressed) << "precondition";         // Callback exists.
  FX_DCHECK(new_node.global_transform) << "precondition";            // Callback exists.
  FX_DCHECK(new_node.bounding_box) << "precondition";                // Callback exists.
  FX_DCHECK(new_node.hit_test) << "precondition";                    // Callback exists.
  FX_DCHECK(new_node.add_annotation_view_holder) << "precondition";  // Callback exists.
  FX_DCHECK(new_node.session_id != scheduling::kInvalidSessionId) << "precondition";

  if (!IsValid(koid) || IsTracked(koid))
    return;  // Bail.

  nodes_[koid] = RefNode{
      .view_ref = std::make_shared<fuchsia::ui::views::ViewRef>(std::move(new_node.view_ref)),
      .event_reporter = new_node.event_reporter,
      .may_receive_focus = std::move(new_node.may_receive_focus),
      .is_input_suppressed = std::move(new_node.is_input_suppressed),
      .global_transform = std::move(new_node.global_transform),
      .bounding_box = std::move(new_node.bounding_box),
      .hit_test = std::move(new_node.hit_test),
      .add_annotation_view_holder = std::move(new_node.add_annotation_view_holder),
      .session_id = new_node.session_id};

  ref_node_koids_.insert({new_node.session_id, koid});

  FX_DCHECK(IsStateValid()) << "postcondition";
}

void ViewTree::NewAttachNode(zx_koid_t koid) {
  FX_DCHECK(IsValid(koid)) << "precondition";
  FX_DCHECK(!IsTracked(koid)) << "precondition";

  if (!IsValid(koid) || IsTracked(koid))
    return;  // Bail.

  nodes_[koid] = AttachNode{};

  FX_DCHECK(IsStateValid()) << "postcondition";
}

void ViewTree::DeleteNode(const zx_koid_t koid) {
  FX_DCHECK(IsTracked(koid)) << "precondition";

  // Remove from view ref koid mapping, if applicable.
  if (IsRefNode(koid)) {
    for (auto it = ref_node_koids_.begin(); it != ref_node_koids_.end(); ++it) {
      if (it->second == koid) {
        ref_node_koids_.erase(it);
        break;  // |it| is invalid, but we exit loop immediately.
      }
    }
  }

  // Remove from node set.
  nodes_.erase(koid);

  // Remove from node set's parent references.
  for (auto& item : nodes_) {
    if (auto ptr = std::get_if<AttachNode>(&item.second)) {
      if (ptr->parent == koid) {
        ptr->parent = ZX_KOID_INVALID;
      }
    } else if (auto ptr = std::get_if<RefNode>(&item.second)) {
      if (ptr->parent == koid) {
        ptr->parent = ZX_KOID_INVALID;
      }
    } else {
      FX_NOTREACHED() << "unknown type";
    }
  }

  // Remove |koid| if it is the root.
  if (root_ == koid) {
    root_ = ZX_KOID_INVALID;
  }

  FX_DCHECK(IsStateValid()) << "postcondition";
}

void ViewTree::MakeGlobalRoot(zx_koid_t koid) {
  FX_DCHECK(!IsValid(koid) || (IsTracked(koid) && IsRefNode(koid) && MayReceiveFocus(koid)))
      << "precondition";

  root_ = koid;

  FX_DCHECK(IsStateValid()) << "postcondition";
}

void ViewTree::ConnectToParent(zx_koid_t child, zx_koid_t parent) {
  FX_DCHECK(IsTracked(child)) << "precondition";
  FX_DCHECK(IsTracked(parent)) << "precondition";

  if (auto ptr = std::get_if<AttachNode>(&nodes_[child])) {
    if (std::holds_alternative<RefNode>(nodes_[parent])) {
      ptr->parent = parent;
      FX_DCHECK(IsStateValid()) << "postcondition";
      return;
    }
  } else if (auto ptr = std::get_if<RefNode>(&nodes_[child])) {
    if (std::holds_alternative<AttachNode>(nodes_[parent])) {
      ptr->parent = parent;
      FX_DCHECK(IsStateValid()) << "postcondition";
      return;
    }
  }
  FX_NOTREACHED() << "invariant: child/parent types must be known and must be different";
}

void ViewTree::DisconnectFromParent(zx_koid_t child) {
  FX_DCHECK(IsTracked(child)) << "precondition";

  if (auto ptr = std::get_if<AttachNode>(&nodes_[child])) {
    if (!IsTracked(ptr->parent))
      return;  // Parent (a RefNode) was never set, or already deleted.

    if (auto parent_ptr = std::get_if<RefNode>(&nodes_[ptr->parent])) {
      ptr->parent = ZX_KOID_INVALID;
      FX_DCHECK(IsStateValid()) << "postcondition";
      return;
    }
  } else if (auto ptr = std::get_if<RefNode>(&nodes_[child])) {
    if (!IsTracked(ptr->parent))
      return;  // Parent (an AttachNode) was never set, or already deleted.

    if (auto parent_ptr = std::get_if<AttachNode>(&nodes_[ptr->parent])) {
      ptr->parent = ZX_KOID_INVALID;
      FX_DCHECK(IsStateValid()) << "postcondition";
      return;
    }
  }
  FX_NOTREACHED() << "invariant: child/parent types are known and correct";
}

void ViewTree::InvalidateAnnotationViewHolder(zx_koid_t koid) {
  if (!IsTracked(koid)) {
    return;
  }

  auto node = std::get_if<RefNode>(&nodes_.at(koid));
  node->add_annotation_view_holder = nullptr;
}

std::string ViewTree::ToString() const {
  std::stringstream output;

  output << std::endl << "ViewTree Dump" << std::endl;
  output << "  root: " << root_ << std::endl;
  output << "  nodes: " << std::endl;
  for (const auto& item : nodes_) {
    if (const auto ptr = std::get_if<AttachNode>(&item.second)) {
      output << "    attach-node(" << item.first << ") -> parent: " << ptr->parent << std::endl;
    } else if (const auto ptr = std::get_if<RefNode>(&item.second)) {
      output << "    ref-node(" << item.first << ") -> parent: " << ptr->parent
             << ", event-reporter: " << ptr->event_reporter.get()
             << ", may-receive-focus: " << std::boolalpha << ptr->may_receive_focus()
             << ", session-id: " << ptr->session_id << std::endl;
    } else {
      FX_NOTREACHED() << "impossible";
    }
  }
  output << "  ref-node-koids:" << std::endl;
  for (const auto& item : ref_node_koids_) {
    output << "    session-id " << item.first << " has koid " << item.second << std::endl;
  }

  return output.str();
}

view_tree::SubtreeSnapshot ViewTree::Snapshot() const {
  // Create a ViewNode for each RefNode.
  std::unordered_map<zx_koid_t, view_tree::ViewNode> all_nodes;
  for (const auto& [koid, variant_node] : nodes_) {
    // Ignore AttachNodes.
    if (const auto& ref_node = std::get_if<RefNode>(&variant_node)) {
      all_nodes[koid];  // Creates the element if it doesn't exist.

      // If there's a RefNode parent we add this as a child of it.
      if (ref_node->parent != ZX_KOID_INVALID) {
        const auto& attach_node = std::get<AttachNode>(nodes_.at(ref_node->parent));
        if (attach_node.parent != ZX_KOID_INVALID) {
          const zx_koid_t ref_parent = attach_node.parent;
          all_nodes[koid].parent = ref_parent;
          all_nodes[ref_parent].children.emplace(koid);
        }
      }
    }
  }

  {  // To sort out unconnected nodes: set all dangling nodes as having no parents.
    std::vector<zx_koid_t> dangling_nodes;
    for (const auto& [koid, node] : all_nodes) {
      if (node.parent == ZX_KOID_INVALID && koid != root_) {
        dangling_nodes.emplace_back(koid);
      }
    }

    for (auto koid : dangling_nodes) {
      OrphanSubgraph(all_nodes, koid);
    }
  }

  view_tree::SubtreeSnapshot snapshot{
      // Gfx does not currently support other compositors as subtrees.
      .tree_boundaries = {}};
  auto& [root, view_tree, unconnected_views, hit_tester, tree_boundaries] = snapshot;
  root = root_;

  // Sort out all nodes without parents.
  for (auto& [koid, node] : all_nodes) {
    if (node.parent == ZX_KOID_INVALID && koid != root) {
      unconnected_views.insert(koid);
    } else {
      // If it does have a parent, add it to |view_tree|.
      auto [it, success] = view_tree.emplace(koid, std::move(node));
      FX_DCHECK(success);
      const auto& ref_node = std::get<RefNode>(nodes_.at(koid));
      const glm::mat4 world_from_view_transform = ref_node.global_transform();
      it->second.local_from_world_transform = glm::inverse(world_from_view_transform);
      it->second.is_focusable = ref_node.may_receive_focus();
      const escher::BoundingBox bbox = ref_node.bounding_box();
      it->second.bounding_box = {.min = {bbox.min().x, bbox.min().y},
                                 .max = {bbox.max().x, bbox.max().y}};
      it->second.view_ref = ref_node.view_ref;  // Copy shared_ptr.
    }
  }

  // Set up the hit tester.
  // TODO(fxbug.dev/74533): The hit testing closures generated here are not thread safe.
  hit_tester = [this](zx_koid_t starting_view_koid, glm::vec2 view_local_point, bool is_semantic) {
    const std::optional<glm::mat4> world_from_view_transform =
        GlobalTransformOf(starting_view_koid);
    if (!world_from_view_transform.has_value()) {
      return view_tree::SubtreeHitTestResult{};
    }

    const auto world_point =
        utils::TransformPointerCoords(view_local_point, world_from_view_transform.value());
    const auto world_z_ray = escher::ray4{
        .origin = {world_point.x, world_point.y, -1000, 1},
        .direction = {0, 0, 1, 0},
    };

    // Gfx does not support embedding other subtrees so we never have continuations.
    view_tree::SubtreeHitTestResult results{.continuations = {}};
    // Perform hit test.
    gfx::ViewHitAccumulator accumulator;
    HitTestFrom(starting_view_koid, world_z_ray, &accumulator, is_semantic);
    for (const auto& hit : accumulator.hits()) {
      results.hits.emplace_back(hit.view_ref_koid);
    }
    return results;
  };

  return snapshot;
}

}  // namespace scenic_impl::gfx
