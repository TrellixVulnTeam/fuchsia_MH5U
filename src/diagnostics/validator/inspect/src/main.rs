// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod data; // Inspect data maintainer/updater/scanner-from-vmo; compare engine
mod metrics; // Evaluates memory performance of Inspect library
mod puppet; // Interface to target Inspect library wrapper programs (puppets)
mod results; // Stores and formats reports-to-user
mod runner; // Coordinates testing operations
mod trials; // Defines the trials to run

use {
    anyhow::{format_err, Error},
    argh::FromArgs,
    fidl_test_inspect_validate as validate,
    serde::Serialize,
    std::str::FromStr,
};

/// Validate Inspect VMO formats written by 'puppet' programs controlled by
/// this Validator program and exercising Inspect library implementations.
//#[derive(StructOpt, Debug)]
#[derive(Debug, FromArgs)]
struct Opt {
    /// report results in a pretty human-readable format. Without this flag,
    /// results will be printed as JSON.
    #[argh(option, long = "output", default = "OutputType::Json")]
    output: OutputType,

    /// when trees differ, render 'full' text, 'diff' type difference, or 'both'.
    #[argh(option, long = "difftype", default = "DiffType::Full")]
    diff_type: DiffType,

    /// required arg(s): The URL(s) of the puppet(s).
    #[argh(option, long = "url")]
    puppet_urls: Vec<String>,

    /// has no effect.
    #[argh(switch, long = "quiet")]
    // TODO(fxbug.dev/84729)
    #[allow(unused)]
    quiet: bool,

    #[argh(switch, long = "verbose")]
    /// has no effect.
    // TODO(fxbug.dev/84729)
    #[allow(unused)]
    verbose: bool,

    #[argh(switch, long = "version", short = 'v')]
    /// prints the version information and exits.
    version: bool,

    #[argh(switch)]
    /// tests that the Archive FIDL service output matches the expected values.
    test_archive: bool,
}

#[derive(Debug)]
enum OutputType {
    Text,
    Json,
}

impl FromStr for OutputType {
    type Err = Error;
    fn from_str(s: &str) -> Result<OutputType, Error> {
        Ok(match s {
            "text" => OutputType::Text,
            "json" => OutputType::Json,
            _ => return Err(format_err!("Output type must be 'text' or 'json'")),
        })
    }
}

/// When reporting a discrepancy between local and remote Data trees, should the output include:
/// - The full rendering of both trees?
/// - The condensed diff between the trees? (This may still be quite large.)
/// - Both full and condensed renderings?
#[derive(Clone, Copy, Debug, Serialize)]
pub enum DiffType {
    Full,
    Diff,
    Both,
}

impl FromStr for DiffType {
    type Err = Error;
    fn from_str(s: &str) -> Result<DiffType, Error> {
        Ok(match s {
            "full" => DiffType::Full,
            "diff" => DiffType::Diff,
            "both" => DiffType::Both,
            _ => return Err(format_err!("Diff type must be 'full' or 'diff' or 'both'")),
        })
    }
}

#[fuchsia::component]
async fn main() -> Result<(), Error> {
    let mut results = results::Results::new();
    let Opt { output, puppet_urls, version, diff_type, test_archive, .. } = argh::from_env();
    if version {
        println!("Inspect Validator version 0.9. See README.md for more information.");
        return Ok(());
    }
    results.diff_type = diff_type;
    results.test_archive = test_archive;

    run_all_puppets(puppet_urls, &mut results).await;
    match output {
        OutputType::Text => results.print_pretty_text(),
        OutputType::Json => println!("{}", results.to_json()),
    }
    if results.failed() {
        return Err(format_err!("A test failed"));
    } else {
        Ok(())
    }
}

async fn run_all_puppets(urls: Vec<String>, results: &mut results::Results) {
    if urls.len() == 0 {
        results.error("At least one component URL is required.".to_string());
    }
    for url in urls {
        runner::run_all_trials(&url, results).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[fuchsia::test]
    async fn url_is_required() {
        let mut results = results::Results::new();
        run_all_puppets(vec![], &mut results).await;
        assert!(results.failed());
        assert!(results.to_json().contains("At least one"));
    }

    #[fuchsia::test]
    async fn bad_url_fails() {
        let mut results = results::Results::new();
        run_all_puppets(vec!["a".to_owned()], &mut results).await;
        assert!(results.failed());
        assert!(results.to_json().contains("URL may be invalid"), "{}", results.to_json());
    }

    #[fuchsia::test]
    async fn all_urls_are_tried() {
        let mut results = results::Results::new();
        run_all_puppets(vec!["a".to_owned(), "b".to_owned()], &mut results).await;
        assert!(results.to_json().contains("invalid: a"));
        assert!(results.to_json().contains("invalid: b"));
    }

    // The only way to test success is to actually start a component, and that's
    // not suitable for unit tests. Failure on a valid URL will be caught in integration
    // tests.
}
