// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dispatch.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common/macros.h"
#include "common/vk/assert.h"
#include "common/vk/debug_utils.h"
#include "context.h"
#include "device.h"
#include "queue_pool.h"

//
// NOTE: dispatch.c is reentrant but single-threaded (for now)
//

//
// FOR DEBUG ONLY
//
// Track outstanding number of dispatches that are waiting on prior
// dispatches to complete.
//

#ifndef NDEBUG
#define SPN_DISPATCH_TRACK_WAITING
#endif

#define SPN_DISPATCH_TRACK_STAGE

//
// NOTE:
//
// It's unlikely we'll want to support more than 255 outstanding
// dispatch ids unless we're running on an extremely large GPU.
//
// Note that 255 in-flight or waiting dispatches represents a very large
// amount of processing.
//
// Increasing this limit to either a total of 1024 dispatches or up to
// 1024 per stage would be extreme.
//
// There are two angles of attack here:
//
//   1. Implement a per stage id pool and hide a tag in the dispatch id.
//
//   2. Increase the size of the stage_id type to 16 bits.
//
// One implication of a larger spn_dispatch_stage_id_t is that we store
// one per handle in handle pool.  This is a strong reason to choose
// option (1).
//

// clang-format off
#define SPN_DISPATCH_ID_BITS   (8 * sizeof(spn_dispatch_id_t))
#define SPN_DISPATCH_ID_COUNT  BITS_TO_MASK_MACRO(SPN_DISPATCH_STAGE_ID_BITS)
// clang-format on

//
// The dispatch_stage_id_t may expand to a larger type and include a tag.
//

typedef uint8_t spn_dispatch_stage_id_t;

// clang-format off
#define SPN_DISPATCH_STAGE_ID_BITS     (8 * sizeof(spn_dispatch_stage_id_t))
#define SPN_DISPATCH_STAGE_ID_INVALID  BITS_TO_MASK_MACRO(SPN_DISPATCH_STAGE_ID_BITS)
#define SPN_DISPATCH_STAGE_ID_COUNT    BITS_TO_MASK_MACRO(SPN_DISPATCH_STAGE_ID_BITS)
// clang-format on

//
// Type determined by max number of dispatches that can be waited upon.
//
// This may vary by stage in a future implementation.
//

typedef spn_dispatch_id_t spn_dispatch_stage_wait_count_t;  // same size for now

//
// The completion payload size limit is currently 48 bytes.
//
// Lower this if the submission callback payloads shrink further.
//

// clang-format off
#define SPN_DISPATCH_COMPLETION_PAYLOAD_QWORDS  6
#define SPN_DISPATCH_COMPLETION_PAYLOAD_SIZE    MEMBER_SIZE_MACRO(struct spn_dispatch_completion, payload)
// clang-format on

//
// NOTE: We're forever limiting the signalling bitmap to a massive 1024
// dispatch ids per stage.
//
// If the stage id is  8 bits, spn_dispatch_signal is  9 dwords (36 bytes).
// If the stage id is 10 bits, spn_dispatch_signal is 33 dwords (132 bytes).
//

// clang-format off
#define SPN_DISPATCH_SIGNAL_BITMAP_DWORDS MIN_MACRO(uint32_t, 32, ((1 << SPN_DISPATCH_STAGE_ID_BITS) / 32))
#define SPN_DISPATCH_SIGNAL_BITMAP_SIZE   MEMBER_SIZE_MACRO(struct spn_dispatch_signal, bitmap)
// clang-format on

struct spn_dispatch_signal
{
  uint32_t index;
  uint32_t bitmap[SPN_DISPATCH_SIGNAL_BITMAP_DWORDS];
};

//
// The arg is an spn_[path|raster]_builder_impl pointer.
//

struct spn_dispatch_flush
{
  void * arg;
};

//
// When a dispatch completes, it may invoke a completion routine to
// reclaim resources and/or dispatch more work.
//

struct spn_dispatch_completion
{
  spn_dispatch_completion_pfn_t pfn;
  uint64_t                      payload[SPN_DISPATCH_COMPLETION_PAYLOAD_QWORDS];
};

//
//
//

struct spn_dispatch_submitter
{
  spn_dispatch_submitter_pfn_t pfn;
  void *                       data;
};

//
//
//

#ifdef SPN_DISPATCH_TRACK_STAGE

struct spn_dispatch_track
{
  // map of ids to stages
  spn_dispatch_stage_e stage[SPN_DISPATCH_STAGE_ID_COUNT];

  // stage counts
  union
  {
    struct
    {
#undef SPN_DISPATCH_STAGE
#define SPN_DISPATCH_STAGE(e_) uint16_t e_;

      SPN_DISPATCH_STAGES()
    } named;

#undef SPN_DISPATCH_STAGE
#define SPN_DISPATCH_STAGE(e_) +1

    uint16_t array[0 SPN_DISPATCH_STAGES()];
  } count;
};

