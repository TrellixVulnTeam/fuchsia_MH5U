// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_PROCESS_SYMBOL_DATA_PROVIDER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_PROCESS_SYMBOL_DATA_PROVIDER_H_

#include "src/developer/debug/zxdb/symbols/symbol_data_provider.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace zxdb {

class Process;

// Implementation of SymbolDataProvider that links it to a process. It provides access to process
// memory but reports errors for all attempts to access frame-related information such as registers.
// For that, see FrameSymbolDataProvider.
class ProcessSymbolDataProvider : public SymbolDataProvider {
 public:
  // SymbolDataProvider overrides:
  debug::Arch GetArch() override;
  void GetMemoryAsync(uint64_t address, uint32_t size, GetMemoryCallback callback) override;
  void WriteMemory(uint64_t address, std::vector<uint8_t> data, WriteCallback cb) override;
  void GetTLSSegment(const SymbolContext& symbol_context, GetTLSSegmentCallback cb) override;

  std::optional<uint64_t> GetDebugAddressForContext(const SymbolContext& context) const override;

 protected:
  FRIEND_MAKE_REF_COUNTED(ProcessSymbolDataProvider);
  FRIEND_REF_COUNTED_THREAD_SAFE(ProcessSymbolDataProvider);

  explicit ProcessSymbolDataProvider(fxl::WeakPtr<Process> process);
  ~ProcessSymbolDataProvider() override;

  fxl::WeakPtr<Process>& process() { return process_; }

 private:
  fxl::WeakPtr<Process> process_;
  debug::Arch arch_;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_PROCESS_SYMBOL_DATA_PROVIDER_H_
