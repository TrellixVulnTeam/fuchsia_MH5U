// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "log_listener_ostream.h"

#include "format.h"

namespace netemul {
namespace internal {

/*
 * LogListenerOStreamImpl
 *
 * public
 */

LogListenerOStreamImpl::LogListenerOStreamImpl(
    fidl::InterfaceRequest<fuchsia::logger::LogListenerSafe> request, std::string prefix,
    std::ostream* stream, async_dispatcher_t* dispatcher)
    : LogListenerImpl(std::move(request), std::move(prefix), dispatcher), stream_(stream) {
  assert(stream_);
}

/*
 * LogListenerOStreamImpl
 *
 * protected
 */

void LogListenerOStreamImpl::LogImpl(fuchsia::logger::LogMessage m) {
  *stream_ << "[" << prefix_ << "]";
  FormatTime(m.time);
  *stream_ << "[" << m.pid << "]"
           << "[" << m.tid << "]";
  FormatTags(m.tags);
  FormatLogLevel(m.severity);
  *stream_ << " " << m.msg << std::endl;
}

/*
 * LogListenerOStreamImpl
 *
 * private
 */

void LogListenerOStreamImpl::FormatTime(const zx_time_t timestamp) {
  internal::FormatTime(stream_, timestamp);
}

void LogListenerOStreamImpl::FormatTags(const std::vector<std::string>& tags) {
  internal::FormatTags(stream_, tags);
}

void LogListenerOStreamImpl::FormatLogLevel(const int32_t severity) {
  internal::FormatLogLevel(stream_, severity);
}

}  // namespace internal
}  // namespace netemul
