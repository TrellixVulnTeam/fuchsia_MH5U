// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msd_intel_context.h"

#include "address_space.h"
#include "command_buffer.h"
#include "magma_intel_gen_defs.h"
#include "msd_intel_connection.h"
#include "platform_logger.h"
#include "platform_thread.h"
#include "platform_trace.h"

void MsdIntelContext::SetEngineState(EngineCommandStreamerId id,
                                     std::unique_ptr<MsdIntelBuffer> context_buffer,
                                     std::unique_ptr<Ringbuffer> ringbuffer) {
  DASSERT(context_buffer);
  DASSERT(ringbuffer);

  auto iter = state_map_.find(id);
  DASSERT(iter == state_map_.end());

  state_map_[id] = PerEngineState{std::move(context_buffer), nullptr, std::move(ringbuffer)};
}

bool MsdIntelContext::Map(std::shared_ptr<AddressSpace> address_space, EngineCommandStreamerId id) {
  auto iter = state_map_.find(id);
  if (iter == state_map_.end())
    return DRETF(false, "couldn't find engine command streamer");

  DLOG("Mapping context for engine %d", id);

  PerEngineState& state = iter->second;

  if (state.context_mapping) {
    if (state.context_mapping->address_space().lock() == address_space)
      return true;
    return DRETF(false, "already mapped to a different address space");
  }

  state.context_mapping = AddressSpace::MapBufferGpu(address_space, state.context_buffer);
  if (!state.context_mapping)
    return DRETF(false, "context map failed");

  if (!state.ringbuffer->Map(address_space, &state.ringbuffer_gpu_addr)) {
    state.context_mapping.reset();
    return DRETF(false, "ringbuffer map failed");
  }

  return true;
}

bool MsdIntelContext::Unmap(EngineCommandStreamerId id) {
  auto iter = state_map_.find(id);
  if (iter == state_map_.end())
    return DRETF(false, "couldn't find engine command streamer");

  DLOG("Unmapping context for engine %d", id);

  PerEngineState& state = iter->second;

  if (!state.context_mapping)
    return DRETF(false, "context not mapped");

  state.context_mapping.reset();

  if (!state.ringbuffer->Unmap())
    return DRETF(false, "ringbuffer unmap failed");

  return true;
}

bool MsdIntelContext::GetGpuAddress(EngineCommandStreamerId id, gpu_addr_t* addr_out) {
  auto iter = state_map_.find(id);
  if (iter == state_map_.end())
    return DRETF(false, "couldn't find engine command streamer");

  PerEngineState& state = iter->second;
  if (!state.context_mapping)
    return DRETF(false, "context not mapped");

  *addr_out = state.context_mapping->gpu_addr();
  return true;
}

bool MsdIntelContext::GetRingbufferGpuAddress(EngineCommandStreamerId id, gpu_addr_t* addr_out) {
  auto iter = state_map_.find(id);
  if (iter == state_map_.end())
    return DRETF(false, "couldn't find engine command streamer");

  PerEngineState& state = iter->second;
  if (!state.context_mapping)
    return DRETF(false, "context not mapped");

  *addr_out = state.ringbuffer_gpu_addr;

  return true;
}

MsdIntelContext::~MsdIntelContext() { DASSERT(!wait_thread_.joinable()); }

void MsdIntelContext::Shutdown() {
  if (semaphore_port_)
    semaphore_port_->Close();

  if (wait_thread_.joinable()) {
    DLOG("joining wait thread");
    wait_thread_.join();
    DLOG("joined wait thread");
  }

  semaphore_port_.reset();

  // Clear presubmit command buffers so buffer release doesn't see stuck mappings
  {
    std::lock_guard lock(presubmit_mutex_);
    while (presubmit_queue_.size()) {
      presubmit_queue_.pop();
    }
  }
}

magma::Status MsdIntelContext::SubmitCommandBuffer(std::unique_ptr<CommandBuffer> command_buffer) {
  TRACE_DURATION("magma", "SubmitCommandBuffer");
  uint64_t ATTRIBUTE_UNUSED buffer_id = command_buffer->GetBatchBufferId();
  TRACE_FLOW_STEP("magma", "command_buffer", buffer_id);

  {
    auto context_command_streamer = GetTargetCommandStreamer();

    EngineCommandStreamerId desired_command_streamer =
        (command_buffer->GetFlags() & kMagmaIntelGenCommandBufferForVideo)
            ? VIDEO_COMMAND_STREAMER
            : RENDER_COMMAND_STREAMER;

    if (context_command_streamer) {
      if (*context_command_streamer != desired_command_streamer)
        return DRET_MSG(MAGMA_STATUS_INVALID_ARGS,
                        "Context command streamer %d != desired command streamer %d",
                        *context_command_streamer, desired_command_streamer);

    } else {
      SetTargetCommandStreamer(desired_command_streamer);
    }
  }

  {
    std::shared_ptr<MsdIntelContext> context = command_buffer->GetContext().lock();
    DASSERT(context.get() == static_cast<MsdIntelContext*>(this));

    std::shared_ptr<MsdIntelConnection> connection = context->connection().lock();
    if (!connection)
      return DRET(MAGMA_STATUS_CONNECTION_LOST);

    // If there are any mappings pending release, submit them now.
    connection->SubmitPendingReleaseMappings(context);
  }

  if (killed())
    return DRET(MAGMA_STATUS_CONTEXT_KILLED);

  return SubmitBatch(std::move(command_buffer));
}

