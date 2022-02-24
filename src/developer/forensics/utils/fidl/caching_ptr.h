// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_UTILS_FIDL_CACHING_PTR_H_
#define SRC_DEVELOPER_FORENSICS_UTILS_FIDL_CACHING_PTR_H_

#include <lib/async/cpp/task.h>
#include <lib/async/dispatcher.h>
#include <lib/fidl/cpp/interface_ptr.h>
#include <lib/fit/function.h>
#include <lib/fpromise/promise.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/syslog/cpp/macros.h>

#include <functional>

#include "src/developer/forensics/utils/errors.h"
#include "src/developer/forensics/utils/fit/bridge_map.h"
#include "src/developer/forensics/utils/fit/timeout.h"
#include "src/lib/backoff/exponential_backoff.h"

namespace forensics {
namespace fidl {

// Wrapper around InterfacePtr<Interface> that can cache the results of calls made to the interface.
//
// For example, if we wished to fetch a device's update channel from
// fuchsia::update::channel::Provider then we would use CachingPtr as follows:
//
// class CachingChannelPtr {
// public:
//  CachingChannelPtr(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory>
//  services)
//      : connection_(dispatcher, services, [this] { GetChannel(); }) {}
//
//  ::fpromise::promise<std::string> GetChannel(zx::duration timeout) {
//    return connection_.GetValue(fit::Timeout(timeout));
//  }
//
// private:
//  void GetChannel() {
//    connection_->GetCurrent([this](std::string channel) {
//      std::optional<std::string> value;
//      if (!channel.empty()) {
//        connection_.SetValue(channel);
//      } else {
//        connection_.SetError(Error::kMissingValue);
//      }
//    });
//  }
//
//  CachingPtr<fuchsia::update::channel::Provider, std::string> connection_;
// };
//
// This class is not thread safe.
template <typename Interface, typename V>
class CachingPtr {
 public:
  CachingPtr(async_dispatcher_t* dispatcher, std::shared_ptr<sys::ServiceDirectory> services,
             std::function<void(void)> make_call)
      : dispatcher_(dispatcher),
        services_(services),
        pending_calls_(dispatcher_),
        make_call_(make_call),
        make_call_task_([this] {
          Connect();
          make_call_();
        }),
        make_call_backoff_(/*initial_delay=*/zx::msec(100), /*retry_factor=*/2u,
                           /*max_delay=*/zx::hour(1)) {
    // Post |make_call_| on the async loop with an immediate deadline in an attempt to
    // pre-cache |value_|. Because the class that owns the CachingPtr and supplies |make_call_| may
    // capture itself in the lambda, we need to ensure that the owning class is fully initialized
    // before |make_call_| is executed. Thus, we post |make_call_| on the async loop and are
    // guarenteed that any data initialized along side the CachingPtr is initialized before
    // |make_call_| is executed.
    //
    // This isn't safe if |dispatcher_| is on a different thread than |this|.
    make_call_task_.Post(dispatcher_);
  }

  void SetValue(V value) { SetAsDone(std::move(value)); }

  void SetError(Error error) { SetAsDone(error); }

  ::fpromise::promise<V, Error> GetValue(fit::Timeout timeout) {
    if (value_) {
      return ::fpromise::make_result_promise(ValueToResult());
    }

    const uint64_t id = pending_calls_.NewBridgeForTask(Interface::Name_);

    // A call to GetValue() is only ever completed with an error due to circumstances that affect
    // only that call, i.e. the call times out or there is an issue posting the timeout task, so we
    // don't set an error for all pending GetValue() calls, i.e. we don't set |value_| with the
    // Error and instead only propagate the Error to the caller.
    return pending_calls_.WaitForDone(id, std::move(timeout))
        .then([id, this](const ::fpromise::result<void, Error>& result) {
          pending_calls_.Delete(id);
          return result;
        })
        .and_then([this]() { return ValueToResult(); })
        .or_else([](const Error& error) { return ::fpromise::error(error); });
  }

  Interface* operator->() { return connection_.get(); }

 private:
  ::fpromise::result<V, Error> ValueToResult() const {
    if (!value_) {
      FX_LOGS(FATAL) << "Attempting to return a result when none has been cached";
    }

    if (value_->HasValue()) {
      return ::fpromise::ok(value_->Value());
    } else {
      return ::fpromise::error(value_->Error());
    }
  }

  void Connect() {
    connection_ = services_->Connect<Interface>();
    connection_.set_error_handler([this](zx_status_t status) {
      FX_PLOGS(WARNING, status) << "Lost connection with " << Interface::Name_;
      if (const auto post_status =
              make_call_task_.PostDelayed(dispatcher_, make_call_backoff_.GetNext());
          post_status != ZX_OK) {
        FX_PLOGS(ERROR, post_status) << "Failed to post task to make call on async loop";
        SetError(Error::kAsyncTaskPostFailure);
      }
    });
  }

  void SetAsDone(ErrorOr<V> value) {
    value_ = std::make_unique<ErrorOr<V>>(std::move(value));

    pending_calls_.CompleteAllOk();
    connection_.Unbind();

    // We never need to make another call nor re-connect.
    make_call_task_.Cancel();
    make_call_backoff_.Reset();
  }

  async_dispatcher_t* dispatcher_;
  const std::shared_ptr<sys::ServiceDirectory> services_;

  ::fidl::InterfacePtr<Interface> connection_;
  fit::BridgeMap<> pending_calls_;

  // The std::unique_ptr<> indicates whether the value is cached, the ErrorOr<> indicates
  // whether the cached value is a payload or an error.
  std::unique_ptr<ErrorOr<V>> value_;

  std::function<void(void)> make_call_;
  async::TaskClosure make_call_task_;
  backoff::ExponentialBackoff make_call_backoff_;
};

}  // namespace fidl
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_UTILS_FIDL_CACHING_PTR_H_
