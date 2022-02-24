// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <iostream>

#include <src/lib/fxl/strings/string_printf.h>

#include "garnet/bin/trace_manager/trace_provider_bundle.h"

namespace tracing {

TraceProviderBundle::TraceProviderBundle(fuchsia::tracing::provider::ProviderPtr provider,
                                         uint32_t id, zx_koid_t pid, const std::string& name)
    : provider(std::move(provider)), id(id), pid(pid), name(name) {}

std::string TraceProviderBundle::ToString() const {
  // The pid and name should be present, so we don't try to get fancy with
  // the formatting if it turns out they're not.
  return fxl::StringPrintf("#%u {%lu:%s}", id, pid, name.c_str());
}

std::ostream& operator<<(std::ostream& out, const TraceProviderBundle& bundle) {
  return out << bundle.ToString();
}

}  // namespace tracing
