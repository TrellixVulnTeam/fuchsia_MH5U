// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ZIRCON_SYSTEM_ULIB_ZBITL_TESTS_VMO_TESTS_H_
#define ZIRCON_SYSTEM_ULIB_ZBITL_TESTS_VMO_TESTS_H_

#include <lib/zbitl/vmo.h>

#include <memory>
#include <string>

#include <fbl/unique_fd.h>
#include <gtest/gtest.h>

struct VmoTestTraits {
  using storage_type = zx::vmo;
  using payload_type = uint64_t;
  using creation_traits = VmoTestTraits;

  static constexpr bool kDefaultConstructedViewHasStorageError = true;
  static constexpr bool kExpectExtensibility = true;
  static constexpr bool kExpectOneShotReads = false;
  static constexpr bool kExpectUnbufferedReads = true;
  static constexpr bool kExpectUnbufferedWrites = false;

  struct Context {
    storage_type TakeStorage() { return std::move(storage_); }

    storage_type storage_;
  };

  static void Create(size_t size, Context* context) {
    zx::vmo vmo;
    ASSERT_EQ(zx::vmo::create(size, ZX_VMO_RESIZABLE, &vmo), ZX_OK);
    *context = {std::move(vmo)};
  }

  static void Create(fbl::unique_fd fd, size_t size, Context* context) {
    ASSERT_TRUE(fd);
    std::unique_ptr<std::byte[]> buff{new std::byte[size]};
    EXPECT_EQ(static_cast<ssize_t>(size), read(fd.get(), buff.get(), size));
    ASSERT_NO_FATAL_FAILURE(Create(size, context));
    ASSERT_EQ(context->storage_.write(buff.get(), 0u, size), ZX_OK);
  }

  static void Read(const storage_type& storage, payload_type payload, size_t size,
                   std::string* contents) {
    contents->resize(size);
    ASSERT_EQ(ZX_OK, storage.read(contents->data(), payload, size));
  }

  static void Write(const storage_type& storage, uint32_t offset, const std::string& data) {
    ASSERT_EQ(ZX_OK, storage.write(data.data(), offset, data.size()));
  }

  static void ToPayload(const storage_type& storage, uint32_t offset, payload_type& payload) {
    payload = static_cast<payload_type>(offset);
  }

  static const zx::vmo& GetVmo(const storage_type& storage) { return storage; }
};

struct UnownedVmoTestTraits {
  using storage_type = zx::unowned_vmo;
  using payload_type = uint64_t;
  using creation_traits = VmoTestTraits;

  static constexpr bool kDefaultConstructedViewHasStorageError = true;
  static constexpr bool kExpectExtensibility = true;
  static constexpr bool kExpectOneShotReads = false;
  static constexpr bool kExpectUnbufferedReads = true;
  static constexpr bool kExpectUnbufferedWrites = false;

  struct Context {
    storage_type TakeStorage() { return std::move(storage_); }

    storage_type storage_;
    zx::vmo keepalive_;
  };

  static void Create(size_t size, Context* context) {
    typename VmoTestTraits::Context vmo_context;
    ASSERT_NO_FATAL_FAILURE(VmoTestTraits::Create(size, &vmo_context));
    context->storage_ = zx::unowned_vmo{vmo_context.storage_};
    context->keepalive_ = std::move(vmo_context.storage_);
  }

  static void Create(fbl::unique_fd fd, size_t size, Context* context) {
    typename VmoTestTraits::Context vmo_context;
    ASSERT_NO_FATAL_FAILURE(VmoTestTraits::Create(std::move(fd), size, &vmo_context));
    context->storage_ = zx::unowned_vmo{vmo_context.storage_};
    context->keepalive_ = std::move(vmo_context.storage_);
  }

  static void Read(const storage_type& storage, payload_type payload, size_t size,
                   std::string* contents) {
    ASSERT_NO_FATAL_FAILURE(VmoTestTraits::Read(*storage, payload, size, contents));
  }

  static void Write(const storage_type& storage, uint32_t offset, const std::string& data) {
    ASSERT_NO_FATAL_FAILURE(VmoTestTraits::Write(*storage, offset, data));
  }

  static void ToPayload(const storage_type& storage, uint32_t offset, payload_type& payload) {
    payload = static_cast<payload_type>(offset);
  }

  static const zx::vmo& GetVmo(const storage_type& storage) { return *storage; }
};

