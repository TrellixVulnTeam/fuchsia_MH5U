// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use hub_report::*;

#[fuchsia::component]
async fn main() {
    expect_dir_listing("/hub/children", vec!["child"]).await;
    expect_dir_listing(
        "/hub/children/child",
        vec!["children", "component_type", "debug", "id", "url"],
    )
    .await;
    expect_dir_listing("/hub/children/child/children", vec![]).await;
}
