// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod channel;
pub mod socket;

pub trait LoggingFixture: Send {
    fn log(&mut self, msg: &str);
}