static void
spn_dispatch_track_reset(struct spn_dispatch_track * const track)
{
  memset(track, 0, sizeof(*track));
}

static void
spn_dispatch_track_begin(struct spn_dispatch_track * const track,
                         spn_dispatch_stage_e const        stage,
                         spn_dispatch_id_t const           id)
{
  track->stage[id] = stage;

  track->count.array[stage] += 1;
}

static void
spn_dispatch_track_end(struct spn_dispatch_track * const track, spn_dispatch_id_t const id)
{
  spn_dispatch_stage_e stage = track->stage[id];

  track->count.array[stage] -= 1;
}

static void
spn_dispatch_track_dump_count(struct spn_dispatch_track const * const track)
{
  fprintf(stderr,
          "{\n"
#undef SPN_DISPATCH_STAGE
#define SPN_DISPATCH_STAGE(e_) "\t" #e_ ": %u\n"
          SPN_DISPATCH_STAGES() "}\n"

#undef SPN_DISPATCH_STAGE
#define SPN_DISPATCH_STAGE(e_) , track->count.named.e_
          SPN_DISPATCH_STAGES());
}

#endif

//
//
//

struct spn_dispatch
{
  VkCommandPool cp;

  VkCommandBuffer                 cbs[SPN_DISPATCH_ID_COUNT];
  VkFence                         fences[SPN_DISPATCH_ID_COUNT];
  struct spn_dispatch_signal      signals[SPN_DISPATCH_ID_COUNT];
  struct spn_dispatch_flush       flushes[SPN_DISPATCH_ID_COUNT];
  struct spn_dispatch_submitter   submitters[SPN_DISPATCH_ID_COUNT];
  struct spn_dispatch_completion  completions[SPN_DISPATCH_ID_COUNT];
  spn_dispatch_stage_wait_count_t wait_counts[SPN_DISPATCH_ID_COUNT];

  struct spn_dispatch_id_count
  {
    uint32_t available;
    uint32_t executing;
    uint32_t complete;

#ifdef SPN_DISPATCH_TRACK_WAITING
    uint32_t waiting;  // NOTE(allanmac): debug only
#endif
  } counts;

  struct
  {
    spn_dispatch_stage_id_t available[SPN_DISPATCH_STAGE_ID_COUNT];
    spn_dispatch_id_t       executing[SPN_DISPATCH_ID_COUNT];
    spn_dispatch_id_t       complete[SPN_DISPATCH_ID_COUNT];

#ifdef SPN_DISPATCH_TRACK_STAGE
    struct spn_dispatch_track track;
#endif
  } indices;

  // a large array that maps handle ids to dispatch stage ids
  spn_dispatch_stage_id_t * handle_stage_ids;
};

//
//
//

#ifdef SPN_DISPATCH_TRACK_STAGE

static void
spn_dispatch_track_dump(struct spn_dispatch const * const dispatch,
                        uint64_t const                    timeout_ns,
                        VkResult const                    result)
{
  if (timeout_ns > 0ul)
    {
      static char const * const stage_names[] = {

#undef SPN_DISPATCH_STAGE
#define SPN_DISPATCH_STAGE(e_) #e_,

        SPN_DISPATCH_STAGES()
      };

      fprintf(stderr,
              "dispatch->counts.executing: %u --> %s\n",
              dispatch->counts.executing,
              vk_get_result_string(result));

      for (uint32_t ii = 0; ii < dispatch->counts.executing; ii++)
        {
          spn_dispatch_id_t    id    = dispatch->indices.executing[ii];
          spn_dispatch_stage_e stage = dispatch->indices.track.stage[id];

          fprintf(stderr,
                  "dispatch->indices.stage[ %3u ] = ( %2u ) : %s\n",
                  id,
                  stage,
                  stage_names[stage]);
        }

      spn_dispatch_track_dump_count(&dispatch->indices.track);
    }
}

#endif

//
//
//

static void
spn_debug_utils_cmd_begin_stage(VkCommandBuffer cb, spn_dispatch_stage_e const stage)
{
  if (pfn_vkCmdBeginDebugUtilsLabelEXT != NULL)
    {
      static char const * const stage_names[] = {

#undef SPN_DISPATCH_STAGE
#define SPN_DISPATCH_STAGE(e_) #e_,

        SPN_DISPATCH_STAGES()
      };

      VkDebugUtilsLabelEXT const label = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        NULL,
        stage_names[stage],
        { 1.0f, 0.0f, 0.0f, 1.0f },
      };

      pfn_vkCmdBeginDebugUtilsLabelEXT(cb, &label);
    }
}

void
spn_debug_utils_cmd_begin(VkCommandBuffer cb, char const * const label_name)
{
  if (pfn_vkCmdBeginDebugUtilsLabelEXT != NULL)
    {
      VkDebugUtilsLabelEXT const label = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        NULL,
        label_name,
        { 0.0f, 1.0f, 0.0f, 1.0f },
      };

      pfn_vkCmdBeginDebugUtilsLabelEXT(cb, &label);
    }
}

