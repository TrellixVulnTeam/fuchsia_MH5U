// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/debug_agent/elf_utils.h"

// clang-format off
// Included early because of conflicts.
#include "src/lib/elflib/elflib.h"
// clang-format on

#include <link.h>

#include <string>

#include "src/developer/debug/debug_agent/process_handle.h"
#include "src/developer/debug/ipc/records.h"

namespace debug_agent {

namespace {

// Reads a null-terminated string from the given address of the given process.
debug::Status ReadNullTerminatedString(const ProcessHandle& process, zx_vaddr_t vaddr,
                                       std::string* dest) {
  // Max size of string we'll load as a sanity check.
  constexpr size_t kMaxString = 32768;

  dest->clear();

  constexpr size_t kBlockSize = 256;
  char block[kBlockSize];
  while (dest->size() < kMaxString) {
    size_t num_read = 0;
    if (auto status = process.ReadMemory(vaddr, block, kBlockSize, &num_read); status.has_error())
      return status;

    for (size_t i = 0; i < num_read; i++) {
      if (block[i] == 0)
        return debug::Status();
      dest->push_back(block[i]);
    }

    if (num_read < kBlockSize)
      return debug::Status();  // Partial read: hit the mapped memory boundary.
    vaddr += kBlockSize;
  }
  return debug::Status();
}

// Returns the fetch function for use by ElfLib for the given process. The ProcessHandle must
// outlive the returned value.
std::function<bool(uint64_t, std::vector<uint8_t>*)> GetElfLibReader(const ProcessHandle& process,
                                                                     uint64_t load_address) {
  return [&process, load_address](uint64_t offset, std::vector<uint8_t>* buf) {
    size_t num_read = 0;
    if (process.ReadMemory(load_address + offset, buf->data(), buf->size(), &num_read).has_error())
      return false;
    return num_read == buf->size();
  };
}

}  // namespace

debug::Status WalkElfModules(const ProcessHandle& process, uint64_t dl_debug_addr,
                             std::function<bool(uint64_t base_addr, uint64_t lmap)> cb) {
  size_t num_read = 0;
  uint64_t lmap = 0;
  if (auto status = process.ReadMemory(dl_debug_addr + offsetof(r_debug, r_map), &lmap,
                                       sizeof(lmap), &num_read);
      status.has_error())
    return status;

  size_t module_count = 0;

  // Walk the linked list.
  constexpr size_t kMaxObjects = 512;  // Sanity threshold.
  while (lmap != 0) {
    if (module_count++ >= kMaxObjects)
      return debug::Status("Too many modules, memory likely corrupted.");

    uint64_t base;
    if (process.ReadMemory(lmap + offsetof(link_map, l_addr), &base, sizeof(base), &num_read)
            .has_error())
      break;

    uint64_t next;
    if (process.ReadMemory(lmap + offsetof(link_map, l_next), &next, sizeof(next), &num_read)
            .has_error())
      break;

    if (!cb(base, lmap))
      break;

    lmap = next;
  }

  return debug::Status();
}

std::vector<debug_ipc::Module> GetElfModulesForProcess(const ProcessHandle& process,
                                                       uint64_t dl_debug_addr) {
  std::vector<debug_ipc::Module> modules;
  WalkElfModules(process, dl_debug_addr, [&process, &modules](uint64_t base, uint64_t lmap) {
    debug_ipc::Module module;
    module.base = base;
    module.debug_address = lmap;

    uint64_t str_addr;
    size_t num_read;
    if (process
            .ReadMemory(lmap + offsetof(link_map, l_name), &str_addr, sizeof(str_addr), &num_read)
            .has_error())
      return false;

    if (ReadNullTerminatedString(process, str_addr, &module.name).has_error())
      return false;

    if (auto elf = elflib::ElfLib::Create(GetElfLibReader(process, module.base)))
      module.build_id = elf->GetGNUBuildID();

    modules.push_back(std::move(module));
    return true;
  });
  return modules;
}

// The dynamic loader puts the address of the code it calls after changing the shared library
// lists in r_debug.r_brk where the dl_debug_addr points to the r_debug structure.
uint64_t GetLoaderBreakpointAddress(const ProcessHandle& process, uint64_t dl_debug_addr) {
  size_t num_read = 0;
  uint64_t rbrk = 0;
  if (process.ReadMemory(dl_debug_addr + offsetof(r_debug, r_brk), &rbrk, sizeof(rbrk), &num_read)
          .ok() &&
      num_read == sizeof(rbrk))
    return rbrk;
  return 0;
}

}  // namespace debug_agent
