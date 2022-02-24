// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "zircon_platform_connection.h"

#include "magma_common_defs.h"
#include "zircon_platform_status.h"

namespace magma {

class ZirconPlatformPerfCountPool : public PlatformPerfCountPool {
 public:
  ZirconPlatformPerfCountPool(uint64_t id, zx::channel channel)
      : pool_id_(id), server_end_(std::move(channel)) {}

  uint64_t pool_id() override { return pool_id_; }

  // Sends a OnPerformanceCounterReadCompleted. May be called from any thread.
  magma::Status SendPerformanceCounterCompletion(uint32_t trigger_id, uint64_t buffer_id,
                                                 uint32_t buffer_offset, uint64_t time,
                                                 uint32_t result_flags) override {
    fidl::Result result =
        fidl::WireSendEvent(server_end_)
            ->OnPerformanceCounterReadCompleted(
                trigger_id, buffer_id, buffer_offset, time,
                fuchsia_gpu_magma::wire::ResultFlags::TruncatingUnknown(result_flags));
    switch (result.status()) {
      case ZX_OK:
        return MAGMA_STATUS_OK;
      case ZX_ERR_PEER_CLOSED:
        return MAGMA_STATUS_CONNECTION_LOST;
      case ZX_ERR_TIMED_OUT:
        return MAGMA_STATUS_TIMED_OUT;
      default:
        return MAGMA_STATUS_INTERNAL_ERROR;
    }
  }

