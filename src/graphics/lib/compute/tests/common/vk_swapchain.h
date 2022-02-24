// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_LIB_COMPUTE_TESTS_COMMON_VK_SWAPCHAIN_H_
#define SRC_GRAPHICS_LIB_COMPUTE_TESTS_COMMON_VK_SWAPCHAIN_H_

//
//
//

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

//
//
//

#ifdef __cplusplus
extern "C" {
#endif

//
//
//

// Swapchain-related data. Usage is the following:
//
// 1) Create a VkSurfaceKHR then call vk_swapchain_create(...) with appropriate
//    configuration arguments passed as a vk_swapchain_config_t pointer.
//
// 2) Retrieve swapchain surface information with one of the individual getter
//    functions (e.g. vk_swapchain_get_image_count(), vk_swapchain_get_image_view(), etc).
//
// 3) To render to a swapchain, the hard way, do the following:
//
//     - Call vk_swapchain_acquire_next_image() to acquire the index
//       of the new swapchain image.
//
//     - Perform one or more queue submits to render something to
//       the image (using vk_swapchain_get_image() and
//       vk_swapchain_get_image_view() to retrieve handles to the
//       coresponding VkImage and VkImageView, respectively).
//
//       NOTE: The first submit *must* wait on
//       vk_swapchain_get_image_acquired_semaphore(), because the
//       image might not be ready for access yet after
//       vk_swapchain_acquire_next_image() returns.
//
//       NOTE2: The last submit *must* signal
//       vk_swapchain_get_image_rendered_semaphore(), because it
//       is waited on to present the image.
//
//     - Call vk_swapchain_present_image() to send the content of
//       the current swapchain image for presentation. Note that this
//       will always wait on vk_swapchain_get_image_rendered_semaphore()
//
//    Usage example:
//
//        // Acquire next swapchain image
//        uint32_t image_index;
//        if (!vk_swapchain_acquire_next_image(swapchain, &image_index)) {
//           // exit rendering loop.
//        }
//
//        // Begin one or more command buffers, fill them with commands.
//
//        // Submit the command buffer(s), waiting and signalling the
//        // right semaphores.
//
//        VkSemaphore waitSemaphore =
//            vk_swapchain_get_image_acquired_semaphore(swapchain);
//
//        VkSemaphore signalSemaphore =
//            vk_swapchain_get_image_rendered_semaphore(swapchain);
//
//        const VkSubmitInfo submitInfo = {
//          .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
//          ...
//          .waitSemaphoreCount = 1,
//          .pWaitSemaphores = &waitSemaphore,
//          .pWaitDstStageMask = ...,
//          ...
//          .signalSemaphoreCount = 1,
//          .pSignalSemaphores = &signalSemaphore,
//        };
//        vkQueueSubmit(queue, 1, &submitInfo, ...);
//
//        // Send rendered image to presentation.
//        vk_swapchain_present_image(swapchain);
//
//
// 4) Convenience functions are provided in "vk_swapchain_queue.h" to make
//    this easier for simple applications. See documentation comments there.
//
// Also see the vk_triangle_test and vk_transfer_test examples for examples.
//

typedef struct vk_swapchain vk_swapchain_t;

// Swapchain staging means providing an intermediate target image to the client
// instead of the real swapchain image, and ensuring that its content is
// blitted or copied to the swapchain when vk_swapchain_present_image() is
// called.
//
// This is useful in the following cases:
//
//  - The client wants a pixel format that is not supported by the
//    swapchain implementation. E.g. the clients wants R8G8B8A but the
//    swapchain only supports B8G8R8A8.
//
// - The client wants an image usage that is not supported by the swapchain
//   implementation. E.g. Intel GPUs do not support the
//   VK_IMAGE_USAGE_STORAGE_BIT usage flag, which means shaders cannot write
//   directly to swapchain images.
//
// Staging requires a full blit/transfer per swapchain present event, and
// thus can be expensive, but it is a useful fallback when no other solution
// works.
//
// Valid values are:
//
// NONE: Never enable swapchain staging (the default). Asking for an unsupported
// (pixel_format, image_usage) combination will simply fail.
//
// IF_NEEDED: Enable swapchain staging if needed, as detected by
// vk_swapchain_create() based on configuration values and the surface's
// presentation formats and feature flags.
//
// FORCED: Force swapchain staging, even if it is not needed. This is only
// useful for debugging and unit-testing.
//
// NOTE: Not all (pixel_format, image_usage) combinations are supported. For
// now, the following are guaranteed to work:
//
//   - FORCED staging, where the intermediate and swapchain images share
//     the same format, and image usage.
//
//   - IF_NEEDED, when the client asks for VK_FORMAT_B8G8R8A8_UNORM with a
//     VK_IMAGE_USAGE_STORAGE_BIT flag that is not supported by the swapchain.
//     In this case, the target image will use VK_FORMAT_R8G8B8A8_UNORM
//     format internally, but its image view will swap the R and B channels,
//     to its content can be directly transferred to the swapchain image
//     without pixel format conversion.
//
//   - IF_NEEDED, when the client asks for VK_FORMAT_A8R8G8B8_UNORM but the
//     swapchain only supports VK_FORMAT_B8G8R8A8_UNORM. The same technique
//     as above is used to swap the R and B channels of the target image in
//     its image view.
//
typedef enum
{
  VK_SWAPCHAIN_STAGING_MODE_NONE = 0,
  VK_SWAPCHAIN_STAGING_MODE_IF_NEEDED,
  VK_SWAPCHAIN_STAGING_MODE_FORCED,
} vk_swapchain_staging_mode_t;

typedef struct
{
  // The Vulkan instance, device, physical device, device and allocator to use.
  VkInstance                    instance;
  VkDevice                      device;
  VkPhysicalDevice              physical_device;
  const VkAllocationCallbacks * allocator;

  // Queue family and index to be used for presentation.
  uint32_t present_queue_family;
  uint32_t present_queue_index;

  // The target presentation surface to use and its extent.
  VkSurfaceKHR surface_khr;

  // Maximum number of inflight frames to send to the swapchain.
  // This should be at least 1, and will be capped by the max number of
  // swapchain images supported by the surface / presentation engine.
  // Use 2 for double-buffering, and 3 for triple-buffering.
  uint32_t max_frames;

  // Favorite surface pixel format. If not 0, the swapchain will try to
  // use this when creating the swapchain images. Check the results by
  // looking at |surface_format.format| after swapchain creation.
  VkFormat pixel_format;

  // Set to true to disable synchronization to the vertical blanking period.
  // Will result in tearing, but useful for benchmarking.
  bool disable_vsync;

  // If not 0, this is taken as the required image usage bits for the
  // swapchain creation. Default will be VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT.
  VkImageUsageFlags image_usage_flags;

  // Swapchain staging mode, default is to disable it.
  vk_swapchain_staging_mode_t staging_mode;

  // TODO(digit): Provide a way to suggest a favorite presentation mode.
  // TODO(digit): Provide a way to provide an old swapchain to support resizes.

} vk_swapchain_config_t;

// Create a new vk_swapchain_t instance. On success, returns a pointer
// to the new instance. On failure, aborts with an error message on stderr
// explaining the issue.
extern vk_swapchain_t *
vk_swapchain_create(const vk_swapchain_config_t * config);

// Print swapchain details to stdout. Useful for debugging.
extern void
vk_swapchain_print(const vk_swapchain_t * swapchain);

// Destroy a given swapchain instance. This will also destroy any optional
// command buffers and framebuffers enabled with vk_swapchain_enable_image_xxx().
extern void
vk_swapchain_destroy(vk_swapchain_t * swapchain);

// Retrieve swapchain surface extent.
extern VkExtent2D
vk_swapchain_get_extent(const vk_swapchain_t * swapchain);

// Retrieve swapchain surface format and color space.
extern VkSurfaceFormatKHR
vk_swapchain_get_format(const vk_swapchain_t * swapchain);

// Retrieve number of swapchain images.
extern uint32_t
vk_swapchain_get_image_count(const vk_swapchain_t * swapchain);

// Retrieve number of sync frames (will be <= the image count).
extern uint32_t
vk_swapchain_get_frame_count(const vk_swapchain_t * swapchain);

// Retrieve the VkSwapchainKHR value used by this vk_swapchain_t instance.
// Should only be useful for debugging.
extern VkSwapchainKHR
vk_swapchain_get_swapchain_khr(const vk_swapchain_t * swapchain);

// Retrieve VkImage associated with swapchain image at |image_index|.
// Requires |image_index < image_count|.
extern VkImage
vk_swapchain_get_image(const vk_swapchain_t * swapchain, uint32_t image_index);

// Retrieve VkImageView associated with swapchain image at |image_index|.
// Requires |image_index < image_count|.
extern VkImageView
vk_swapchain_get_image_view(const vk_swapchain_t * swapchain, uint32_t image_index);

// NOTE: For simpler cases, consider using vk_swapchain_prepare_next_image() and
// vk_swapchain_submit_and_present_image() instead.
//
// Acquire the next swapchain image. On failure, i.e. if the display surface was
// resized or invalidated, return false. Otherwise, return true and sets
// |*p_image_index| to the swapchain image index. The latter can also be
// retrieved as vk_swapchain_get_image_index().
//
// IMPORTANT: The caller should then queue one or more submits, but the first
// one must wait on the vk_swapchain_get_image_acquired() semaphore, and the
// last one must signal the vk_swapchain_get_image_rendered() semaphore.
extern bool
vk_swapchain_acquire_next_image(vk_swapchain_t * swapchain, uint32_t * p_image_index);

// Return the current swapchain image index. The one returned when calling
// vk_swapchain_acquire_next_image().
extern uint32_t
vk_swapchain_get_image_index(const vk_swapchain_t * swapchain);

// Return the semaphore used to wait for the current swapchain image acquisition.
// This is the semaphore that vk_swapchain_submit_and_present_image() will use for
// waiting, or that any queue submit performed after
// vk_swapchain_acquire_next_image() should wait on.
extern VkSemaphore
vk_swapchain_get_image_acquired_semaphore(const vk_swapchain_t * swapchain);

// Return the semaphore used to signal rendering completion for the current
// swapchain image. This is the semaphore that is waited on by
// vk_swapchain_present_image(), and which is also signaled internally by
// vk_swapchain_submit_and_present_image().
extern VkSemaphore
vk_swapchain_get_image_rendered_semaphore(const vk_swapchain_t * swapchain);

// Return the semaphore used to wait for the current swapchain image acquisition
// and transfer ownership to the caller. The next call to
// vk_swapchain_acquire_next_image() will create a new semaphore on demand.
// This is necessary because certain libraries, like Skia, insist on owning
// the semaphores they wait on.
extern VkSemaphore
vk_swapchain_take_image_acquired_semaphore(vk_swapchain_t * swapchain);

// Same as above for the semaphore returned by vk_swapchain_get_image_rendered_semaphore().
extern VkSemaphore
vk_swapchain_take_image_rendered_semaphore(vk_swapchain_t * swapchain);

// Present the current swapchain image after waiting for
// vk_swapchain_get_image_rendered_semaphore(), which should have been
// signaled by a previous submit performed by the caller.
extern bool
vk_swapchain_present_image(vk_swapchain_t * swapchain);

//
//
//

#ifdef __cplusplus
}
#endif

//
//
//

#endif  // SRC_GRAPHICS_LIB_COMPUTE_TESTS_COMMON_VK_SWAPCHAIN_H_
