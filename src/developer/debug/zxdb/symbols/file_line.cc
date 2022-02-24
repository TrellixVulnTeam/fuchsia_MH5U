// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/file_line.h"

#include <lib/syslog/cpp/macros.h>

#include <tuple>

namespace zxdb {

FileLine::FileLine() = default;

FileLine::FileLine(std::string file, int line) : FileLine(std::move(file), std::string(), line) {}

FileLine::FileLine(std::string file, std::string comp_dir, int line)
    : file_(std::move(file)), comp_dir_(std::move(comp_dir)), line_(line) {
  // For "line 0" entries there should be no file or compilation directory set. These entries
  // correspond to no code. Having a compilation directory or file name set in these cases will
  // confuse FileLine comparison operations since ideally "no code" should always compare as equal
  // to "no code".
  FX_DCHECK(line_ > 0 || (file_.empty() && comp_dir_.empty()));
}

FileLine::~FileLine() = default;

bool operator<(const FileLine& a, const FileLine& b) {
  return std::make_tuple(a.line(), a.file(), a.comp_dir()) <
         std::make_tuple(b.line(), b.file(), b.comp_dir());
}

bool operator==(const FileLine& a, const FileLine& b) {
  // See comment above decl, the compilation directory is deliberately omitted.
  return a.line() == b.line() && a.file() == b.file();
}

bool operator!=(const FileLine& a, const FileLine& b) { return !operator==(a, b); }

}  // namespace zxdb
