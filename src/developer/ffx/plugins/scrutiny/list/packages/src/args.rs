// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {argh::FromArgs, ffx_core::ffx_command};

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "packages",
    description = "Lists all the packages in the build",
    example = "To list all the packages in the build:

        $ffx scrutiny list packages",
    note = "Lists all the packages in the build in a json format."
)]
pub struct ScrutinyPackagesCommand {}