void
spn_debug_utils_cmd_end(VkCommandBuffer cb)
{
  if (pfn_vkCmdEndDebugUtilsLabelEXT != NULL)
    {
      pfn_vkCmdEndDebugUtilsLabelEXT(cb);
    }
}

//
//
//

void
spn_device_dispatch_create(struct spn_device * const device)
{
  //
  // allocate
  //
  struct spn_dispatch * const dispatch = spn_allocator_host_perm_alloc(&device->allocator.host.perm,
                                                                       SPN_MEM_FLAGS_READ_WRITE,
                                                                       sizeof(*dispatch));
  //
  // hang it off the device
  //
  device->dispatch = dispatch;

  //
  // create command pool
  //
  VkCommandPoolCreateInfo const cpci = {
    //
    // FIXME(allanmac): I don't think we are actually TRANSIENT so I'm
    // not indicating so with a flag.  The command buffers can be held
    // for a while before being submitted.
    //
    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .pNext            = NULL,
    .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    .queueFamilyIndex = device->environment.qfi
  };

  vk(CreateCommandPool(device->environment.d, &cpci, device->environment.ac, &dispatch->cp));

  //
  // create command buffers
  //
  VkCommandBufferAllocateInfo const cbai = {

    .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .pNext              = NULL,
    .commandPool        = dispatch->cp,
    .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = SPN_DISPATCH_ID_COUNT
  };

  vk(AllocateCommandBuffers(device->environment.d, &cbai, dispatch->cbs));

  //
  // create fences
  //
  VkFenceCreateInfo const fci = {

    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    .pNext = NULL,
    .flags = 0
  };

  for (uint32_t ii = 0; ii < SPN_DISPATCH_ID_COUNT; ii++)
    {
      vk(CreateFence(device->environment.d, &fci, device->environment.ac, dispatch->fences + ii));
    }

  //
  // allocate and initialize handle stage ids
  //
  uint32_t const handle_count          = spn_device_handle_pool_get_handle_count(device);
  size_t const   handle_stage_ids_size = sizeof(*dispatch->handle_stage_ids) * handle_count;

  dispatch->handle_stage_ids = spn_allocator_host_perm_alloc(&device->allocator.host.perm,
                                                             SPN_MEM_FLAGS_READ_WRITE,
                                                             handle_stage_ids_size);

  memset(dispatch->handle_stage_ids, SPN_DISPATCH_STAGE_ID_INVALID, handle_stage_ids_size);

  //
  // initialize counts and indices
  //
  dispatch->counts.available = SPN_DISPATCH_STAGE_ID_COUNT;
  dispatch->counts.executing = 0;
  dispatch->counts.complete  = 0;

#ifdef SPN_DISPATCH_TRACK_WAITING
  dispatch->counts.waiting = 0;
#endif

#ifdef SPN_DISPATCH_TRACK_STAGE
  spn_dispatch_track_reset(&dispatch->indices.track);
#endif

  for (uint32_t ii = 0; ii < SPN_DISPATCH_STAGE_ID_COUNT; ii++)
    {
      dispatch->indices.available[ii] = ii;
    }
}

//
//
//

void
spn_device_dispatch_dispose(struct spn_device * const device)
{
  struct spn_dispatch * const dispatch = device->dispatch;

  //
  // FIXME -- interrupt and free VkFences
  //

  // destroy fences
  for (uint32_t ii = 0; ii < SPN_DISPATCH_ID_COUNT; ii++)
    {
      vkDestroyFence(device->environment.d, dispatch->fences[ii], device->environment.ac);
    }

  // free command buffers
  vkFreeCommandBuffers(device->environment.d, dispatch->cp, SPN_DISPATCH_ID_COUNT, dispatch->cbs);

  // destroy command pool
  vkDestroyCommandPool(device->environment.d, dispatch->cp, device->environment.ac);

  // free handle stage ids
  spn_allocator_host_perm_free(&device->allocator.host.perm, dispatch->handle_stage_ids);

  // free dispatch
  spn_allocator_host_perm_free(&device->allocator.host.perm, dispatch);
}

//
//
//

static void
spn_device_dispatch_submitter_default(VkQueue               queue,
                                      VkFence               fence,
                                      VkCommandBuffer const cb,
                                      void *                data)
{
  struct VkSubmitInfo const si = { .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                   .commandBufferCount = 1,
                                   .pCommandBuffers    = &cb };

  vk(QueueSubmit(queue, 1, &si, fence));
}

//
//
//

