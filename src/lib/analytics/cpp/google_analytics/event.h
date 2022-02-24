// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_ANALYTICS_CPP_GOOGLE_ANALYTICS_EVENT_H_
#define SRC_LIB_ANALYTICS_CPP_GOOGLE_ANALYTICS_EVENT_H_

#include <optional>
#include <string_view>

#include "src/lib/analytics/cpp/google_analytics/hit.h"

namespace analytics::google_analytics {

// Representation of a Google Analytics event.
// See
// https://developers.google.com/analytics/devguides/collection/protocol/v1/parameters#events
class Event : public Hit {
 public:
  Event(std::string_view category, std::string_view action,
        const std::optional<std::string_view>& label = std::nullopt,
        const std::optional<int64_t>& value = std::nullopt);
};

}  // namespace analytics::google_analytics

#endif  // SRC_LIB_ANALYTICS_CPP_GOOGLE_ANALYTICS_EVENT_H_
