// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Types that are shared across more than one crate in the Fuchsia identity stack.
#![deny(missing_docs)]

// Async task management.
mod task_group;
// Staged file atomic write support.
mod staged_file;

pub use crate::task_group::{cancel_or, TaskGroup, TaskGroupCancel, TaskGroupError};

pub use crate::staged_file::{StagedFile, StagedFileError};