struct MapOwnedVmoTestTraits {
  using storage_type = zbitl::MapOwnedVmo;
  using payload_type = uint64_t;
  using creation_traits = MapOwnedVmoTestTraits;

  static constexpr bool kDefaultConstructedViewHasStorageError = true;
  static constexpr bool kExpectExtensibility = true;
  static constexpr bool kExpectOneShotReads = true;
  static constexpr bool kExpectUnbufferedReads = true;
  static constexpr bool kExpectUnbufferedWrites = true;

  struct Context {
    storage_type TakeStorage() { return std::move(storage_); }

    storage_type storage_;
  };

  static void Create(size_t size, Context* context) {
    typename VmoTestTraits::Context vmo_context;
    ASSERT_NO_FATAL_FAILURE(VmoTestTraits::Create(size, &vmo_context));
    *context = {zbitl::MapOwnedVmo{std::move(vmo_context.storage_), /*writable=*/true}};
  }

  static void Create(fbl::unique_fd fd, size_t size, Context* context) {
    typename VmoTestTraits::Context vmo_context;
    ASSERT_NO_FATAL_FAILURE(VmoTestTraits::Create(std::move(fd), size, &vmo_context));
    *context = {zbitl::MapOwnedVmo{vmo_context.TakeStorage(), /*writable=*/true}};
  }

  static void Read(const storage_type& storage, payload_type payload, size_t size,
                   std::string* contents) {
    ASSERT_NO_FATAL_FAILURE(VmoTestTraits::Read(storage.vmo(), payload, size, contents));
  }

  static void Write(const storage_type& storage, uint32_t offset, const std::string& data) {
    ASSERT_NO_FATAL_FAILURE(VmoTestTraits::Write(storage.vmo(), offset, data));
  }

  static void ToPayload(const storage_type& storage, uint32_t offset, payload_type& payload) {
    payload = static_cast<payload_type>(offset);
  }

  static const zx::vmo& GetVmo(const storage_type& storage) { return storage.vmo(); }
};

struct MapUnownedVmoTestTraits {
  using storage_type = zbitl::MapUnownedVmo;
  using payload_type = uint64_t;
  using creation_traits = MapOwnedVmoTestTraits;

  static constexpr bool kDefaultConstructedViewHasStorageError = true;
  static constexpr bool kExpectExtensibility = true;
  static constexpr bool kExpectOneShotReads = true;
  static constexpr bool kExpectUnbufferedReads = true;
  static constexpr bool kExpectUnbufferedWrites = true;

  struct Context {
    storage_type TakeStorage() { return std::move(storage_); }

    storage_type storage_;
    zx::vmo keepalive_;
  };

  static void Create(fbl::unique_fd fd, size_t size, Context* context) {
    typename UnownedVmoTestTraits::Context unowned_vmo_context;
    ASSERT_NO_FATAL_FAILURE(
        UnownedVmoTestTraits::Create(std::move(fd), size, &unowned_vmo_context));
    context->storage_ = zbitl::MapUnownedVmo{std::move(unowned_vmo_context.storage_),
                                             /*writable=*/true};
    context->keepalive_ = std::move(unowned_vmo_context.keepalive_);
  }

  static void Create(size_t size, Context* context) {
    typename UnownedVmoTestTraits::Context unowned_vmo_context;
    ASSERT_NO_FATAL_FAILURE(UnownedVmoTestTraits::Create(size, &unowned_vmo_context));
    *context = {zbitl::MapUnownedVmo{std::move(unowned_vmo_context.storage_),
                                     /*writable=*/true},
                std::move(unowned_vmo_context.keepalive_)};
  }

  static void Read(const storage_type& storage, payload_type payload, size_t size,
                   std::string* contents) {
    ASSERT_NO_FATAL_FAILURE(VmoTestTraits::Read(storage.vmo(), payload, size, contents));
  }

  static void Write(const storage_type& storage, uint32_t offset, const std::string& data) {
    ASSERT_NO_FATAL_FAILURE(VmoTestTraits::Write(storage.vmo(), offset, data));
  }

  static void ToPayload(const storage_type& storage, uint32_t offset, payload_type& payload) {
    payload = static_cast<payload_type>(offset);
  }

  static const zx::vmo& GetVmo(const storage_type& storage) { return storage.vmo(); }
};

#endif  // ZIRCON_SYSTEM_ULIB_ZBITL_TESTS_VMO_TESTS_H_
