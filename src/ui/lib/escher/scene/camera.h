// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_LIB_ESCHER_SCENE_CAMERA_H_
#define SRC_UI_LIB_ESCHER_SCENE_CAMERA_H_

#include "src/ui/lib/escher/forward_declarations.h"
#include "src/ui/lib/escher/geometry/types.h"
#include "src/ui/lib/escher/hmd/pose_buffer.h"

#include <vulkan/vulkan.hpp>

namespace escher {

// Used to indicate which eye a camera represents, in order to distinguish them
// for stereo rendering.
enum class CameraEye {
  kLeft,
  kRight,
};

// Generates and encapsulates a view/projection matrix pair.  The camera follows
// the Vulkan convention of looking down the negative Z-axis.
class Camera {
 public:
  Camera(const mat4& transform, const mat4& projection);

  // Create an camera in the default position for a full-screen orthographic
  // projection.
  static Camera NewOrtho(const ViewingVolume& volume, const mat4* clip_space_transform = nullptr);

  // Create an orthographic camera looking at the viewing volume in the
  // specified direction.
  static Camera NewForDirectionalShadowMap(const ViewingVolume& volume, const glm::vec3& direction);

  // Create a camera with a perspective projection.
  static Camera NewPerspective(const ViewingVolume& volume, const mat4& transform, float fovy,
                               const mat4* clip_space_transform = nullptr);

  const mat4& transform() const { return transform_; }
  const mat4& projection() const { return projection_; }

  void SetLatchedPoseBuffer(const BufferPtr& latched_pose_buffer, CameraEye eye) {
    latched_pose_buffer_ = latched_pose_buffer;
    latched_camera_eye_ = eye;
  }
  const BufferPtr& latched_pose_buffer() const { return latched_pose_buffer_; }
  CameraEye latched_camera_eye() const { return latched_camera_eye_; }

  // This viewport class is independent of framebuffer size.
  // All values are specified over the range [0,1].
  // The class default constructs a viewport over the entire framebuffer.
  struct Viewport {
    float x = 0;
    float y = 0;
    float width = 1;
    float height = 1;

    // Given the framebuffer size, return the corresponding vk::Rect2D
    vk::Rect2D vk_rect_2d(uint32_t fb_width, uint32_t fb_height) const;
  };

  void SetViewport(const Viewport& viewport) { viewport_ = viewport; }
  const Viewport& viewport() const { return viewport_; }

 private:
  mat4 transform_;
  mat4 projection_;

  // Contains the latched pose and vp matrices latched out of pose_buffer_.
  // See pose_buffer_latching_shader.h for details on buffer layout.
  BufferPtr latched_pose_buffer_;
  CameraEye latched_camera_eye_ = CameraEye::kLeft;

  Viewport viewport_;
};

// Debugging.
ESCHER_DEBUG_PRINTABLE(Camera);

}  // namespace escher

#endif  // SRC_UI_LIB_ESCHER_SCENE_CAMERA_H_