static void
spn_device_dispatch_signal_waiters_dword(struct spn_device * const   device,
                                         struct spn_dispatch * const dispatch,
                                         uint32_t const              bitmap_base,
                                         uint32_t                    bitmap_dword)
{
  do
    {
      uint32_t const lsb_plus_1 = __builtin_ffs(bitmap_dword);
      uint32_t const lsb        = lsb_plus_1 - 1;
      uint32_t const mask       = 1u << lsb;

      // mask off lsb
      bitmap_dword &= ~mask;

      // which dispatch id?
      spn_dispatch_id_t const id = bitmap_base + lsb;

      // submit command buffer?
      spn_dispatch_stage_wait_count_t const wait_count = --dispatch->wait_counts[id];

      if (wait_count == 0)
        {
#ifdef SPN_DISPATCH_TRACK_WAITING
          // decrement the waiting count
          dispatch->counts.waiting -= 1;
#endif
          // push to executing -- coerce to possibly narrower integer type
          dispatch->indices.executing[dispatch->counts.executing++] = id;

          // submit!
          dispatch->submitters[id].pfn(spn_device_queue_next(device),
                                       dispatch->fences[id],
                                       dispatch->cbs[id],
                                       dispatch->submitters[id].data);
        }
    }
  while (bitmap_dword != 0);
}

static void
spn_device_dispatch_signal_waiters(struct spn_device * const                device,
                                   struct spn_dispatch * const              dispatch,
                                   struct spn_dispatch_signal const * const signal)
{
  //
  // for all dispatch ids in the bitmap
  //   - decrement the count of the bit dispatch
  //   - if zero then add to the executing list and submit
  //
  uint32_t index = signal->index;

  do
    {
      // which bit is lit?
      uint32_t const lsb_plus_1 = __builtin_ffs(index);
      uint32_t const lsb        = lsb_plus_1 - 1;
      uint32_t const mask       = 1u << lsb;

      // mask off lsb
      index &= ~mask;

      // process one dword of the bitmap
      spn_device_dispatch_signal_waiters_dword(device, dispatch, lsb * 32, signal->bitmap[lsb]);
    }
  while (index != 0);
}

//
//
//

static void
spn_device_dispatch_process_complete(struct spn_device * const   device,
                                     struct spn_dispatch * const dispatch)
{
  //
  // it's possible that execution of a pfn will update this count
  //
  while (dispatch->counts.complete > 0)
    {
      spn_dispatch_id_t const id = dispatch->indices.complete[--dispatch->counts.complete];

      // is there a pfn?
      struct spn_dispatch_completion * const completion = dispatch->completions + id;
      bool const                             is_pfn     = (completion->pfn != NULL);

      // are there dispatches waiting for a signal?
      struct spn_dispatch_signal const * const dispatch_signal = dispatch->signals + id;
      bool const                               is_signalling   = (dispatch_signal->index != 0);

      // NOTE: we make the dispatch available *before* invoking the callback
      dispatch->indices.available[dispatch->counts.available++] = id;

#ifdef SPN_DISPATCH_TRACK_STAGE
      spn_dispatch_track_end(&dispatch->indices.track, id);
#endif

      // invoke completion
      if (is_pfn && is_signalling)
        {
          // save the pfn payload -- ~48 bytes
          uint64_t payload[SPN_DISPATCH_COMPLETION_PAYLOAD_QWORDS];

          memcpy(payload, completion->payload, SPN_DISPATCH_COMPLETION_PAYLOAD_SIZE);

          // save the signals -- ~36 bytes
          struct spn_dispatch_signal const dispatch_signal_copy = *dispatch_signal;

          // invoke pfn
          completion->pfn(payload);

          // signal waiters
          spn_device_dispatch_signal_waiters(device, dispatch, &dispatch_signal_copy);
        }
      else if (is_pfn)
        {
          // save the pfn payload -- ~48 bytes
          uint64_t payload[SPN_DISPATCH_COMPLETION_PAYLOAD_QWORDS];

          memcpy(payload, completion->payload, SPN_DISPATCH_COMPLETION_PAYLOAD_SIZE);

          // invoke pfn
          completion->pfn(payload);
        }
      else if (is_signalling)
        {
          // save the signals -- ~36 bytes
          struct spn_dispatch_signal const dispatch_signal_copy = *dispatch_signal;

          // signal waiters
          spn_device_dispatch_signal_waiters(device, dispatch, &dispatch_signal_copy);
        }
    }
}

//
//
//

