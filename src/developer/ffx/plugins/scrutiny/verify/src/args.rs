// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {argh::FromArgs, ffx_core::ffx_command, ffx_scrutiny_verify_sub_command::Subcommand};

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "verify", description = "Verify the build")]
pub struct ScrutinyVerifyCommand {
    #[argh(subcommand)]
    pub subcommand: Subcommand,
}
