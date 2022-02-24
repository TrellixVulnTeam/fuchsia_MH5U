// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_ULIB_ZBITL_TESTS_STDIO_TESTS_H_
#define ZIRCON_SYSTEM_ULIB_ZBITL_TESTS_STDIO_TESTS_H_

#include <lib/zbitl/stdio.h>

#include "src/lib/files/scoped_temp_dir.h"
#include "tests.h"

struct StdioTestTraits {
  using storage_type = FILE*;
  using payload_type = long int;

  static constexpr bool kExpectExtensibility = true;
  static constexpr bool kExpectOneShotReads = false;
  static constexpr bool kExpectUnbufferedReads = true;
  static constexpr bool kExpectUnbufferedWrites = false;

  struct Context {
    ~Context() {
      if (storage_) {
        fclose(storage_);
      }
    }

    storage_type TakeStorage() const { return storage_; }

    storage_type storage_ = nullptr;
    files::ScopedTempDir dir_;
  };

  static void Create(size_t size, Context* context) {
    std::string filename;
    ASSERT_TRUE(context->dir_.NewTempFile(&filename));
    FILE* f = fopen(filename.c_str(), "r+");
    ASSERT_NE(f, nullptr) << "failed to open " << filename << ": " << strerror(errno);
    if (size > 0) {
      fseek(f, static_cast<long int>(size) - 1, SEEK_SET);
      putc(0, f);
    }
    context->storage_ = f;
    ASSERT_FALSE(ferror(f));
  }

  static void Create(fbl::unique_fd fd, size_t size, Context* context) {
    ASSERT_TRUE(fd);
    FILE* f = fdopen(fd.release(), "r+");
    ASSERT_NE(f, nullptr) << "failed to open descriptor: " << strerror(errno);
    context->storage_ = f;
  }

  static void Read(storage_type storage, payload_type payload, size_t size, Bytes* contents) {
    contents->resize(size);
    ASSERT_EQ(0, fseek(storage, payload, SEEK_SET))
        << "failed to seek to payload: " << strerror(errno);
    size_t n = fread(contents->data(), 1, size, storage);
    ASSERT_EQ(0, ferror(storage)) << "failed to read payload: " << strerror(errno);
    ASSERT_EQ(size, n) << "did not fully read payload";
  }

  static void Write(storage_type storage, uint32_t offset, const Bytes& data) {
    ASSERT_EQ(0, fseek(storage, offset, SEEK_SET))
        << "failed to seek to offset " << offset << ": " << strerror(errno);
    ssize_t n = fwrite(data.data(), 1, data.size(), storage);
    ASSERT_GE(n, 0) << "write: " << strerror(errno);
    ASSERT_EQ(data.size(), static_cast<size_t>(n)) << "did not fully write data";
  }

  static void ToPayload(storage_type storage, uint32_t offset, payload_type& payload) {
    payload = static_cast<payload_type>(offset);
  }
};

#endif  // ZIRCON_SYSTEM_ULIB_ZBITL_TESTS_STDIO_TESTS_H_
