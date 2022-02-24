// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {fidl_fidl_test_components as ftest, fuchsia_component::client};

#[fuchsia::test]
async fn custom_resolved_component_serves_protocol() {
    let trigger = client::connect_to_protocol::<ftest::TriggerMarker>()
        .expect("failed to open trigger service");
    let out = trigger.run().await.expect("trigger failed");
    assert_eq!(out, "Triggered");
}
