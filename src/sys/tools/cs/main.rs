// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![warn(missing_docs)]

//! `cs` performs a Component Search on the current system.

mod freq;
mod log_stats;

use {
    anyhow::Error,
    freq::BlobFrequencies,
    fuchsia_async as fasync,
    log_stats::{LogSeverity, LogStats},
    structopt::StructOpt,
};

#[derive(StructOpt, Debug)]
#[structopt(
    name = "Component Statistics (cs) Reporting Tool",
    about = "Displays information about components on the system."
)]
enum Opt {
    /// DEPRECATED: Use `ffx component list` instead.
    /// Output the component tree.
    #[structopt(name = "tree")]
    Tree {
        // Output only cmx/cml/running/stopped components depending on the flag.
        #[structopt(short = "o", long = "only")]
        // TODO(fxbug.dev/84729)
        #[allow(unused)]
        only: Option<String>,
        // whether or not to display a column showing component type and a column
        // showing running/stopped.
        #[structopt(short = "v", long = "verbose")]
        // TODO(fxbug.dev/84729)
        #[allow(unused)]
        verbose: bool,
    },

    /// DEPRECATED: Use `ffx component show` instead.
    /// Output detailed information about components on the system.
    #[structopt(name = "info")]
    Info {
        /// Print information for any component whose URL/name matches this substring.
        #[structopt(short = "f", long = "filter", default_value = "")]
        // TODO(fxbug.dev/84729)
        #[allow(unused)]
        filter: String,
    },

    /// DEPRECATED: Use `ffx component select` instead.
    /// Output all components that expose a capability.
    #[structopt(name = "select")]
    Select {
        /// The capability to search for.
        #[structopt(short = "c", long = "capability", default_value = "")]
        // TODO(fxbug.dev/84729)
        #[allow(unused)]
        capability: String,
    },

    /// Display per-component statistics for syslogs.
    #[structopt(name = "logs")]
    Logs {
        /// The minimum severity to show in the log stats.
        #[structopt(long = "min-severity", default_value = "info")]
        min_severity: LogSeverity,
    },

    /// Print out page-in frequencies for all blobs in CSV format
    #[structopt(name = "freq")]
    PageInFrequencies,
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    // Visit the directory /hub and recursively traverse it, outputting information about the
    // component hierarchy. See https://fuchsia.dev/fuchsia-src/concepts/components/hub for more
    // information on the Hub directory structure.
    let opt = Opt::from_args();

    match opt {
        Opt::Logs { min_severity } => {
            let log_stats = LogStats::new(min_severity).await?;
            println!("{}", log_stats);
        }
        Opt::Info { .. } => {
            println!("'cs info' is deprecated. Please use 'ffx component show' instead!");
        }
        Opt::Select { .. } => {
            println!("'cs select' is deprecated. Please use 'ffx component select' instead!");
        }
        Opt::Tree { .. } => {
            println!("'cs tree' is deprecated. Please use 'ffx component list' instead!");
        }
        Opt::PageInFrequencies => {
            let frequencies = BlobFrequencies::collect().await;
            println!("{}", frequencies);
        }
    }
    Ok(())
}