static spn_result_t
spn_device_dispatch_process_executing(struct spn_device * const   device,
                                      struct spn_dispatch * const dispatch,
                                      uint32_t const              imports_count,
                                      VkFence * const             imports,
                                      bool const                  wait_all,
                                      uint64_t const              timeout_ns)
{
  uint32_t const count_executing = dispatch->counts.executing;
  uint32_t const fences_count    = count_executing + imports_count;

  if (fences_count == 0)
    {
      return SPN_SUCCESS;
    }

  //
  // VkWaitForFences() requires a linear array of VkFences
  //
  // The executing fences are followed by the imports.
  //
  // For now, this uses a VLA.
  //
  VkFence fences[fences_count];

  for (uint32_t ii = 0; ii < count_executing; ii++)
    {
      fences[ii] = dispatch->fences[dispatch->indices.executing[ii]];
    }

  //
  // skip if no imports
  //
  if (imports_count > 0)
    {
      memcpy(fences + count_executing, imports, sizeof(*imports) * imports_count);
    }

  //
  // wait for signalled or timeout
  //
  VkResult result = vkWaitForFences(device->environment.d,  //
                                    fences_count,
                                    fences,
                                    wait_all,
                                    timeout_ns);

  switch (result)
    {
      case VK_SUCCESS:
        break;

      case VK_TIMEOUT:
#ifndef NDEBUG
        if (timeout_ns > 0ul)
          {
            fprintf(stderr,
                    "VK_TIMEOUT counts:\n"
                    "  available: %u\n"
                    "  executing: %u\n"
                    "  complete : %u\n"
#ifdef SPN_DISPATCH_TRACK_WAITING
                    "  waiting  : %u\n"
#endif
                    ,
                    dispatch->counts.available,
                    dispatch->counts.executing,
                    dispatch->counts.complete,
#ifdef SPN_DISPATCH_TRACK_WAITING
                    dispatch->counts.waiting
#endif
            );
          }
#endif
        spn_dispatch_track_dump(dispatch, timeout_ns, result);
        return SPN_TIMEOUT;

      case VK_ERROR_OUT_OF_HOST_MEMORY:
      case VK_ERROR_OUT_OF_DEVICE_MEMORY:
      case VK_ERROR_DEVICE_LOST:
      default:
        spn_dispatch_track_dump(dispatch, timeout_ns, result);
        spn_device_lost(device);
        return SPN_ERROR_CONTEXT_LOST;
    }

  //
  // collect signalled dispatches...
  //
  uint32_t still_executing = 0;

  for (uint32_t ii = 0; ii < count_executing; ii++)
    {
      spn_dispatch_id_t const id = dispatch->indices.executing[ii];

      switch (vkGetFenceStatus(device->environment.d, fences[ii]))
        {
          case VK_SUCCESS:
            dispatch->indices.complete[dispatch->counts.complete++] = id;
            break;

          case VK_NOT_READY:
            dispatch->indices.executing[still_executing++] = id;
            break;

          default:
            spn_device_lost(device);
            return SPN_ERROR_CONTEXT_LOST;
        }
    }

  //
  // save count of dispatches that weren't signalled
  //
  dispatch->counts.executing = still_executing;

  return SPN_SUCCESS;
}

//
//
//

static spn_result_t
spn_device_wait_for_fences(struct spn_device * const device,
                           uint32_t const            imports_count,
                           VkFence * const           imports,
                           bool const                wait_all,
                           uint64_t const            timeout_ns,
                           uint32_t * const          executing_count)
{
  struct spn_dispatch * const dispatch = device->dispatch;

  spn_result_t const result = spn_device_dispatch_process_executing(device,
                                                                    dispatch,
                                                                    imports_count,
                                                                    imports,
                                                                    wait_all,
                                                                    timeout_ns);

  // return timeout or worse...
  if (result != SPN_SUCCESS)
    {
      // return count of executing dispatches
      if (executing_count != NULL)
        {
          *executing_count = dispatch->counts.executing;
        }
      return result;
    }

  // otherwise, process completed dispatches...
  spn_device_dispatch_process_complete(device, dispatch);

  // return count of executing dispatches
  if (executing_count != NULL)
    {
      *executing_count = dispatch->counts.executing;
    }

  return SPN_SUCCESS;
}

//
//
//

spn_result_t
spn_device_wait_all(struct spn_device * const device,
                    bool const                wait_all,
                    char const * const        label_name)
{
  // begin debug info label
  if (pfn_vkQueueBeginDebugUtilsLabelEXT != NULL)
    {
      VkDebugUtilsLabelEXT const label = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
        NULL,
        label_name,
        { 0.0f, 0.0f, 1.0f, 1.0f },
      };

      pfn_vkQueueBeginDebugUtilsLabelEXT(spn_device_queue_label(device), &label);
    }

  spn_result_t const result = spn_device_wait_for_fences(device,  //
                                                         0,
                                                         NULL,
                                                         wait_all,
                                                         spn_device_get_timeout_ns(device),
                                                         NULL);

  // end debug info label
  if (pfn_vkQueueEndDebugUtilsLabelEXT != NULL)
    {
      pfn_vkQueueEndDebugUtilsLabelEXT(spn_device_queue_label(device));
    }

  return result;
}

spn_result_t
spn_device_wait(struct spn_device * const device, char const * const label_name)
{
  return spn_device_wait_all(device, false, label_name);
}

//
//
//

//
// CONTEXT SCHEDULING
//

spn_result_t
spn_vk_context_wait(spn_context_t    context,
                    uint32_t const   imports_count,
                    VkFence * const  imports,
                    bool const       wait_all,
                    uint64_t const   timeout_ns,
                    uint32_t * const executing_count)
{
  return spn_device_wait_for_fences(context->device,
                                    imports_count,
                                    imports,
                                    wait_all,
                                    timeout_ns,
                                    executing_count);
}

