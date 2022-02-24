// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_LIB_COMPUTE_SPINEL_PLATFORMS_VK_ALLOCATOR_DEVICE_H_
#define SRC_GRAPHICS_LIB_COMPUTE_SPINEL_PLATFORMS_VK_ALLOCATOR_DEVICE_H_

//
// All device memory allocations are either long-lasting or
// short-lived and are made via the functions below.
//
// Once a Spinel instance is created, its *internal* allocations are
// short-lived and acquired from a suballocator.
//
// External-facing APIs like the path/raster builders and compositions
// acquire long-lived memory allocations.
//

#include <vulkan/vulkan_core.h>

//
//
//

#include "suballocator.h"

//
//
//

struct spn_vk_environment;

//
//
//

#define SPN_ALLOCATOR_DEVICE_PERM_MAX_QUEUE_FAMILY_INDICES 16

//
//
//

struct spn_allocator_device_perm
{
  uint32_t queue_family_indices[SPN_ALLOCATOR_DEVICE_PERM_MAX_QUEUE_FAMILY_INDICES];
  uint32_t queue_family_count;

  VkMemoryPropertyFlags mpf;
  VkBufferUsageFlags    buf;
};

//
//
//

struct spn_allocator_device_temp
{
  struct spn_allocator_host_perm *   host_perm;
  struct spn_allocator_device_perm * device_perm;

  VkDescriptorBufferInfo dbi;
  VkDeviceMemory         devmem;

  struct spn_suballocator suballocator;
};

//
// PERM / DURABLE
//

void
spn_allocator_device_perm_create(struct spn_allocator_device_perm * const device_perm,
                                 struct spn_vk_environment * const        environment,
                                 VkMemoryPropertyFlags const              mpf,
                                 VkBufferUsageFlags const                 buf,
                                 uint32_t const                           queue_family_count,
                                 uint32_t const                           queue_family_indices[]);

void
spn_allocator_device_perm_dispose(struct spn_allocator_device_perm * const device_perm,
                                  struct spn_vk_environment * const        environment);

void
spn_allocator_device_perm_alloc(struct spn_allocator_device_perm * const device_perm,
                                struct spn_vk_environment * const        environment,
                                VkDeviceSize const                       size,
                                VkDeviceSize * const                     alignment,
                                VkDescriptorBufferInfo * const           dbi,
                                VkDeviceMemory * const                   devmem);

void
spn_allocator_device_perm_free(struct spn_allocator_device_perm * const device_perm,
                               struct spn_vk_environment * const        environment,
                               VkDescriptorBufferInfo * const           dbi,
                               VkDeviceMemory                           devmem);

//
// TEMP / EPHEMERAL
//

void
spn_allocator_device_temp_create(struct spn_allocator_device_temp * const device_temp,
                                 struct spn_allocator_host_perm * const   host_perm,
                                 struct spn_allocator_device_perm * const device_perm,
                                 struct spn_vk_environment * const        environment,
                                 uint32_t const                           subbufs,
                                 VkDeviceSize const                       size);

void
spn_allocator_device_temp_dispose(struct spn_allocator_device_temp * const device_temp,
                                  struct spn_vk_environment * const        environment);

void
spn_allocator_device_temp_alloc(struct spn_allocator_device_temp * const device_temp,
                                struct spn_device * const                device,
                                spn_suballocator_wait_pfn                wait,
                                VkDeviceSize const                       size,
                                spn_subbuf_id_t * const                  subbuf_id,
                                VkDescriptorBufferInfo * const           dbi);

void
spn_allocator_device_temp_free(struct spn_allocator_device_temp * const device_temp,
                               spn_subbuf_id_t const                    subbuf_id);

//
//
//

#endif  // SRC_GRAPHICS_LIB_COMPUTE_SPINEL_PLATFORMS_VK_ALLOCATOR_DEVICE_H_
