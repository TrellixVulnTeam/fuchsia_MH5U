// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {argh::FromArgs, ffx_core::ffx_command};

/// Options for "ffx debug connect".
#[ffx_command()]
#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand, name = "connect", description = "start the debugger and connect to the target")]
pub struct ConnectCommand {
    /// extra arguments passed to zxdb. Any arguments starting with "-" must be after a "--" separator.
    #[argh(positional)]
    pub zxdb_args: Vec<String>,
}
