// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//
//
//

#include "device.h"

#include <memory.h>
#include <stdlib.h>

#include "block_pool.h"
#include "common/vk/assert.h"
#include "composition_impl.h"
#include "context.h"
#include "dispatch.h"
#include "handle_pool.h"
#include "hotsort/platforms/vk/hotsort_vk.h"
#include "path_builder_impl.h"
#include "queue_pool.h"
#include "raster_builder_impl.h"
#include "render_impl.h"
#include "spinel_assert.h"
#include "status.h"
#include "styling_impl.h"
#include "vk.h"
#include "vk_target.h"

//
//
//

uint64_t
spn_device_get_timeout_ns(struct spn_device * const device)
{
  //
  // FIXME(allanmac): Eventually get rid of this but in the meantime
  // make this part of a target's config.
  //
  return 1000UL * 1000UL * 1000UL * 10UL;  // 10 seconds
}

//
//
//

void
spn_device_lost(struct spn_device * const device)
{
  //
  // FIXME(allanmac): Properly shutting down Spinel is WIP.
  //
  abort();
}

//
//
//

spn_result_t
spn_device_reset(struct spn_device * const device)
{
  return SPN_ERROR_NOT_IMPLEMENTED;
}

//
//
//

static spn_result_t
spn_device_create(struct spn_vk_environment * const               environment,
                  struct spn_vk_context_create_info const * const create_info,
                  struct spn_context * const                      context)
{
  struct spn_device * device = malloc(sizeof(*device));

  context->device = device;

  //
  // make a copy of the environment
  //
  device->environment = *environment;

  //
  // keep a back-pointer to the context
  //
  device->context = context;

  //
  // create the Spinel instance
  //
  device->instance = spn_vk_create(environment, create_info->spinel);

  //
  // create the associated HotSort instance
  //
  device->hs = hotsort_vk_create(environment->d,
                                 environment->ac,
                                 environment->pc,
                                 spn_vk_pl_hotsort(device->instance),
                                 create_info->hotsort);

  //
  // the target configuration guides early resource allocation
  //
  struct spn_vk_target_config const * const config = spn_vk_get_config(device->instance);

  //
  // perm host
  //
  spn_allocator_host_perm_create(&device->allocator.host.perm,
                                 config->allocator.host.perm.alignment);

  //
  // perm device read-write
  //
  spn_allocator_device_perm_create(&device->allocator.device.perm.drw,
                                   environment,
                                   config->allocator.device.drw.properties,
                                   config->allocator.device.drw.usage,
                                   0,
                                   NULL);

  //
  // perm host write / device read
  //
  spn_allocator_device_perm_create(&device->allocator.device.perm.hw_dr,
                                   environment,
                                   config->allocator.device.hw_dr.properties,
                                   config->allocator.device.hw_dr.usage,
                                   0,
                                   NULL);

  //
  // perm host read-write / device read
  //
  spn_allocator_device_perm_create(&device->allocator.device.perm.hrw_dr,
                                   environment,
                                   config->allocator.device.hrw_dr.properties,
                                   config->allocator.device.hrw_dr.usage,
                                   0,
                                   NULL);

  //
  // perm host read / device write
  //
  spn_allocator_device_perm_create(&device->allocator.device.perm.hr_dw,
                                   environment,
                                   config->allocator.device.hr_dw.properties,
                                   config->allocator.device.hr_dw.usage,
                                   0,
                                   NULL);

  //
  // temp device-local
  //
  spn_allocator_device_temp_create(&device->allocator.device.temp.drw,
                                   &device->allocator.host.perm,
                                   &device->allocator.device.perm.drw,
                                   environment,
                                   config->allocator.device.temp.subbufs,
                                   config->allocator.device.temp.size);

  //
  // FIXME(allanmac): verify performance - this is a placeholder implementation
  //
  spn_device_queue_pool_create(device, 1);

  spn_device_handle_pool_create(device, create_info->handle_count);

  spn_device_dispatch_create(device);

  spn_device_block_pool_create(device,
                               create_info->block_pool_size,
                               spn_device_handle_pool_get_handle_count(device));

  spn_device_status_create(device);

  return SPN_SUCCESS;
}

//
//
//

static spn_result_t
spn_device_dispose(struct spn_device * const device)
{
  //
  // FIXME(allanmac): do we want to use spn_device_lost()?
  //

  // drain all in-flight completions
  spn(device_wait_all(device, true, __func__));

  // shut down each major module in reverse order
  spn_device_status_dispose(device);
  spn_device_block_pool_dispose(device);
  spn_device_dispatch_dispose(device);
  spn_device_handle_pool_dispose(device);
  spn_device_queue_pool_dispose(device);

  spn_allocator_device_temp_dispose(&device->allocator.device.temp.drw, &device->environment);
  spn_allocator_device_perm_dispose(&device->allocator.device.perm.hw_dr, &device->environment);
  spn_allocator_device_perm_dispose(&device->allocator.device.perm.hr_dw, &device->environment);
  spn_allocator_device_perm_dispose(&device->allocator.device.perm.drw, &device->environment);

  spn_allocator_host_perm_dispose(&device->allocator.host.perm);

  hotsort_vk_release(device->environment.d, device->environment.ac, device->hs);

  spn_vk_dispose(device->instance, &device->environment);

  free(device->context);
  free(device);

  return SPN_SUCCESS;
}

//
//
//

spn_result_t
spn_vk_context_create(struct spn_vk_environment * const               environment,
                      struct spn_vk_context_create_info const * const create_info,
                      spn_context_t * const                           context)
{
  *context = malloc(sizeof(**context));

  (*context)->status         = spn_device_get_status;
  (*context)->dispose        = spn_device_dispose;
  (*context)->path_builder   = spn_path_builder_impl_create;
  (*context)->path_retain    = spn_device_handle_pool_validate_retain_h_paths;
  (*context)->path_release   = spn_device_handle_pool_validate_release_h_paths;
  (*context)->raster_builder = spn_raster_builder_impl_create;
  (*context)->raster_retain  = spn_device_handle_pool_validate_retain_h_rasters;
  (*context)->raster_release = spn_device_handle_pool_validate_release_h_rasters;
  (*context)->composition    = spn_composition_impl_create;
  (*context)->styling        = spn_styling_impl_create;
  (*context)->render         = spn_render_impl;

  return spn_device_create(environment, create_info, *context);
}

//
//
//