//
//
//

spn_result_t
spn_device_dispatch_acquire(struct spn_device * const  device,
                            spn_dispatch_stage_e const stage,
                            spn_dispatch_id_t * const  p_id)
{
  struct spn_dispatch * const dispatch = device->dispatch;

  // any available?
  while (dispatch->counts.available == 0)
    {
      spn_result_t const result = spn_device_wait(device, __func__);

      if (result != SPN_SUCCESS)
        {
          return result;
        }
    }

  // pop
  spn_dispatch_id_t const id = dispatch->indices.available[--dispatch->counts.available];

  // save
  *p_id = id;

#ifdef SPN_DISPATCH_TRACK_STAGE
  spn_dispatch_track_begin(&dispatch->indices.track, stage, id);
#endif

  // reset the fence
  vk(ResetFences(device->environment.d, 1, dispatch->fences + id));

  // zero the signals
  struct spn_dispatch_signal * signal = dispatch->signals + id;

  memset(signal, 0, sizeof(*signal));

  // NULL the flush arg
  dispatch->flushes[id].arg = NULL;

  // set up default pfn/data
  dispatch->submitters[id] = (struct spn_dispatch_submitter){

    .pfn  = spn_device_dispatch_submitter_default,
    .data = NULL
  };

  // NULL the completion pfn
  dispatch->completions[id].pfn = NULL;

  // zero the wait count
  dispatch->wait_counts[id] = 0;

  // initialize the cb now
  static VkCommandBufferBeginInfo const cbbi = {

    .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .pNext            = NULL,
    .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    .pInheritanceInfo = NULL
  };

  VkCommandBuffer cb = dispatch->cbs[id];

  // all ids are eventually submitted
  vk(BeginCommandBuffer(cb, &cbbi));

  // label the cb
  spn_debug_utils_cmd_begin_stage(cb, stage);

  return SPN_SUCCESS;
}

//
//
//

VkCommandBuffer
spn_device_dispatch_get_cb(struct spn_device * const device, spn_dispatch_id_t const id)
{
  return device->dispatch->cbs[id];
}

void
spn_device_dispatch_set_submitter(struct spn_device * const          device,
                                  spn_dispatch_id_t const            id,
                                  spn_dispatch_submitter_pfn_t const submitter_pfn,
                                  void *                             submitter_data)
{
  struct spn_dispatch * const dispatch = device->dispatch;

  // save pfn and data
  struct spn_dispatch_submitter * const submitter = dispatch->submitters + id;

  submitter->pfn  = submitter_pfn;
  submitter->data = submitter_data;
}

void *
spn_device_dispatch_set_completion(struct spn_device * const           device,
                                   spn_dispatch_id_t const             id,
                                   spn_dispatch_completion_pfn_t const completion_pfn,
                                   size_t const                        completion_payload_size)
{
  assert(completion_payload_size <= SPN_DISPATCH_COMPLETION_PAYLOAD_SIZE);

  struct spn_dispatch * const dispatch = device->dispatch;

  // save pfn and return payload
  struct spn_dispatch_completion * const completion = dispatch->completions + id;

  completion->pfn = completion_pfn;

  return completion->payload;
}

//
//
//

void
spn_device_dispatch_set_flush_arg(struct spn_device * const device,
                                  spn_dispatch_id_t const   id,
                                  void *                    arg)
{
  struct spn_dispatch * const dispatch = device->dispatch;

  // save pfn and return payload
  struct spn_dispatch_flush * const flush = dispatch->flushes + id;

  flush->arg = arg;
}

void
spn_device_dispatch_reset_flush_arg(struct spn_device * const device, spn_dispatch_id_t const id)
{
  struct spn_dispatch * const dispatch = device->dispatch;

  // save pfn and return payload
  struct spn_dispatch_flush * const flush = dispatch->flushes + id;

  flush->arg = NULL;
}

//
//
//

void
spn_device_dispatch_submit(struct spn_device * const device, spn_dispatch_id_t const id)
{
  struct spn_dispatch * const dispatch = device->dispatch;
  VkCommandBuffer             cb       = dispatch->cbs[id];

  spn_debug_utils_cmd_end(cb);

  //
  // end the command buffer
  //
  vk(EndCommandBuffer(cb));

  //
  // shortcut: launch immediately if there are no dependencies
  //
  spn_dispatch_stage_wait_count_t const wait_count = dispatch->wait_counts[id];

  if (wait_count == 0)
    {
      // push to executing
      dispatch->indices.executing[dispatch->counts.executing++] = id;

      // submit!
      dispatch->submitters[id].pfn(spn_device_queue_next(device),
                                   dispatch->fences[id],
                                   cb,
                                   dispatch->submitters[id].data);
    }
#ifdef SPN_DISPATCH_TRACK_WAITING
  else
    {
      dispatch->counts.waiting += 1;
    }
#endif
}

