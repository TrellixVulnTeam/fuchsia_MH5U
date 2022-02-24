// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/gfx/resources/shapes/rectangle_shape.h"

namespace scenic_impl {
namespace gfx {

const ResourceTypeInfo RectangleShape::kTypeInfo = {ResourceType::kShape | ResourceType::kRectangle,
                                                    "RectangleShape"};

RectangleShape::RectangleShape(Session* session, SessionId session_id, ResourceId id,
                               float initial_width, float initial_height)
    : PlanarShape(session, session_id, id, RectangleShape::kTypeInfo),
      width_(initial_width),
      height_(initial_height) {}

bool RectangleShape::ContainsPoint(const escher::vec2& point) const {
  const escher::vec2 pt = point + escher::vec2(0.5f * width_, 0.5f * height_);
  constexpr float kEpsilon = 1e-3f;
  return pt.x >= -kEpsilon && pt.y >= -kEpsilon && pt.x <= width_ + kEpsilon &&
         pt.y <= height_ + kEpsilon;
}

}  // namespace gfx
}  // namespace scenic_impl
