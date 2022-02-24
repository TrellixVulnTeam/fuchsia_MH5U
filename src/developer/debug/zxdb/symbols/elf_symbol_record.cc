// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/elf_symbol_record.h"

#include "llvm/Demangle/Demangle.h"

namespace zxdb {

ElfSymbolRecord::ElfSymbolRecord(ElfSymbolType t, uint64_t relative_address, uint64_t size,
                                 const std::string& linkage_name)
    : type(t), relative_address(relative_address), size(size), linkage_name(linkage_name) {
  unmangled_name = llvm::demangle(linkage_name);
}

}  // namespace zxdb
