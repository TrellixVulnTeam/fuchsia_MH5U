// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![warn(clippy::all)]

#[fuchsia::component]
async fn main() {
    tracing::info!("Hello, world!");
}