 private:
  uint64_t pool_id_;
  fidl::ServerEnd<fuchsia_gpu_magma::PerformanceCounterEvents> server_end_;
};

void ZirconPlatformConnection::SetError(fidl::CompleterBase* completer, magma_status_t error) {
  if (!error_) {
    error_ = DRET_MSG(error, "ZirconPlatformConnection encountered dispatcher error");
    if (completer) {
      completer->Close(magma::ToZxStatus(error));
    } else {
      server_binding_->Close(magma::ToZxStatus(error));
    }
    async_loop()->Quit();
  }
}

bool ZirconPlatformConnection::Bind(zx::channel server_endpoint) {
  fidl::OnUnboundFn<ZirconPlatformConnection> unbind_callback =
      [](ZirconPlatformConnection* self, fidl::UnbindInfo unbind_info,
         fidl::ServerEnd<fuchsia_gpu_magma::Primary> server_channel) {
        // |kDispatcherError| indicates the async loop itself is shutting down,
        // which could only happen when |interface| is being destructed.
        // Therefore, we must avoid using the same object.
        if (unbind_info.reason() == fidl::Reason::kDispatcherError)
          return;

        self->server_binding_ = cpp17::nullopt;
        self->async_loop()->Quit();
      };

  // Note: the async loop should not be started until we assign |server_binding_|.
  server_binding_ = fidl::BindServer(async_loop()->dispatcher(), std::move(server_endpoint), this,
                                     std::move(unbind_callback));
  return true;
}

bool ZirconPlatformConnection::HandleRequest() {
  zx_status_t status = async_loop_.Run(zx::time::infinite(), true /* once */);
  if (status != ZX_OK)
    return false;
  return true;
}

bool ZirconPlatformConnection::BeginShutdownWait() {
  zx_status_t status = async_begin_wait(async_loop()->dispatcher(), &async_wait_shutdown_);
  if (status != ZX_OK)
    return DRETF(false, "Couldn't begin wait on shutdown: %s", zx_status_get_string(status));
  return true;
}

void ZirconPlatformConnection::AsyncWaitHandler(async_dispatcher_t* dispatcher, AsyncWait* wait,
                                                zx_status_t status,
                                                const zx_packet_signal_t* signal) {
  if (status != ZX_OK)
    return;

  bool quit = false;
  if (wait == &async_wait_shutdown_) {
    DASSERT(signal->observed == ZX_EVENT_SIGNALED);
    quit = true;
    DLOG("got shutdown event");
  } else {
    DASSERT(false);
  }

  if (quit) {
    server_binding_->Close(ZX_ERR_CANCELED);
    async_loop()->Quit();
  }
}

bool ZirconPlatformConnection::AsyncTaskHandler(async_dispatcher_t* dispatcher, AsyncTask* task,
                                                zx_status_t status) {
  switch (static_cast<MSD_CONNECTION_NOTIFICATION_TYPE>(task->notification.type)) {
    case MSD_CONNECTION_NOTIFICATION_CHANNEL_SEND: {
      zx_status_t status = zx_channel_write(server_notification_endpoint_.get(), 0,
                                            task->notification.u.channel_send.data,
                                            task->notification.u.channel_send.size, nullptr, 0);
      if (status != ZX_OK)
        return DRETF(false, "Failed writing to channel: %s", zx_status_get_string(status));
      return true;
    }
    case MSD_CONNECTION_NOTIFICATION_CONTEXT_KILLED:
      // Setting the error will close the connection.
      SetError(nullptr, MAGMA_STATUS_CONTEXT_KILLED);
      return true;
    case MSD_CONNECTION_NOTIFICATION_PERFORMANCE_COUNTERS_READ_COMPLETED:
      // Should be handled in MagmaSystemConnection.
      break;
  }
  return DRETF(false, "Unhandled notification type: %lu", task->notification.type);
}

void ZirconPlatformConnection::EnableFlowControl(EnableFlowControlRequestView request,
                                                 EnableFlowControlCompleter::Sync& completer) {
  flow_control_enabled_ = true;
}

void ZirconPlatformConnection::FlowControl(uint64_t size) {
  if (!flow_control_enabled_)
    return;

  messages_consumed_ += 1;
  bytes_imported_ += size;

  if (messages_consumed_ >= kMaxInflightMessages / 2) {
    fidl::Result result =
        fidl::WireSendEvent(server_binding_.value())->OnNotifyMessagesConsumed(messages_consumed_);
    if (result.ok()) {
      messages_consumed_ = 0;
    } else if (!result.is_canceled() && !result.is_peer_closed()) {
      DMESSAGE("SendOnNotifyMessagesConsumedEvent failed: %s", result.FormatDescription().c_str());
    }
  }

  if (bytes_imported_ >= kMaxInflightBytes / 2) {
    fidl::Result result =
        fidl::WireSendEvent(server_binding_.value())->OnNotifyMemoryImported(bytes_imported_);
    if (result.ok()) {
      bytes_imported_ = 0;
    } else if (!result.is_canceled() && !result.is_peer_closed()) {
      DMESSAGE("SendOnNotifyMemoryImportedEvent failed: %s", result.FormatDescription().c_str());
    }
  }
}

void ZirconPlatformConnection::ImportObject(ImportObjectRequestView request,
                                            ImportObjectCompleter::Sync& completer) {
  DLOG("ZirconPlatformConnection: ImportObject");
  auto object_type = static_cast<PlatformObject::Type>(request->object_type);

  uint64_t size = 0;

  if (object_type == magma::PlatformObject::BUFFER) {
    zx::unowned_vmo vmo(request->object.get());
    zx_status_t status = vmo->get_size(&size);
    if (status != ZX_OK) {
      SetError(&completer, MAGMA_STATUS_INVALID_ARGS);
      return;
    }
  }
  FlowControl(size);

  if (!delegate_->ImportObject(request->object.release(),
                               static_cast<PlatformObject::Type>(request->object_type)))
    SetError(&completer, MAGMA_STATUS_INVALID_ARGS);
}

void ZirconPlatformConnection::ReleaseObject(ReleaseObjectRequestView request,
                                             ReleaseObjectCompleter::Sync& completer) {
  DLOG("ZirconPlatformConnection: ReleaseObject");
  FlowControl();

  if (!delegate_->ReleaseObject(request->object_id,
                                static_cast<PlatformObject::Type>(request->object_type)))
    SetError(&completer, MAGMA_STATUS_INVALID_ARGS);
}

void ZirconPlatformConnection::CreateContext(CreateContextRequestView request,
                                             CreateContextCompleter::Sync& completer) {
  DLOG("ZirconPlatformConnection: CreateContext");
  FlowControl();

  magma::Status status = delegate_->CreateContext(request->context_id);
  if (!status.ok())
    SetError(&completer, status.get());
}

void ZirconPlatformConnection::DestroyContext(DestroyContextRequestView request,
                                              DestroyContextCompleter::Sync& completer) {
  DLOG("ZirconPlatformConnection: DestroyContext");
  FlowControl();

  magma::Status status = delegate_->DestroyContext(request->context_id);
  if (!status.ok())
    SetError(&completer, status.get());
}

// DEPRECATED - TODO(fxb/86670) remove
void ZirconPlatformConnection::ExecuteCommandBufferWithResources2(
    ExecuteCommandBufferWithResources2RequestView request,
    ExecuteCommandBufferWithResources2Completer::Sync& completer) {
  FlowControl();

  auto command_buffer = std::make_unique<magma_command_buffer>();

  *command_buffer = {
      .resource_count = static_cast<uint32_t>(request->resources.count()),
      .batch_buffer_resource_index = request->command_buffer.resource_index,
      .batch_start_offset = request->command_buffer.start_offset,
      .wait_semaphore_count = static_cast<uint32_t>(request->wait_semaphores.count()),
      .signal_semaphore_count = static_cast<uint32_t>(request->signal_semaphores.count()),
      .flags = static_cast<uint64_t>(request->command_buffer.flags),
  };

  std::vector<magma_exec_resource> resources;
  resources.reserve(request->resources.count());

  for (auto& buffer_range : request->resources) {
    resources.push_back({
        buffer_range.buffer_id,
        buffer_range.offset,
        buffer_range.size,
    });
  }

  // Merge semaphores into one vector
  std::vector<uint64_t> semaphores;
  semaphores.reserve(request->wait_semaphores.count() + request->signal_semaphores.count());

  for (uint64_t semaphore_id : request->wait_semaphores) {
    semaphores.push_back(semaphore_id);
  }
  for (uint64_t semaphore_id : request->signal_semaphores) {
    semaphores.push_back(semaphore_id);
  }

  magma::Status status = delegate_->ExecuteCommandBufferWithResources(
      request->context_id, std::move(command_buffer), std::move(resources), std::move(semaphores));

  if (!status)
    SetError(&completer, status.get());
}

void ZirconPlatformConnection::ExecuteCommand(ExecuteCommandRequestView request,
                                              ExecuteCommandCompleter::Sync& completer) {
  FlowControl();

  // TODO(fxbug.dev/92606) - support > 1 command buffer
  if (request->command_buffers.count() > 1) {
    SetError(&completer, MAGMA_STATUS_UNIMPLEMENTED);
    return;
  }

  auto command_buffer = std::make_unique<magma_command_buffer>();

  *command_buffer = {
      .resource_count = static_cast<uint32_t>(request->resources.count()),
      .batch_buffer_resource_index = request->command_buffers[0].resource_index,
      .batch_start_offset = request->command_buffers[0].start_offset,
      .wait_semaphore_count = static_cast<uint32_t>(request->wait_semaphores.count()),
      .signal_semaphore_count = static_cast<uint32_t>(request->signal_semaphores.count()),
      .flags = static_cast<uint64_t>(request->flags),
  };

  std::vector<magma_exec_resource> resources;
  resources.reserve(request->resources.count());

  for (auto& buffer_range : request->resources) {
    resources.push_back({
        buffer_range.buffer_id,
        buffer_range.offset,
        buffer_range.size,
    });
  }

  // Merge semaphores into one vector
  std::vector<uint64_t> semaphores;
  semaphores.reserve(request->wait_semaphores.count() + request->signal_semaphores.count());

  for (uint64_t semaphore_id : request->wait_semaphores) {
    semaphores.push_back(semaphore_id);
  }
  for (uint64_t semaphore_id : request->signal_semaphores) {
    semaphores.push_back(semaphore_id);
  }

  magma::Status status = delegate_->ExecuteCommandBufferWithResources(
      request->context_id, std::move(command_buffer), std::move(resources), std::move(semaphores));

  if (!status)
    SetError(&completer, status.get());
}

void ZirconPlatformConnection::ExecuteImmediateCommands(
    ExecuteImmediateCommandsRequestView request,
    ExecuteImmediateCommandsCompleter::Sync& completer) {
  DLOG("ZirconPlatformConnection: ExecuteImmediateCommands");
  FlowControl();

  magma::Status status = delegate_->ExecuteImmediateCommands(
      request->context_id, request->command_data.count(), request->command_data.mutable_data(),
      request->semaphores.count(), request->semaphores.mutable_data());
  if (!status)
    SetError(&completer, status.get());
}

void ZirconPlatformConnection::Flush(FlushRequestView request, FlushCompleter::Sync& completer) {
  DLOG("ZirconPlatformConnection: Flush");
  completer.Reply();
}

void ZirconPlatformConnection::MapBufferGpu(MapBufferGpuRequestView request,
                                            MapBufferGpuCompleter::Sync& completer) {
  DLOG("ZirconPlatformConnection: MapBufferGpuFIDL");
  FlowControl();

  magma::Status status =
      delegate_->MapBufferGpu(request->buffer_id, request->gpu_va, request->page_offset,
                              request->page_count, static_cast<uint64_t>(request->flags));
  if (!status.ok())
    SetError(&completer, status.get());
}

void ZirconPlatformConnection::UnmapBufferGpu(UnmapBufferGpuRequestView request,
                                              UnmapBufferGpuCompleter::Sync& completer) {
  DLOG("ZirconPlatformConnection: UnmapBufferGpuFIDL");
  FlowControl();

  magma::Status status = delegate_->UnmapBufferGpu(request->buffer_id, request->gpu_va);
  if (!status.ok())
    SetError(&completer, status.get());
}

void ZirconPlatformConnection::BufferRangeOp(BufferRangeOpRequestView request,
                                             BufferRangeOpCompleter::Sync& completer) {
  DLOG("ZirconPlatformConnection:::BufferOp %d", static_cast<uint32_t>(request->op));
  FlowControl();
  uint32_t buffer_op;
  switch (request->op) {
    case fuchsia_gpu_magma::wire::BufferOp::kPopulateTables:
      buffer_op = MAGMA_BUFFER_RANGE_OP_POPULATE_TABLES;
      break;
    case fuchsia_gpu_magma::wire::BufferOp::kDepopulateTables:
      buffer_op = MAGMA_BUFFER_RANGE_OP_DEPOPULATE_TABLES;
      break;
    default:
      SetError(&completer, MAGMA_STATUS_INVALID_ARGS);
      return;
  }
  magma::Status status =
      delegate_->BufferRangeOp(request->buffer_id, buffer_op, request->offset, request->length);

  if (!status) {
    SetError(&completer, status.get());
  }
}

void ZirconPlatformConnection::EnablePerformanceCounterAccess(
    EnablePerformanceCounterAccessRequestView request,
    EnablePerformanceCounterAccessCompleter::Sync& completer) {
  DLOG("ZirconPlatformConnection:::EnablePerformanceCounterAccess");
  FlowControl();

  magma::Status status = delegate_->EnablePerformanceCounterAccess(
      magma::PlatformHandle::Create(request->access_token.release()));
  if (!status) {
    SetError(&completer, status.get());
  }
}

void ZirconPlatformConnection::IsPerformanceCounterAccessAllowed(
    IsPerformanceCounterAccessAllowedRequestView request,
    IsPerformanceCounterAccessAllowedCompleter::Sync& completer) {
  DLOG("ZirconPlatformConnection:::IsPerformanceCounterAccessAllowed");
  completer.Reply(delegate_->IsPerformanceCounterAccessAllowed());
}

void ZirconPlatformConnection::EnablePerformanceCounters(
    EnablePerformanceCountersRequestView request,
    EnablePerformanceCountersCompleter::Sync& completer) {
  FlowControl();
  magma::Status status =
      delegate_->EnablePerformanceCounters(request->counters.data(), request->counters.count());
  if (!status) {
    SetError(&completer, status.get());
  }
}

void ZirconPlatformConnection::CreatePerformanceCounterBufferPool(
    CreatePerformanceCounterBufferPoolRequestView request,
    CreatePerformanceCounterBufferPoolCompleter::Sync& completer) {
  FlowControl();
  auto pool = std::make_unique<ZirconPlatformPerfCountPool>(request->pool_id,
                                                            request->event_channel.TakeChannel());
  magma::Status status = delegate_->CreatePerformanceCounterBufferPool(std::move(pool));
  if (!status) {
    SetError(&completer, status.get());
  }
}

void ZirconPlatformConnection::ReleasePerformanceCounterBufferPool(
    ReleasePerformanceCounterBufferPoolRequestView request,
    ReleasePerformanceCounterBufferPoolCompleter::Sync& completer) {
  FlowControl();
  magma::Status status = delegate_->ReleasePerformanceCounterBufferPool(request->pool_id);
  if (!status) {
    SetError(&completer, status.get());
  }
}

void ZirconPlatformConnection::AddPerformanceCounterBufferOffsetsToPool(
    AddPerformanceCounterBufferOffsetsToPoolRequestView request,
    AddPerformanceCounterBufferOffsetsToPoolCompleter::Sync& completer) {
  FlowControl();
  for (auto& offset : request->offsets) {
    magma::Status status = delegate_->AddPerformanceCounterBufferOffsetToPool(
        request->pool_id, offset.buffer_id, offset.offset, offset.size);
    if (!status) {
      SetError(&completer, status.get());
    }
  }
}

void ZirconPlatformConnection::RemovePerformanceCounterBufferFromPool(
    RemovePerformanceCounterBufferFromPoolRequestView request,
    RemovePerformanceCounterBufferFromPoolCompleter::Sync& completer) {
  FlowControl();
  magma::Status status =
      delegate_->RemovePerformanceCounterBufferFromPool(request->pool_id, request->buffer_id);
  if (!status) {
    SetError(&completer, status.get());
  }
}

void ZirconPlatformConnection::DumpPerformanceCounters(
    DumpPerformanceCountersRequestView request, DumpPerformanceCountersCompleter::Sync& completer) {
  FlowControl();
  magma::Status status = delegate_->DumpPerformanceCounters(request->pool_id, request->trigger_id);
  if (!status) {
    SetError(&completer, status.get());
  }
}

void ZirconPlatformConnection::ClearPerformanceCounters(
    ClearPerformanceCountersRequestView request,
    ClearPerformanceCountersCompleter::Sync& completer) {
  FlowControl();
  magma::Status status =
      delegate_->ClearPerformanceCounters(request->counters.data(), request->counters.count());
  if (!status) {
    SetError(&completer, status.get());
  }
}

std::shared_ptr<PlatformConnection> PlatformConnection::Create(
    std::unique_ptr<PlatformConnection::Delegate> delegate, msd_client_id_t client_id,
    std::unique_ptr<magma::PlatformHandle> thread_profile) {
  if (!delegate)
    return DRETP(nullptr, "attempting to create PlatformConnection with null delegate");

  zx::channel server_endpoint;
  zx::channel client_endpoint;
  zx_status_t status = zx::channel::create(0, &server_endpoint, &client_endpoint);
  if (status != ZX_OK)
    return DRETP(nullptr, "zx::channel::create failed");

  zx::channel server_notification_endpoint;
  zx::channel client_notification_endpoint;
  status = zx::channel::create(0, &server_notification_endpoint, &client_notification_endpoint);
  if (status != ZX_OK)
    return DRETP(nullptr, "zx::channel::create failed");

  auto shutdown_event = magma::PlatformEvent::Create();
  if (!shutdown_event)
    return DRETP(nullptr, "Failed to create shutdown event");

  auto connection = std::make_shared<ZirconPlatformConnection>(
      std::move(delegate), client_id, std::move(client_endpoint),
      std::move(server_notification_endpoint), std::move(client_notification_endpoint),
      std::shared_ptr<magma::PlatformEvent>(std::move(shutdown_event)), std::move(thread_profile));

  if (!connection->Bind(std::move(server_endpoint)))
    return DRETP(nullptr, "fidl::BindSingleInFlightOnly failed: %d", status);

  if (!connection->BeginShutdownWait())
    return DRETP(nullptr, "Failed to begin shutdown wait");

  return connection;
}

}  // namespace magma
