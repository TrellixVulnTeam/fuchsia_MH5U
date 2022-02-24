// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {argh::FromArgs, diagnostics_data::Severity, ffx_core::ffx_command};

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "test",
    description = "Run test suite",
    note = "Run tests or inspect output from a previous test run."
)]
pub struct TestCommand {
    #[argh(subcommand)]
    pub subcommand: TestSubcommand,
}

#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand)]
pub enum TestSubcommand {
    Run(RunCommand),
    List(ListCommand),
    Result(ResultCommand),
}

#[derive(FromArgs, Debug, PartialEq, Clone)]
#[argh(
    subcommand,
    name = "run",
    description = "Entry point for executing tests",
    note = "Runs test suites implementing the `fuchsia.test.Suite` protocol.

Tests are specified either as a command line argument, or via JSON file.

For example, when specifying a test via command line:
    ffx test run fuchsia-pkg://fuchsia.com/my_test#meta/my_test.cm
When specifying a test via command line and passing arguments to the test:
    ffx test run fuchsia-pkg://fuchsia.com/my_test#meta/my_test.cm -- arg1 arg2

Specifying tests via JSON file is currently unstable, but may be done with the
--test-file option.

Note that if running multiple iterations of a test and an iteration times
out, no further iterations will be executed."
)]
pub struct RunCommand {
    /// test timeout in seconds
    #[argh(option, short = 't')]
    pub timeout: Option<u32>,

    /// test url, and any arguments passed to tests, following `--`.
    /// When --test-file is specified test_args should not be specified.
    #[argh(positional)]
    pub test_args: Vec<String>,

    // TODO(satsukiu): once stable, document the format
    /// read test url and options from the specified file instead of from the command line.
    /// Stdin may be specified by passing `--test-file -`.
    /// May not be used in conjunction with `test_args`, `--test-file`, `--test-filter`,
    /// `--run-disabled`, `--parallel`, `--max-severity-logs`
    /// This option is currently unstable and the format of the file is subject to change.
    #[argh(option)]
    pub test_file: Option<String>,

    /// test filter. Glob pattern for matching tests. Can be
    /// specified multiple times to pass in multiple patterns.
    /// Tests may be excluded by prepending a '-' to the glob pattern.
    /// example: --test-filter glob1 --test-filter glob2 --test-filter -glob3.
    #[argh(option)]
    pub test_filter: Vec<String>,

    /// whether to also run tests that have been marked disabled/ignored
    /// by the test author.
    #[argh(switch)]
    pub run_disabled: bool,

    /// whether to filter ANSI escape sequences from output
    #[argh(switch)]
    pub filter_ansi: bool,

    /// continue running unfinished suites if a suite times out.
    /// By default, unfinished suites are immediately terminated if a suite times out.
    /// This option is only relevant when multiple suites are run.
    #[argh(switch)]
    pub continue_on_timeout: bool,

    /// stop running unfinished suites after the number of provided failures has occurred.
    /// By default, all suites are run to completion if a suite fails.
    #[argh(option)]
    pub stop_after_failures: Option<u16>,

    /// run tests in parallel, up to the number provided.
    #[argh(option)]
    pub parallel: Option<u16>,

    /// number of times to run the test. By default run 1 time. If an iteration
    /// of the test times out, no further iterations are executed.
    #[argh(option)]
    pub count: Option<u16>,

    /// when set, only logs with a severity equal to the given one or higher will be printed.
    #[argh(option)]
    pub min_severity_logs: Option<Severity>,

    /// when set, the test will fail if any log with a higher severity is emitted.
    #[argh(option)]
    pub max_severity_logs: Option<Severity>,

    /// when set, output test results to the specified directory.
    #[argh(option)]
    pub output_directory: Option<String>,

    /// when set, disables structured output to a directory.
    #[argh(switch)]
    pub disable_output_directory: bool,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "list-cases",
    description = "List test suite cases",
    note = "Lists the set of test cases available in a test suite"
)]
pub struct ListCommand {
    /// test url
    #[argh(positional)]
    pub test_url: String,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "result",
    description = "Manage test results",
    note = "Inspect and manage the results from previous test runs"
)]
pub struct ResultCommand {
    #[argh(subcommand)]
    pub subcommand: ResultSubCommand,
}

#[derive(FromArgs, PartialEq, Debug)]
#[argh(subcommand)]
pub enum ResultSubCommand {
    Show(ShowResultCommand),
    List(ListResultCommand),
    Delete(DeleteResultCommand),
    Save(SaveResultCommand),
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "show",
    description = "Display test results",
    note = "Display the results of a previous test run.

When no options are provided, displays the results of the most recent test run.
"
)]
pub struct ShowResultCommand {
    /// when set, display the results of the specified directory.
    #[argh(option)]
    pub directory: Option<String>,
    /// when set, display the results of a run with specified index.
    #[argh(option)]
    pub index: Option<u32>,
    /// when set, display the results of a run with specified name.
    #[argh(option)]
    pub name: Option<String>,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "list",
    description = "List test results",
    note = "Display a list of previous test runs"
)]
pub struct ListResultCommand {}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "delete",
    description = "Delete test results",
    note = "Manually delete a previous test run result.

Either --index or --name must be specified."
)]
pub struct DeleteResultCommand {
    /// when set, display the results of a run with specified index.
    #[argh(option)]
    pub index: Option<u32>,
    /// when set, display the results of a run with specified name.
    #[argh(option)]
    pub name: Option<String>,
}

#[derive(FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "save",
    description = "Save test results",
    note = "Mark a test result so it is not garbage collected.

By default, ffx test only retains the last 'n' test results, as configured
with 'test.save_count'. A test result saved and given a name using the save
command will be exempted from this cleanup and will not be counted towards the
total number of saved test results."
)]
pub struct SaveResultCommand {
    /// the index of the test result to save.
    #[argh(option)]
    pub index: u32,
    /// the name to assign the saved test result.
    #[argh(option)]
    pub name: String,
}
