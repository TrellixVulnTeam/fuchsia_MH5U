// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fidl.test.unionmemberremove/cpp/wire.h>  // nogncheck

#include <optional>
#include <string>
namespace fidl_test = fidl_test_unionmemberremove;

std::optional<int32_t> parse_as_int(const std::string& s) {
  char* end;
  long int n = strtol(s.c_str(), &end, 10);
  if (end)
    return static_cast<int32_t>(n);
  return {};
}

// [START contents]
fidl_test::wire::JsonValue writer(fidl::AnyArena& allocator, const std::string& s) {
  std::optional<int32_t> maybe_int = parse_as_int(s);
  if (maybe_int) {
    return fidl_test::wire::JsonValue::WithIntValue(*maybe_int);
  }
  return fidl_test::wire::JsonValue::WithStringValue(allocator, allocator, s);
}

std::string reader(const fidl_test::wire::JsonValue& value) {
  switch (value.Which()) {
    case fidl_test::wire::JsonValue::Tag::kIntValue:
      return std::to_string(value.int_value());
    case fidl_test::wire::JsonValue::Tag::kStringValue:
      return std::string(value.string_value().data(), value.string_value().size());
    case fidl_test::wire::JsonValue::Tag::kUnknown:
    default:
      return "<unknown>";
  }
}
// [END contents]

int main(int argc, const char** argv) { return 0; }
