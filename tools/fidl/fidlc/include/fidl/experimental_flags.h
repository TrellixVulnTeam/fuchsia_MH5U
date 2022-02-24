// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_FIDL_FIDLC_INCLUDE_FIDL_EXPERIMENTAL_FLAGS_H_
#define TOOLS_FIDL_FIDLC_INCLUDE_FIDL_EXPERIMENTAL_FLAGS_H_

#include <map>
#include <string_view>

namespace fidl {

class ExperimentalFlags {
 public:
  using FlagSet = uint32_t;
  enum class Flag : FlagSet {
    kNewSyntaxOnly = 0b1000,
    kUnknownInteractions = 0b10000,
  };

  ExperimentalFlags() : flags_(0) {}
  ExperimentalFlags(Flag flag) : flags_(static_cast<FlagSet>(flag)) {}

  bool SetFlagByName(const std::string_view flag);
  void SetFlag(Flag flag);

  bool IsFlagEnabled(Flag flag) const;

 private:
  static std::map<const std::string_view, const Flag> FLAG_STRINGS;

  FlagSet flags_;
};

}  // namespace fidl

#endif  // TOOLS_FIDL_FIDLC_INCLUDE_FIDL_EXPERIMENTAL_FLAGS_H_
