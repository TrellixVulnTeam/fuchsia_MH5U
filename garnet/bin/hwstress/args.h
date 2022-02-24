// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_HWSTRESS_ARGS_H_
#define GARNET_BIN_HWSTRESS_ARGS_H_

#include <lib/cmdline/args_parser.h>
#include <lib/fitx/result.h>
#include <lib/stdcompat/span.h>

#include <istream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace hwstress {

// Subcommand to run.
enum class StressTest {
  kCpu,
  kFlash,
  kLight,
  kMemory,
};

// A std::vector<uint32_t> that can be used with the args parsing library.
struct CpuCoreList {
  std::vector<uint32_t> cores;
};

// Parse a CpuCoreList.
std::istream& operator>>(std::istream& is, CpuCoreList& result);

// Parsed command line arguments.
struct CommandLineArgs {
  // The subcommand to run.
  StressTest subcommand;

  //
  // Common arguments.
  //

  // Show help.
  bool help = false;

  // Verbosity level of diagnostics.
  std::string log_level = "normal";

  // Duration in seconds.
  //
  // A value of "0" indicates forever.
  double test_duration_seconds = 0.0;

  // Amount of RAM or flash memory to test.
  cmdline::Optional<int64_t> mem_to_test_megabytes;

  //
  // Flash-specific arguments.
  //

  // Path to the Fuchsia Volume Manager
  std::string fvm_path;

  // Number of full write/read cycles to perform before finishing the test.
  uint64_t iterations = 0;

  // Destroy any existing flash test partitions.
  bool destroy_partitions = false;

  //
  // Memory-specific arguments.
  //

  // Amount of RAM to test.
  cmdline::Optional<int64_t> ram_to_test_percent;

  //
  // CPU-specific arguments.
  //

  // Target CPU utilization, as a percentage in (0.0, 100.0].
  double utilization_percent = 100.0;

  // CPU workload to use.
  std::string cpu_workload;

  // CPU cores to stress.
  CpuCoreList cores_to_test;

  //
  // LED-specific arguments.
  //

  // Amount of time the light should be on/off during LED tests.
  double light_on_time_seconds = 0.5;
  double light_off_time_seconds = 0.5;
};

// Print usage information to stdout.
void PrintUsage();

// Parse args, returning failure or the parsed arguments.
fitx::result<std::string, CommandLineArgs> ParseArgs(cpp20::span<const char* const> args);

}  // namespace hwstress

#endif  // GARNET_BIN_HWSTRESS_ARGS_H_
