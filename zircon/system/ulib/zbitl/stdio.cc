// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/cksum.h>
#include <lib/zbitl/stdio.h>

#include <cerrno>
#include <cstdio>
#include <memory>
#include <new>

namespace zbitl {
namespace {

constexpr size_t kBufferSize = BUFSIZ;

using error_type = StorageTraits<FILE*>::error_type;

}  // namespace

fitx::result<error_type, uint32_t> StorageTraits<FILE*>::Capacity(FILE* f) {
  if (fseek(f, 0, SEEK_END)) {
    return fitx::error{errno};
  }
  long int eof = ftell(f);
  return fitx::ok(static_cast<uint32_t>(
      std::min(static_cast<long int>(std::numeric_limits<uint32_t>::max()), eof)));
}

fitx::result<error_type> StorageTraits<FILE*>::EnsureCapacity(FILE* f, uint32_t capacity_bytes) {
  if (fseek(f, 0, SEEK_END)) {
    return fitx::error{errno};
  }
  long int eof = ftell(f);
  if (eof < 0) {
    return fitx::error{errno};
  }
  uint32_t current = static_cast<uint32_t>(eof);
  if (current >= capacity_bytes) {
    return fitx::ok();  // Current capacity is sufficient.
  }

  // Write a single byte to reserve enough space for the new capacity.
  if (fseek(f, capacity_bytes - current - 1, SEEK_END)) {
    return fitx::error{errno};
  }
  if (putc(0, f) == EOF) {
    return fitx::error{errno};
  }
  return fitx::ok();
}

fitx::result<error_type> StorageTraits<FILE*>::Read(FILE* f, payload_type offset, void* buffer,
                                                    uint32_t length) {
  if (fseek(f, offset, SEEK_SET) != 0) {
    return fitx::error{errno};
  }
  if (fread(buffer, 1, length, f) != length) {
    return fitx::error{ferror(f) ? errno : ESPIPE};
  }
  return fitx::ok();
}

fitx::result<error_type> StorageTraits<FILE*>::DoRead(FILE* f, payload_type offset, uint32_t length,
                                                      bool (*cb)(void*, ByteView), void* arg) {
  if (length == 0) {
    cb(arg, {});
    return fitx::ok();
  }

  if (fseek(f, offset, SEEK_SET) != 0) {
    return fitx::error{errno};
  }

  auto size = [&]() { return std::min(static_cast<size_t>(length), kBufferSize); };
  std::unique_ptr<std::byte[]> buf{new std::byte[size()]};

  while (length > 0) {
    size_t n = fread(buf.get(), 1, size(), f);
    if (n == 0) {
      return fitx::error{ferror(f) ? errno : ESPIPE};
    }
    ZX_ASSERT(n <= kBufferSize);
    if (!cb(arg, {buf.get(), n})) {
      break;
    }
    length -= static_cast<uint32_t>(n);
  }

  return fitx::ok();
}

fitx::result<error_type> StorageTraits<FILE*>::Write(FILE* f, uint32_t offset, ByteView data) {
  if (fseek(f, offset, SEEK_SET) != 0) {
    return fitx::error{errno};
  }

  while (!data.empty()) {
    size_t n = fwrite(data.data(), 1, data.size(), f);
    if (n == 0) {
      return fitx::error{ferror(f) ? errno : ESPIPE};
    }
    ZX_ASSERT(n <= data.size());
    offset += static_cast<uint32_t>(n);
    data = data.subspan(n);
  }
  return fitx::ok();
}

}  // namespace zbitl