//
//
//

void
spn_device_dispatch_happens_after(struct spn_device * const device,
                                  spn_dispatch_id_t const   id_after,
                                  spn_dispatch_id_t const   id_before)
{
  struct spn_dispatch * const dispatch = device->dispatch;

  uint32_t const bitmap_after_dword_idx  = id_after / 32;
  uint32_t const bitmap_after_dword_bit  = id_after & 31;
  uint32_t const bitmap_after_dword_mask = 1u << bitmap_after_dword_bit;

  struct spn_dispatch_signal * const signal = dispatch->signals + id_before;

  uint32_t * const bitmap_after_dword = signal->bitmap + bitmap_after_dword_idx;

  uint32_t const curr = *bitmap_after_dword;
  uint32_t const next = (curr | bitmap_after_dword_mask);

  if (next != curr)
    {
      // update the index dword
      uint32_t const bitmap_after_dword_mask = 1u << bitmap_after_dword_idx;

      signal->index |= bitmap_after_dword_mask;

      // update the bitmap dword
      *bitmap_after_dword = next;

      dispatch->wait_counts[id_after] += 1;
    }
}

//
//
//

static void
spn_dispatch_happens_after_dword(struct spn_dispatch * const dispatch,
                                 uint32_t const              bitmap_after_index_mask,
                                 uint32_t const              bitmap_after_dword_idx,
                                 uint32_t const              bitmap_after_dword_mask,
                                 uint32_t const              bitmap_before_base,
                                 uint32_t                    bitmap_before_dword)
{
  do
    {
      // which bit is lit?
      uint32_t const lsb_plus_1 = __builtin_ffs(bitmap_before_dword);
      uint32_t const lsb        = lsb_plus_1 - 1;
      uint32_t const mask       = 1u << lsb;

      // mask off lsb
      bitmap_before_dword &= ~mask;

      // which dispatch?
      uint32_t const idx = bitmap_before_base + lsb;

      // get the signaller
      struct spn_dispatch_signal * const signal = dispatch->signals + idx;

      // update the signaller index
      signal->index |= bitmap_after_index_mask;

      // update the signaller bitmap
      signal->bitmap[bitmap_after_dword_idx] |= bitmap_after_dword_mask;
    }
  while (bitmap_before_dword != 0);
}

//
//
//

static void
spn_dispatch_accumulate_stage_ids(uint32_t * const                bitmap,
                                  spn_dispatch_stage_id_t * const stage_ids,
                                  spn_handle_t const * const      handles,
                                  uint32_t const                  count)
{
  spn_dispatch_stage_id_t stage_id_prev = SPN_DISPATCH_STAGE_ID_INVALID;

  for (uint32_t ii = 0; ii < count; ii++)
    {
      spn_handle_t const handle = handles[ii];

      spn_dispatch_stage_id_t const stage_id = stage_ids[handle];

      if (stage_id == stage_id_prev)
        continue;

      stage_id_prev = stage_id;

      if (stage_id < SPN_DISPATCH_STAGE_ID_INVALID)
        {
          uint32_t const bitmap_dword_idx  = stage_id / 32;
          uint32_t const bitmap_dword_bit  = stage_id & 31;
          uint32_t const bitmap_dword_mask = 1u << bitmap_dword_bit;

          bitmap[bitmap_dword_idx] |= bitmap_dword_mask;
        }
    }
}

//
//
//

static void
spn_dispatch_flush_stage_ids(struct spn_dispatch * const     dispatch,
                             spn_dispatch_flush_pfn_t const  flush_pfn,
                             uint32_t * const                bitmap,
                             spn_dispatch_stage_id_t * const stage_ids,
                             spn_handle_t const * const      handles,
                             uint32_t const                  count)
{
  spn_dispatch_stage_id_t stage_id_prev = SPN_DISPATCH_STAGE_ID_INVALID;

  for (uint32_t ii = 0; ii < count; ii++)
    {
      spn_handle_t const handle = handles[ii];

      spn_dispatch_stage_id_t const stage_id = stage_ids[handle];

      if (stage_id == stage_id_prev)
        continue;

      stage_id_prev = stage_id;

      if (stage_id < SPN_DISPATCH_STAGE_ID_INVALID)
        {
          uint32_t const bitmap_dword_idx  = stage_id / 32;
          uint32_t const bitmap_dword_bit  = stage_id & 31;
          uint32_t const bitmap_dword_mask = 1u << bitmap_dword_bit;
          uint32_t const bitmap_dword_prev = bitmap[bitmap_dword_idx];

          // only probe once
          if ((bitmap_dword_prev & bitmap_dword_mask) == 0)
            {
              bitmap[bitmap_dword_idx] = bitmap_dword_prev | bitmap_dword_mask;

              struct spn_dispatch_flush * const flush_ptr = dispatch->flushes + stage_id;
              struct spn_dispatch_flush const   flush     = *flush_ptr;

              if (flush.arg != NULL)
                {
                  flush_pfn(flush.arg);  // flush is cleared by flush_pfn
                }
            }
        }
    }
}

