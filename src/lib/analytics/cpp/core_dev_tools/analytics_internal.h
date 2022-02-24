// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_ANALYTICS_CPP_CORE_DEV_TOOLS_ANALYTICS_INTERNAL_H_
#define SRC_LIB_ANALYTICS_CPP_CORE_DEV_TOOLS_ANALYTICS_INTERNAL_H_

#include <string_view>

#include "src/lib/analytics/cpp/core_dev_tools/environment_status.h"
#include "src/lib/analytics/cpp/google_analytics/client.h"

namespace analytics::core_dev_tools::internal {

// Set Google Analytics parameters used by all hits for a session, such as tid (tracking id)
// Note that the input variable bot has three possible status here:
// (1) bot == std::nullopt (default value): will not set ds (data source) parameter
// (2) bot->IsRunByBot() == false: will set ds=user
// (3) bot->IsRunByBot() == true: will set ds=bot-<bot.name>
void PrepareGoogleAnalyticsClient(google_analytics::Client& client, std::string_view tool_name,
                                  std::string_view tracking_id,
                                  std::optional<BotInfo> bot = std::nullopt);

}  // namespace analytics::core_dev_tools::internal

#endif  // SRC_LIB_ANALYTICS_CPP_CORE_DEV_TOOLS_ANALYTICS_INTERNAL_H_
