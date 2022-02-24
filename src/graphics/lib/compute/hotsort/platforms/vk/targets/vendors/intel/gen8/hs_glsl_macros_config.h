// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_LIB_COMPUTE_HOTSORT_PLATFORMS_VK_TARGETS_VENDORS_INTEL_GEN8_HS_GLSL_MACROS_CONFIG_H_
#define SRC_GRAPHICS_LIB_COMPUTE_HOTSORT_PLATFORMS_VK_TARGETS_VENDORS_INTEL_GEN8_HS_GLSL_MACROS_CONFIG_H_

//
//
//

#include "hs_glsl_macros.h"

//
// CHOOSE A COMPARE-EXCHANGE IMPLEMENTATION
//

#if (HS_KEY_DWORDS == 1)
#define HS_CMP_XCHG(a, b) HS_CMP_XCHG_V0(a, b)
#elif (HS_KEY_DWORDS == 2)
#define HS_CMP_XCHG(a, b) HS_CMP_XCHG_V0(a, b)
#endif

//
// CHOOSE A CONDITIONAL MIN/MAX IMPLEMENTATION
//

#if (HS_KEY_DWORDS == 1)
#define HS_COND_MIN_MAX(lt, a, b) HS_COND_MIN_MAX_V0(lt, a, b)
#elif (HS_KEY_DWORDS == 2)
#define HS_COND_MIN_MAX(lt, a, b) HS_COND_MIN_MAX_V0(lt, a, b)
#endif

//
//
//

#endif  // SRC_GRAPHICS_LIB_COMPUTE_HOTSORT_PLATFORMS_VK_TARGETS_VENDORS_INTEL_GEN8_HS_GLSL_MACROS_CONFIG_H_
