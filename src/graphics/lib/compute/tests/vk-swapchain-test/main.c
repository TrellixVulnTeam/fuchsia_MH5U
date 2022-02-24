// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdlib.h>

#include "tests/common/vk_app_state.h"
#include "tests/common/vk_surface.h"
#include "tests/common/vk_swapchain.h"

//
// A tiny test to check that vk_app_state_t create/destruction works properly
// with swapchain support enabled. However, not presentation will be performed.
//

int
main(int argc, char const * argv[])
{
  vk_app_state_config_t app_config = {
    .app_name              = "vk_swapchain_test",
    .enable_validation     = true,
    .enable_debug_report   = true,
    .enable_amd_statistics = true,

    .device_config = {
      .required_queues = VK_QUEUE_GRAPHICS_BIT,
      .vendor_id       = (argc <= 2) ? 0 : (uint32_t)strtoul(argv[1], NULL, 16),
      .device_id       = (argc <= 3) ? 0 : (uint32_t)strtoul(argv[2], NULL, 16),
    },

    .require_swapchain      = true,
  };

  vk_app_state_t app_state = {};

  if (!vk_app_state_init(&app_state, &app_config))
    {
      fprintf(stderr, "FAILURE\n");
      return EXIT_FAILURE;
    }

  vk_app_state_print(&app_state);

  const vk_surface_config_t surface_config = {
    .instance           = app_state.instance,
    .physical_device    = app_state.pd,
    .allocator          = app_state.ac,
    .queue_family_index = app_state.qfi,
    .window_width       = 800,
    .window_height      = 600,
    .window_title       = "vk_swapchain_test",
  };
  vk_surface_t * surface = vk_surface_create(&surface_config);

  if (!surface)
    {
      vk_app_state_destroy(&app_state);
      return EXIT_FAILURE;
    }

  const vk_swapchain_config_t swapchain_config = {
    .instance        = app_state.instance,
    .device          = app_state.d,
    .physical_device = app_state.pd,
    .allocator       = app_state.ac,

    .present_queue_family = app_state.qfi,
    .present_queue_index  = 0,

    .surface_khr = vk_surface_get_surface_khr(surface),
    .max_frames  = 2,
  };

  vk_swapchain_t * swapchain = vk_swapchain_create(&swapchain_config);

  vk_swapchain_print(swapchain);

  vk_swapchain_destroy(swapchain);
  vk_surface_destroy(surface);
  vk_app_state_destroy(&app_state);

  return EXIT_SUCCESS;
}

//
//
//
