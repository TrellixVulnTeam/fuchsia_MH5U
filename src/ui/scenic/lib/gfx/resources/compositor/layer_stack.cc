// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/gfx/resources/compositor/layer_stack.h"

#include <lib/syslog/cpp/macros.h>

#include "src/ui/scenic/lib/gfx/resources/compositor/layer.h"
#include "src/ui/scenic/lib/scenic/util/error_reporter.h"

namespace scenic_impl {
namespace gfx {

const ResourceTypeInfo LayerStack::kTypeInfo = {ResourceType::kLayerStack, "LayerStack"};

LayerStack::LayerStack(Session* session, SessionId session_id, ResourceId id)
    : Resource(session, session_id, id, LayerStack::kTypeInfo) {}

LayerStack::~LayerStack() = default;

bool LayerStack::AddLayer(LayerPtr layer, ErrorReporter* reporter) {
  if (layer->layer_stack_) {
    reporter->ERROR() << "LayerStack::AddLayer(): layer already belongs to a LayerStack.";
    return false;
  }
  if (layers_.size() > 0) {
    reporter->ERROR() << "LayerStack::AddLayer(): LayerStack can only contain a single layer.";
    return false;
  }
  layer->layer_stack_ = this;
  layers_.insert(std::move(layer));
  return true;
}

bool LayerStack::RemoveLayer(LayerPtr layer, ErrorReporter* reporter) {
  if (layer->layer_stack_ != this) {
    reporter->ERROR() << "LayerStack::RemoveLayer(): layer doesn't belong to this stack.";
    return false;
  }
  layer->layer_stack_ = nullptr;
  layers_.erase(layer);
  return true;
}

bool LayerStack::RemoveAllLayers() {
  for (const auto& layer : layers_) {
    layer->layer_stack_ = nullptr;
  }
  layers_.clear();
  return true;
}

void LayerStack::RemoveLayer(Layer* layer) {
  auto it = std::find_if(layers_.begin(), layers_.end(),
                         [layer](const LayerPtr& layer_ptr) { return layer == layer_ptr.get(); });
  FX_DCHECK(it != layers_.end());
  layers_.erase(it);
  (*it)->layer_stack_ = nullptr;
}

}  // namespace gfx
}  // namespace scenic_impl
