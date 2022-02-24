// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_CONNECTIVITY_WEAVE_ADAPTATION_WARM_PLATFORM_CONFIG_H_
#define SRC_CONNECTIVITY_WEAVE_ADAPTATION_WARM_PLATFORM_CONFIG_H_

// ==================== Platform Adaptations ====================

#undef WARM_CONFIG_SUPPORT_LEGACY6LOWPAN_NETWORK
#undef WARM_CONFIG_SUPPORT_WIFI
#undef WARM_CONFIG_SUPPORT_CELLULAR
#undef WARM_CONFIG_SUPPORT_WEAVE_TUNNEL
#undef WARM_CONFIG_SUPPORT_BORDER_ROUTING

#define WARM_CONFIG_SUPPORT_LEGACY6LOWPAN_NETWORK 0
#define WARM_CONFIG_SUPPORT_WIFI 1
#define WARM_CONFIG_SUPPORT_CELLULAR 0
#define WARM_CONFIG_SUPPORT_WEAVE_TUNNEL 1
#define WARM_CONFIG_SUPPORT_BORDER_ROUTING 1

// ========== Platform-specific Configuration Overrides =========

/* none so far */

#endif  // SRC_CONNECTIVITY_WEAVE_ADAPTATION_WARM_PLATFORM_CONFIG_H_