//
// NOTE(allanmac): We need to enforce that there is a maximum total
// number of path and raster builders in order to avoid deadlock.
//
// Unlike other Spinel dispatch clients, the path and raster builders
// acquire and hold a dispatch well before launch.
//
// Note that the span will never be zero.
//

void
spn_device_dispatch_happens_after_handles_and_submit(struct spn_device * const      device,
                                                     spn_dispatch_flush_pfn_t const flush_pfn,
                                                     spn_dispatch_id_t const        id_after,
                                                     spn_handle_t const * const     handles,
                                                     uint32_t const                 size,
                                                     uint32_t const                 head,
                                                     uint32_t const                 span)
{
  struct spn_dispatch * const dispatch = device->dispatch;

  //
  // accumulate all dependencies to bitmap
  //
  struct spn_dispatch_signal signal_before = { 0 };

  uint32_t const count_lo = MIN_MACRO(uint32_t, head + span, size) - head;

  spn_dispatch_accumulate_stage_ids(signal_before.bitmap,
                                    dispatch->handle_stage_ids,
                                    handles + head,
                                    count_lo);

  if (span > count_lo)
    {
      uint32_t const count_hi = span - count_lo;

      spn_dispatch_accumulate_stage_ids(signal_before.bitmap,
                                        dispatch->handle_stage_ids,
                                        handles,
                                        count_hi);
    }

  //
  // update all dependencies with id_after
  //
  uint32_t const bitmap_after_dword_idx  = id_after / 32;
  uint32_t const bitmap_after_index_mask = 1u << bitmap_after_dword_idx;
  uint32_t const bitmap_after_dword_bit  = id_after & 31;
  uint32_t const bitmap_after_dword_mask = 1u << bitmap_after_dword_bit;

  uint32_t wait_count = 0;

  for (uint32_t ii = 0; ii < SPN_DISPATCH_SIGNAL_BITMAP_DWORDS; ii++)
    {
      uint32_t bitmap_before_dword = signal_before.bitmap[ii];

      if (bitmap_before_dword != 0)
        {
          // accumulate count
          wait_count += __builtin_popcount(bitmap_before_dword);

          // update signaller
          spn_dispatch_happens_after_dword(dispatch,
                                           bitmap_after_index_mask,
                                           bitmap_after_dword_idx,
                                           bitmap_after_dword_mask,
                                           ii * 32,
                                           bitmap_before_dword);
        }
    }

  //
  // update wait count
  //
  if (wait_count > 0)
    {
      dispatch->wait_counts[id_after] += wait_count;
    }

  //
  // submit the dispatch
  //
  spn_device_dispatch_submit(device, id_after);

  //
  // flush all dependencies
  //
  struct spn_dispatch_signal flush_before = { 0 };

  spn_dispatch_flush_stage_ids(dispatch,
                               flush_pfn,
                               flush_before.bitmap,
                               dispatch->handle_stage_ids,
                               handles + head,
                               count_lo);

  if (span > count_lo)
    {
      uint32_t const count_hi = span - count_lo;

      spn_dispatch_flush_stage_ids(dispatch,
                                   flush_pfn,
                                   flush_before.bitmap,
                                   dispatch->handle_stage_ids,
                                   handles,
                                   count_hi);
    }
}

//
//
//

void
spn_device_dispatch_register_handle(struct spn_device * const device,
                                    spn_dispatch_id_t const   id,
                                    spn_handle_t const        handle)
{
  struct spn_dispatch * const dispatch = device->dispatch;

  dispatch->handle_stage_ids[handle] = id;
}

//
//
//

static void
spn_dispatch_stage_ids_invalidate(spn_dispatch_stage_id_t * const stage_ids,
                                  spn_handle_t const * const      handles,
                                  uint32_t const                  count)
{
  for (uint32_t ii = 0; ii < count; ii++)
    {
      spn_handle_t const handle = handles[ii];

      stage_ids[handle] = SPN_DISPATCH_STAGE_ID_INVALID;
    }
}

//
// invalidate the ring span of handles
//

void
spn_device_dispatch_handles_complete(struct spn_device * const  device,
                                     spn_handle_t const * const handles,
                                     uint32_t const             size,
                                     uint32_t const             head,
                                     uint32_t const             span)
{
  struct spn_dispatch * const dispatch = device->dispatch;

  uint32_t const count_lo = MIN_MACRO(uint32_t, head + span, size) - head;

  spn_dispatch_stage_ids_invalidate(dispatch->handle_stage_ids, handles + head, count_lo);

  if (span > count_lo)
    {
      uint32_t const count_hi = span - count_lo;

      spn_dispatch_stage_ids_invalidate(dispatch->handle_stage_ids, handles, count_hi);
    }
}

//
//
//
