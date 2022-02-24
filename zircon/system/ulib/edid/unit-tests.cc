// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/edid/edid.h>

#include <zxtest/zxtest.h>

TEST(EdidTest, CaeValidationDtdOverflow) {
  edid::CeaEdidTimingExtension cea = {};
  cea.tag = edid::CeaEdidTimingExtension::kTag;
  cea.dtd_start_idx = 2;

  ASSERT_FALSE(cea.validate());
}

TEST(EdidTest, EisaVidLookup) {
  EXPECT_TRUE(!strcmp(edid::GetEisaVendorName(0x1e6d), "GOLDSTAR COMPANY LTD"));
  EXPECT_TRUE(!strcmp(edid::GetEisaVendorName(0x5a63), "VIEWSONIC CORPORATION"));
}