magma::Status MsdIntelContext::SubmitBatch(std::unique_ptr<MappedBatch> batch) {
  if (!semaphore_port_) {
    semaphore_port_ = magma::SemaphorePort::Create();

    DASSERT(!wait_thread_.joinable());
    wait_thread_ = std::thread([this] {
      magma::PlatformThreadHelper::SetCurrentThreadName("ContextWaitThread");
      DLOG("context wait thread started");
      while (semaphore_port_->WaitOne()) {
      }
      DLOG("context wait thread exited");
    });
  }

  {
    std::lock_guard lock(presubmit_mutex_);
    presubmit_queue_.push(std::move(batch));

    if (presubmit_queue_.size() == 1)
      return SubmitBatchLocked();
  }

  return MAGMA_STATUS_OK;
}

magma::Status MsdIntelContext::SubmitBatchLocked() {
  auto callback = [this](magma::SemaphorePort::WaitSet* wait_set) {
    std::lock_guard lock(presubmit_mutex_);
    this->SubmitBatchLocked();
  };

  while (presubmit_queue_.size()) {
    DLOG("presubmit_queue_ size %zu", presubmit_queue_.size());

    std::vector<std::shared_ptr<magma::PlatformSemaphore>> semaphores;

    auto& batch = presubmit_queue_.front();

    if (batch->IsCommandBuffer()) {
      // Takes ownership
      semaphores = static_cast<CommandBuffer*>(batch.get())->wait_semaphores();
    }

    if (semaphores.size() == 0) {
      auto connection = connection_.lock();
      if (!connection)
        return DRET_MSG(MAGMA_STATUS_CONNECTION_LOST, "couldn't lock reference to connection");

      if (killed())
        return DRET(MAGMA_STATUS_CONTEXT_KILLED);

      if (batch->IsCommandBuffer()) {
        TRACE_DURATION("magma", "SubmitBatchLocked");
        uint64_t ATTRIBUTE_UNUSED buffer_id =
            static_cast<CommandBuffer*>(batch.get())->GetBatchBufferId();
        TRACE_FLOW_STEP("magma", "command_buffer", buffer_id);
      }
      connection->SubmitBatch(std::move(batch));
      presubmit_queue_.pop();
    } else {
      DLOG("adding waitset with %zu semaphores", semaphores.size());

      // Invoke the callback when semaphores are satisfied;
      // the next ProcessPendingFlip will see an empty semaphore array for the front request.
      bool result = semaphore_port_->AddWaitSet(
          std::make_unique<magma::SemaphorePort::WaitSet>(callback, std::move(semaphores)));
      if (result) {
        break;
      } else {
        MAGMA_LOG(WARNING, "SubmitBatchLocked: failed to add to waitset");
      }
    }
  }

  return MAGMA_STATUS_OK;
}

void MsdIntelContext::Kill() {
  if (killed_)
    return;
  killed_ = true;
  auto connection = connection_.lock();
  if (connection)
    connection->SendContextKilled();
}

//////////////////////////////////////////////////////////////////////////////

void msd_context_destroy(msd_context_t* ctx) {
  auto abi_context = MsdIntelAbiContext::cast(ctx);
  // get a copy of the shared ptr
  auto client_context = abi_context->ptr();
  // delete the abi container
  delete abi_context;
  // can safely unmap contexts only from the device thread; for that we go through the connection
  auto connection = client_context->connection().lock();
  DASSERT(connection);
  connection->DestroyContext(std::move(client_context));
}

magma_status_t msd_context_execute_immediate_commands(msd_context_t* ctx, uint64_t commands_size,
                                                      void* commands, uint64_t semaphore_count,
                                                      msd_semaphore_t** msd_semaphores) {
  return MAGMA_STATUS_CONTEXT_KILLED;
}

magma_status_t msd_context_execute_command_buffer_with_resources(
    msd_context_t* ctx, magma_command_buffer* cmd_buf, magma_exec_resource* exec_resources,
    msd_buffer_t** buffers, msd_semaphore_t** wait_semaphores,
    msd_semaphore_t** signal_semaphores) {
  auto context = MsdIntelAbiContext::cast(ctx)->ptr();

  auto command_buffer = CommandBuffer::Create(context, cmd_buf, exec_resources, buffers,
                                              wait_semaphores, signal_semaphores);
  if (!command_buffer)
    return DRET_MSG(MAGMA_STATUS_INVALID_ARGS, "Failed to create command buffer");

  TRACE_DURATION_BEGIN("magma", "PrepareForExecution", "id", command_buffer->GetBatchBufferId());
  if (!command_buffer->PrepareForExecution())
    return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "Failed to prepare command buffer for execution");
  TRACE_DURATION_END("magma", "PrepareForExecution");

  magma::Status status = context->SubmitCommandBuffer(std::move(command_buffer));
  return status.get();
}
