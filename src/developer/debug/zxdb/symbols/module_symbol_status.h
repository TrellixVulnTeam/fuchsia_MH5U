// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_MODULE_SYMBOL_STATUS_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_MODULE_SYMBOL_STATUS_H_

#include <stdint.h>

#include <string>

namespace zxdb {

class LoadedModuleSymbols;

struct ModuleSymbolStatus {
  // Name of the executable or shared library on the system.
  std::string name;

  // Build ID extracted from file.
  std::string build_id;

  // Load address.
  uint64_t base = 0;

  // True if the symbols were successfully loaded.
  bool symbols_loaded = false;

  size_t functions_indexed = 0;
  size_t files_indexed = 0;

  // Local file name with the symbols if the symbols were loaded.
  std::string symbol_file;

  // Represents a handle to the actual symbols. nullptr if the symbols are not loaded.
  LoadedModuleSymbols* symbols = nullptr;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_MODULE_SYMBOL_STATUS_H_
