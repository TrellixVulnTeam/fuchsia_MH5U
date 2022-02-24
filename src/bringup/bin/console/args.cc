// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/bringup/bin/console/args.h"

#include <lib/cmdline/args_parser.h>

#include <algorithm>
#include <unordered_set>

#include "src/lib/fxl/strings/split_string.h"

namespace {

zx::status<Options> GetBootArguments(const fidl::WireSyncClient<fuchsia_boot::Arguments>& client) {
  Options ret;

  fidl::StringView vars[]{
      fidl::StringView{"console.allowed_log_tags"},
      fidl::StringView{"console.denied_log_tags"},
  };
  auto resp = client->GetStrings(fidl::VectorView<fidl::StringView>::FromExternal(vars));
  if (!resp.ok()) {
    printf("console: failed to get boot args: %s\n", zx_status_get_string(resp.status()));
    return zx::error(resp.status());
  }
  if (resp->values.count() != 2) {
    printf("console: boot args returned incorrect number of results: %zd\n", resp->values.count());
    return zx::error(ZX_ERR_INTERNAL);
  }

  auto parse_tags = [](const fidl::StringView tags) -> std::vector<std::string> {
    if (!tags.is_null()) {
      return fxl::SplitStringCopy(tags.get(), ",", fxl::kTrimWhitespace, fxl::kSplitWantNonEmpty);
    }
    return {};
  };
  return zx::ok(Options{
      .allowed_log_tags = parse_tags(resp->values[0]),
      .denied_log_tags = parse_tags(resp->values[1]),
  });
}

template <typename T>
void RemoveIntersection(std::vector<T>& first, const std::vector<T>& second) {
  std::unordered_set second1(second.begin(), second.end());
  first.erase(std::remove_if(first.begin(), first.end(),
                             [&](auto member) { return second1.find(member) != second1.end(); }),
              first.end());
}

}  // namespace

zx_status_t ParseArgs(int argc, const char** argv,
                      const fidl::WireSyncClient<fuchsia_boot::Arguments>& client, Options* opts) {
  cmdline::ArgsParser<Options> parser;
  parser.AddSwitch("allow-log-tag", 'a',
                   "Add a tag to the allow list. Log entries with matching tags will be output to "
                   "the console. If no tags are specified, all log entries will be printed.",
                   &Options::allowed_log_tags);
  parser.AddSwitch("deny-log-tag", 'd',
                   "Add a tag to the deny list. Log entries with matching tags will be prevented "
                   "from being output to the console. This takes precedence over the allow list.",
                   &Options::denied_log_tags);
  std::vector<std::string> params;
  auto status = parser.Parse(argc, argv, opts, &params);
  if (status.has_error()) {
    printf("console: ArgsParser::Parse() = %s\n", status.error_message().data());
    return ZX_ERR_INVALID_ARGS;
  }

  zx::status<Options> boot_args = GetBootArguments(client);
  if (boot_args.is_error()) {
    return boot_args.status_value();
  }
  // Boot arguments take precedence.
  RemoveIntersection(opts->denied_log_tags, boot_args->allowed_log_tags);

  auto& tags = boot_args->allowed_log_tags;
  std::copy(tags.begin(), tags.end(), std::back_inserter(opts->allowed_log_tags));
  tags = boot_args->denied_log_tags;
  std::copy(tags.begin(), tags.end(), std::back_inserter(opts->denied_log_tags));
  return ZX_OK;
}
