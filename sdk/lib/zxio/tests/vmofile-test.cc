// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zx/channel.h>
#include <lib/zx/vmo.h>
#include <lib/zxio/cpp/inception.h>
#include <lib/zxio/zxio.h>

#include <zxtest/zxtest.h>

#include "sdk/lib/zxio/private.h"

constexpr const char* ALPHABET = "abcdefghijklmnopqrstuvwxyz";

class VmoFileNonZeroOffsetTest : public zxtest::Test {
 public:
  void SetUp() override {
    ASSERT_OK(zx::vmo::create(300u, 0u, &backing));
    ASSERT_OK(backing.write(ALPHABET, 0, len));
    ASSERT_OK(backing.write(ALPHABET, len, len + len));
    zx::status client_end = fidl::CreateEndpoints(&server_);
    ASSERT_OK(client_end.status_value());

    ASSERT_OK(zxio_vmofile_init(&storage, fidl::BindSyncClient(std::move(client_end).value()),
                                std::move(backing), /* offset */ 4, /* length */ len,
                                /* seek */ 3));
    io = &storage.io;
  }

  void TearDown() override {
    server_.reset();
    ASSERT_STATUS(ZX_ERR_PEER_CLOSED, zxio_close(io));
  }

 protected:
  zx::vmo backing;
  size_t len = strlen(ALPHABET);
  fidl::ServerEnd<fuchsia_io::File> server_;
  zxio_storage_t storage;
  zxio_t* io;
};

TEST_F(VmoFileNonZeroOffsetTest, Basic) {
  zxio_signals_t observed = ZXIO_SIGNAL_NONE;
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED,
                zxio_wait_one(io, ZXIO_SIGNAL_READABLE, ZX_TIME_INFINITE, &observed));

  zx::channel clone;
  ASSERT_OK(zxio_clone(io, clone.reset_and_get_address()));
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_sync(io));

  zxio_node_attributes_t attr = {};
  ASSERT_OK(zxio_attr_get(io, &attr));
  EXPECT_EQ(len, attr.content_size);
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_attr_set(io, &attr));

  char buffer[1024];
  memset(buffer, 0, sizeof(buffer));
  size_t actual = 0u;
  ASSERT_OK(zxio_read(io, buffer, 8, 0, &actual));
  EXPECT_EQ(actual, 8);
  EXPECT_STREQ("hijklmno", buffer);
  memset(buffer, 0, sizeof(buffer));
  actual = 0u;
  ASSERT_OK(zxio_read_at(io, 1u, buffer, 6, 0, &actual));
  EXPECT_EQ(actual, 6);
  EXPECT_STREQ("fghijk", buffer);
  ASSERT_STATUS(ZX_ERR_WRONG_TYPE, zxio_write(io, buffer, sizeof(buffer), 0, &actual));
  ASSERT_STATUS(ZX_ERR_WRONG_TYPE, zxio_write_at(io, 0u, buffer, sizeof(buffer), 0, &actual));
  size_t offset = 2u;
  ASSERT_OK(zxio_seek(io, ZXIO_SEEK_ORIGIN_START, 2, &offset));
  EXPECT_EQ(offset, 2u);
  memset(buffer, 0, sizeof(buffer));
  actual = 0u;
  ASSERT_OK(zxio_read(io, buffer, 3, 0, &actual));
  EXPECT_STREQ("ghi", buffer);
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_truncate(io, 0u));
  uint32_t flags = 0u;
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_flags_get(io, &flags));
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_flags_set(io, flags));

  constexpr std::string_view name("hello");
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED,
                zxio_open_async(io, 0u, 0u, name.data(), name.length(), ZX_HANDLE_INVALID));
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED,
                zxio_add_inotify_filter(io, name.data(), name.length(), 0u, 0, ZX_HANDLE_INVALID));
  ASSERT_STATUS(ZX_ERR_NOT_SUPPORTED, zxio_unlink(io, name.data(), name.length(), 0));
}

TEST_F(VmoFileNonZeroOffsetTest, GetCopy) {
  zx::vmo vmo;
  size_t size = 0u;
  ASSERT_OK(zxio_vmo_get_copy(io, vmo.reset_and_get_address(), &size));
  EXPECT_NE(vmo, ZX_HANDLE_INVALID);
  EXPECT_EQ(size, len);
}

TEST_F(VmoFileNonZeroOffsetTest, GetClone) {
  zx::vmo vmo;
  size_t size = 0u;
  // Not found because this vmo-file has a non-zero starting offset.
  ASSERT_STATUS(ZX_ERR_NOT_FOUND, zxio_vmo_get_clone(io, vmo.reset_and_get_address(), &size));
  EXPECT_EQ(vmo, ZX_HANDLE_INVALID);
}

TEST_F(VmoFileNonZeroOffsetTest, GetExact) {
  zx::vmo vmo;
  size_t size = 0u;
  // Not found because this vmo-file has a non-zero starting offset.
  ASSERT_STATUS(ZX_ERR_NOT_FOUND, zxio_vmo_get_exact(io, vmo.reset_and_get_address(), &size));
}

TEST(VmoFileTest, GetExact) {
  zx::vmo backing;
  ASSERT_OK(zx::vmo::create(300u, 0u, &backing));
  size_t len = strlen(ALPHABET);
  ASSERT_OK(backing.write(ALPHABET, 0, len));
  ASSERT_OK(backing.write(ALPHABET, len, len + len));

  zx::status ends = fidl::CreateEndpoints<fuchsia_io::File>();
  ASSERT_OK(ends.status_value());

  zxio_storage_t storage;
  ASSERT_OK(zxio_vmofile_init(&storage, fidl::BindSyncClient(std::move(ends->client)),
                              std::move(backing), /* offset */ 0, /* length */ len, /* seek */ 3));
  zxio_t* io = &storage.io;

  zx::vmo vmo;
  size_t size = 0u;
  ASSERT_OK(zxio_vmo_get_exact(io, vmo.reset_and_get_address(), &size));
  EXPECT_NE(vmo, ZX_HANDLE_INVALID);
  uint8_t dest[4];
  ASSERT_OK(vmo.read(dest, 0, 4));
  ASSERT_BYTES_EQ(ALPHABET, dest, 4);

  ends->server.reset();
  ASSERT_STATUS(ZX_ERR_PEER_CLOSED, zxio_close(io));
}
