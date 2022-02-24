// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_GFX_RESOURCES_RESOURCE_TYPE_INFO_H_
#define SRC_UI_SCENIC_LIB_GFX_RESOURCES_RESOURCE_TYPE_INFO_H_

#include <cstdint>

namespace scenic_impl {
namespace gfx {

// All subclasses of Resource are represented here.
enum ResourceType : uint64_t {
  // Low-level resources.
  kMemory = 1ul << 1,
  kHostMemory = 1ul << 2,
  kGpuMemory = 1ul << 3,
  kImageBase = 1ul << 4,
  kImage = 1ul << 5,
  kHostImage = 1ul << 6,
  kGpuImage = 1ul << 7,
  kImagePipe = 1ul << 8,
  kBuffer = 1ul << 9,

  // Shapes.
  kShape = 1ul << 10,
  kRectangle = 1ul << 11,
  kRoundedRectangle = 1ul << 12,
  kCircle = 1ul << 13,
  kMesh = 1ul << 14,

  // Materials.
  kMaterial = 1ul << 15,

  // Views.
  kView = 1ul << 16,
  kViewNode = 1ul << 17,
  kViewHolder = 1ul << 18,

  // Nodes.
  kNode = 1ul << 19,
  kClipNode = 1ul << 20,
  kEntityNode = 1ul << 21,
  kOpacityNode = 1ul << 22,
  kShapeNode = 1ul << 23,

  // Compositor, layers.
  kCompositor = 1ul << 24,
  kDisplayCompositor = 1ul << 25,
  kLayer = 1ul << 26,
  kLayerStack = 1ul << 27,

  // Scene, camera, lighting.
  kScene = 1ul << 28,
  kCamera = 1ul << 29,
  kStereoCamera = 1ul << 30,
  kLight = 1ul << 31,
  kAmbientLight = 1ul << 32,
  kDirectionalLight = 1ul << 33,
  kPointLight = 1ul << 34,
  kRenderer = 1ul << 35,

  // Animation
  kVariable = 1ul << 36,
};

// Bitwise combination of ResourceTypes.  A subclass hierarchy can be
// represented: for each class, a bit is set for that class and all of its
// parent classes.
using ResourceTypeFlags = uint64_t;

// Static metadata about a Resource subclass.
struct ResourceTypeInfo {
  const ResourceTypeFlags flags;
  const char* name;

  // Return true if this type is or inherits from |base_type|, and false
  // otherwise.
  bool IsKindOf(const ResourceTypeInfo& base_type) const {
    return base_type.flags == (flags & base_type.flags);
  }

  bool operator==(const ResourceTypeInfo& type) const { return flags == type.flags; }
};

}  // namespace gfx
}  // namespace scenic_impl

#endif  // SRC_UI_SCENIC_LIB_GFX_RESOURCES_RESOURCE_TYPE_INFO_H_
