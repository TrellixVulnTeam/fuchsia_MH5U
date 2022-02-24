// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_ZBITL_ERROR_STDIO_H_
#define LIB_ZBITL_ERROR_STDIO_H_

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#include <string_view>

// The format of the error messages below should be kept in sync with that of
// the returned strings in error_string.h.

namespace zbitl {

// Prints an error message from a View `Error` value, where Printer is a
// callable type with a printf-like signature.
template <typename ViewError, typename Printer>
inline void PrintViewError(const ViewError& error, Printer&& printer) {
  printer("%.*s at offset %" PRIu32, static_cast<int>(error.zbi_error.size()),
          error.zbi_error.data(), error.item_offset);
  if (error.storage_error) {
    auto storage_error = ViewError::storage_error_string(error.storage_error.value());
    std::string_view storage_error_sv = storage_error;
    printer(": %.*s", static_cast<int>(storage_error_sv.size()), storage_error_sv.data());
  }
  printer("\n");  // To flush the buffer.
}

template <typename ViewError>
inline void PrintViewError(const ViewError& error, FILE* f = stdout) {
  PrintViewError(error, [f](const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
  });
}

// Prints an error message from a View `CopyError` value, where Printer is a
// callable type with a printf-like signature.
template <typename ViewCopyError, typename Printer>
inline void PrintViewCopyError(const ViewCopyError& error, Printer&& printer) {
  printer("%.*s", static_cast<int>(error.zbi_error.size()), error.zbi_error.data());
  if (error.read_error) {
    auto read_error = ViewCopyError::read_error_string(error.read_error.value());
    std::string_view read_error_sv = read_error;
    printer(": read error at source offset %" PRIu32 ": %.*s", error.read_offset,
            static_cast<int>(read_error_sv.size()), read_error_sv.data());
  } else if (error.write_error) {
    auto write_error = ViewCopyError::write_error_string(error.write_error.value());
    std::string_view write_error_sv = write_error;
    printer(": write error at destination offset %" PRIu32 ": %.*s", error.write_offset,
            static_cast<int>(write_error_sv.size()), write_error_sv.data());
  }
  printer("\n");  // To flush the buffer.
}

template <typename ViewCopyError>
inline void PrintViewCopyError(const ViewCopyError& error, FILE* f = stdout) {
  PrintViewCopyError(error, [f](const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
  });
}

// Prints an error message from a BootfsView `Error` value, where Printer is a
// callable type with a printf-like signature.
template <typename BootfsError, typename Printer>
inline void PrintBootfsError(const BootfsError& error, Printer&& printer) {
  printer("%.*s", static_cast<int>(error.reason.size()), error.reason.data());
  if (error.entry_offset > 0) {
    printer(": at dirent offset %" PRIu32, error.entry_offset);
  }
  if (!error.filename.empty()) {
    printer(": with filename \"%.*s\"", static_cast<int>(error.filename.size()),
            error.filename.data());
  }
  if (error.storage_error) {
    auto storage_error = BootfsError::storage_error_string(error.storage_error.value());
    std::string_view storage_error_sv = storage_error;
    printer(": %.*s", static_cast<int>(storage_error_sv.size()), storage_error_sv.data());
  }
  printer("\n");  // To flush the buffer.
}

template <typename BootfsError>
inline void PrintBootfsError(const BootfsError& error, FILE* f = stdout) {
  PrintBootfsError(error, [f](const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
  });
}

}  // namespace zbitl

#endif  // LIB_ZBITL_ERROR_STDIO_H_
