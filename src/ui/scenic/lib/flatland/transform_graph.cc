// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/flatland/transform_graph.h"

#include <lib/syslog/cpp/macros.h>

#include "src/ui/scenic/lib/utils/logging.h"

namespace flatland {

TransformGraph::TransformGraph() : TransformGraph(0) {}

TransformGraph::TransformGraph(TransformHandle::InstanceId instance_id)
    : instance_id_(instance_id) {}

TransformHandle TransformGraph::CreateTransform() {
  FX_DCHECK(is_valid_);
  TransformHandle retval(instance_id_, next_transform_id_++);
  FX_DCHECK(!working_set_.count(retval));
  working_set_.insert(retval);
  live_set_.insert(retval);
  return retval;
}

bool TransformGraph::ReleaseTransform(TransformHandle handle) {
  FX_DCHECK(is_valid_);
  auto iter = working_set_.find(handle);
  if (iter == working_set_.end()) {
    return false;
  }

  working_set_.erase(iter);
  return true;
}

bool TransformGraph::HasChildren(TransformHandle parent) const {
  return children_.find({parent, NORMAL}) != children_.end();
}

void TransformGraph::ClearChildren(TransformHandle parent) {
  FX_DCHECK(is_valid_);
  FX_DCHECK(working_set_.count(parent));
  children_.erase({parent, NORMAL});
}

bool TransformGraph::AddChild(TransformHandle parent, TransformHandle child) {
  FX_DCHECK(is_valid_);
  FX_DCHECK(working_set_.count(parent));

  auto [iter, end_iter] = children_.equal_range({parent, NORMAL});
  for (; iter != end_iter; ++iter) {
    if (iter->second == child) {
      FLATLAND_VERBOSE_LOG << "TransformGraph::AddChild(" << parent << "," << child
                           << "): failure!";

      return false;
    }
  }

  children_.insert({{parent, NORMAL}, child});
  FLATLAND_VERBOSE_LOG << "TransformGraph::AddChild(" << parent << "," << child << "): success!";

  return true;
}

bool TransformGraph::RemoveChild(TransformHandle parent, TransformHandle child) {
  FX_DCHECK(is_valid_);
  FX_DCHECK(working_set_.count(parent));

  auto [iter, end_iter] = children_.equal_range({parent, NORMAL});
  for (; iter != end_iter; ++iter) {
    if (iter->second == child) {
      children_.erase(iter);
      FLATLAND_VERBOSE_LOG << "TransformGraph::RemoveChild(" << parent << "," << child
                           << "): success!";

      return true;
    }
  }

  FLATLAND_VERBOSE_LOG << "TransformGraph::RemoveChild(" << parent << "," << child << "): failure!";

  return false;
}

void TransformGraph::SetPriorityChild(TransformHandle parent, TransformHandle child) {
  FX_DCHECK(is_valid_);
  FX_DCHECK(working_set_.count(parent));

  children_.erase({parent, PRIORITY});
  children_.insert({{parent, PRIORITY}, child});
}

void TransformGraph::ClearPriorityChild(TransformHandle parent) {
  FX_DCHECK(is_valid_);
  FX_DCHECK(working_set_.count(parent));

  children_.erase({parent, PRIORITY});
}

void TransformGraph::ResetGraph(TransformHandle exception) {
  FX_DCHECK(working_set_.count(exception));
  working_set_.clear();
  working_set_.insert(exception);
  children_.clear();
  is_valid_ = true;
}

TransformGraph::TopologyData TransformGraph::ComputeAndCleanup(TransformHandle start,
                                                               uint64_t max_iterations) {
  FX_DCHECK(is_valid_);
  FX_DCHECK(working_set_.count(start));

  TopologyData data;

  // Swap all the live nodes into the dead set, so we can pull them out as we visit them.
  std::swap(live_set_, data.dead_transforms);

  // Clone our children map. We will remove child links after we visit them, to avoid duplicate
  // work when traversing the entire working set of transforms.
  PriorityChildMap children_copy = children_;

  // Compute the topological set starting from the start transform.
  data.sorted_transforms =
      Traverse(start, children_copy, &data.cyclical_edges, max_iterations - data.iterations);
  data.iterations += data.sorted_transforms.size();
  for (auto [transform, child_count] : data.sorted_transforms) {
    auto [start, end] = EqualRangeAllPriorities(children_copy, transform);
    if (start != children_copy.cend()) {
      children_copy.erase(start, end);
    }
    data.dead_transforms.erase(transform);
    live_set_.insert(transform);
  }

  // Compute the topological set starting from every working set transform, for cleanup purposes.
  for (auto transform : working_set_) {
    auto working_transforms =
        Traverse(transform, children_copy, &data.cyclical_edges, max_iterations - data.iterations);
    data.iterations += working_transforms.size();
    for (auto [transform, child_count] : working_transforms) {
      auto [start, end] = EqualRangeAllPriorities(children_copy, transform);
      if (start != children_copy.cend()) {
        children_copy.erase(start, end);
      }
      data.dead_transforms.erase(transform);
      live_set_.insert(transform);
    }
  }

  // Cleanup child state for all dead nodes.
  for (auto transform : data.dead_transforms) {
    auto [start, end] = EqualRangeAllPriorities(children_, transform);
    if (start != children_.cend()) {
      children_.erase(start, end);
    }
  }

  if (data.iterations >= max_iterations) {
    is_valid_ = false;
  }

  return data;
}

TransformGraph::IteratorPair TransformGraph::EqualRangeAllPriorities(const PriorityChildMap& map,
                                                                     TransformHandle handle) {
  auto start = map.lower_bound({handle, PRIORITY});
  auto end = map.upper_bound({handle, NORMAL});
  FX_DCHECK(std::distance(start, end) >= 0);
  return {start, end};
}

TransformGraph::TopologyVector TransformGraph::Traverse(TransformHandle start,
                                                        const PriorityChildMap& children,
                                                        ChildMap* cycles, uint64_t max_length) {
  TopologyVector retval;

  std::vector<IteratorPair> iterator_stack;
  std::vector<TransformHandle> ancestors;
  std::vector<uint64_t> parent_indices;

  // Add the starting handle to the output, and initialize our state.
  auto start_pair = EqualRangeAllPriorities(children, start);
  iterator_stack.push_back(start_pair);
  retval.push_back(
      {start, static_cast<uint64_t>(std::distance(start_pair.first, start_pair.second))});
  ancestors.push_back(start);
  parent_indices.push_back(0);

  // Iterate until we're done, or until we run out of space
  while (!iterator_stack.empty() && retval.size() < max_length) {
    auto& [child_iter, end_iter] = iterator_stack.back();

    // If we're at the end of this iterator, pop to the parent iterator.
    if (child_iter == end_iter) {
      iterator_stack.pop_back();
      ancestors.pop_back();
      if (!parent_indices.empty()) {
        parent_indices.pop_back();
      }
      continue;
    }

    const TransformHandle child = child_iter->second;
    // We increment the child iterator here, instead of at the end of the loop, because the new
    // child may cause the iterator_stack to mutate, invalidating the live references we've
    // captured.
    ++child_iter;

    // Search from the bottom of the stack (since it's more likely), looking for a cycle.
    if (std::find(ancestors.crbegin(), ancestors.crend(), child) != ancestors.crend()) {
      FX_DCHECK(cycles);
      FX_DCHECK(!parent_indices.empty());
      cycles->insert({retval[parent_indices.back()].handle, child});
    } else {
      // If the child is not part of a cycle, add it to the sorted list and update our state.
      parent_indices.push_back(retval.size());
      auto pair = EqualRangeAllPriorities(children, child);
      iterator_stack.push_back(pair);
      retval.push_back({child, static_cast<uint64_t>(std::distance(pair.first, pair.second))});
      ancestors.push_back(child);
    }
  }

  return retval;
}

}  // namespace flatland
