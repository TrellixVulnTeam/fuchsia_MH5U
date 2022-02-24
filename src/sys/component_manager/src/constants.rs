// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {lazy_static::lazy_static, std::path::PathBuf};

lazy_static! {
    pub static ref PKG_PATH: PathBuf = PathBuf::from("/pkg");
}
