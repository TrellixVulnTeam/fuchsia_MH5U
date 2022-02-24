// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async/cpp/task.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <zircon/status.h>

#include <crashsvc/exception_handler.h>
#include <crashsvc/logging.h>

ExceptionHandler::ExceptionHandler(async_dispatcher_t* dispatcher,
                                   zx_handle_t exception_handler_svc,
                                   const zx::duration is_active_timeout)
    : dispatcher_(dispatcher),
      exception_handler_svc_(exception_handler_svc),
      // We are in a build without a server for fuchsia.exception.Handler, e.g., bringup.
      drop_exceptions_(exception_handler_svc_ == ZX_HANDLE_INVALID),
      connection_(),
      is_active_timeout_(is_active_timeout) {
  SetUpClient();
  ConnectToServer();
}

void ExceptionHandler::SetUpClient() {
  if (drop_exceptions_) {
    return;
  }

  auto exception_handler_endpoints = fidl::CreateEndpoints<fuchsia_exception::Handler>();
  if (!exception_handler_endpoints.is_ok()) {
    LogError("Failed to create channel for fuchsia.exception.Handler",
             exception_handler_endpoints.status_value());
    drop_exceptions_ = true;
    return;
  }

  connection_ = {};
  connection_.Bind(std::move(exception_handler_endpoints->client), dispatcher_, this);
  server_endpoint_ = std::move(exception_handler_endpoints->server);
}

void ExceptionHandler::on_fidl_error(const fidl::UnbindInfo info) {
  // If the unbind was only due to dispatcher shutdown, don't reconnect and stop sending exceptions
  // to fuchsia.exception.Handler. This should only happen in tests.
  if (info.is_dispatcher_shutdown()) {
    drop_exceptions_ = true;
    return;
  }

  LogError("Lost connection to fuchsia.exception.Handler", info.status());

  // We immediately bind the |connection_| again, but we don't re-connect to the server of
  // fuchsia.exception.Handler, i.e sending the other endpoint of the channel to the server. Instead
  // the re-connection will be done on the next exception. The reason we don't re-connect (1)
  // immediately is because the server could have been shut down by the system or (2) with a backoff
  // is because we don't want to be queueing up exceptions which underlying processes need to be
  // terminated.
  SetUpClient();
}

void ExceptionHandler::ConnectToServer() {
  if (ConnectedToServer() || drop_exceptions_) {
    return;
  }

  if (const zx_status_t status = fdio_service_connect_at(
          exception_handler_svc_, fidl::DiscoverableProtocolName<fuchsia_exception::Handler>,
          server_endpoint_.channel().release());
      status != ZX_OK) {
    LogError("unable to connect to fuchsia.exception.Handler", status);
    drop_exceptions_ = true;
    return;
  }
}

void ExceptionHandler::Handle(zx::exception exception, const zx_exception_info_t& info) {
  if (drop_exceptions_) {
    return;
  }

  ConnectToServer();

  auto shared_exception = std::make_shared<zx::exception>(std::move(exception));

  auto weak_this = weak_factory_.GetWeakPtr();

  // Sends the exception to the server, if it is still valid, after the call to IsActive
  // has been acknowledged.
  auto is_active_cb = [weak_this, info, shared_exception](auto& result) {
    if (!result.ok()) {
      LogError("Failed to check if handler is active", info, result.status());
      return;
    }

    if (!shared_exception->is_valid()) {
      LogError("Exception was released before hander responded", info);
      return;
    }

    if (weak_this->drop_exceptions_) {
      return;
    }

    weak_this->ConnectToServer();

    fuchsia_exception::wire::ExceptionInfo exception_info;
    exception_info.process_koid = info.pid;
    exception_info.thread_koid = info.tid;
    exception_info.type = static_cast<fuchsia_exception::wire::ExceptionType>(info.type);

    // The server may be in an unresponsive state, unknown here, despite responding to IsActive.
    // However, the response to IsActive narrows window during which it's unknown the server
    // became unresponsive.
    weak_this->connection_->OnException(
        std::move(*shared_exception), exception_info, [info](auto& result) {
          if (!result.ok()) {
            LogError("Failed to pass exception to handler", info, result.status());
          }
        });
  };

  // Releases the exception if it is still valid.
  auto release_exception = [shared_exception, info] {
    if (shared_exception->is_valid()) {
      LogError("Exception handler may be un, releasing exception to kernel", info);
      shared_exception->reset();
    }
  };

  connection_->IsActive(std::move(is_active_cb));
  async::PostDelayedTask(dispatcher_, std::move(release_exception), is_active_timeout_);
}

bool ExceptionHandler::ConnectedToServer() const { return !server_endpoint_.is_valid(); }
