// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fit/defer.h>

#include <iostream>

#include "src/developer/debug/zxdb/common/curl.h"
#include "src/developer/debug/zxdb/common/version.h"
#include "src/lib/fxl/strings/trim.h"
#include "tools/symbol-index/analytics.h"
#include "tools/symbol-index/command_line_options.h"
#include "tools/symbol-index/symbol_index.h"

namespace symbol_index {

int Main(int argc, const char* argv[]) {
  using ::analytics::core_dev_tools::EarlyProcessAnalyticsOptions;

  zxdb::Curl::GlobalInit();
  auto deferred_cleanup_curl = fit::defer(zxdb::Curl::GlobalCleanup);
  auto deferred_cleanup_analytics = fit::defer(Analytics::CleanUp);

  CommandLineOptions options;

  if (const Error error = ParseCommandLine(argc, argv, &options); !error.empty()) {
    // Sometimes the error just has too many "\n" at the end.
    std::cerr << fxl::TrimString(error, "\n") << std::endl;
    return EXIT_FAILURE;
  }

  if (options.requested_version) {
    std::cout << "Version: " << zxdb::kBuildVersion << std::endl;
    return EXIT_SUCCESS;
  }

  if (EarlyProcessAnalyticsOptions<Analytics>(options.analytics, options.analytics_show)) {
    return 0;
  }
  Analytics::InitBotAware(options.analytics);
  Analytics::IfEnabledSendInvokeEvent();

  SymbolIndex symbol_index(options.symbol_index_file);

  if (const Error error = symbol_index.Load(); !error.empty()) {
    std::cerr << error << std::endl;
    return EXIT_FAILURE;
  }

  switch (options.verb) {
    case CommandLineOptions::Verb::kList:
      for (const auto& entry : symbol_index.entries())
        std::cout << entry.ToString() << std::endl;
      break;
    case CommandLineOptions::Verb::kAdd:
      if (options.params.size() == 1)
        options.params.push_back("");
      symbol_index.Add(options.params[0], options.params[1]);
      break;
    case CommandLineOptions::Verb::kAddAll:
      if (options.params.size() == 0)
        options.params.push_back("");
      if (auto err = symbol_index.AddAll(options.params[0]); !err.empty())
        std::cerr << err << std::endl;
      break;
    case CommandLineOptions::Verb::kRemove:
      symbol_index.Remove(options.params[0]);
      break;
    case CommandLineOptions::Verb::kPurge:
      for (const auto& entry : symbol_index.Purge())
        std::cerr << "Purged " << entry.ToString() << std::endl;
      break;
  }

  if (options.verb != CommandLineOptions::Verb::kList) {
    if (const Error error = symbol_index.Save(); !error.empty()) {
      std::cerr << error << std::endl;
      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}

}  // namespace symbol_index

int main(int argc, const char* argv[]) { return symbol_index::Main(argc, argv); }
