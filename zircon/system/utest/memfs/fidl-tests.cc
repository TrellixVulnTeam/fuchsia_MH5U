// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/memfs/memfs.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/device/vfs.h>
#include <zircon/processargs.h>
#include <zircon/syscalls.h>

#include <utility>

#include <fbl/unique_fd.h>
#include <zxtest/zxtest.h>

namespace {

namespace fio = fuchsia_io;

TEST(FidlTests, TestFidlBasic) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  ASSERT_EQ(loop.StartThread(), ZX_OK);

  memfs_filesystem_t* fs;
  ASSERT_EQ(memfs_install_at(loop.dispatcher(), "/fidltmp", &fs), ZX_OK);
  fbl::unique_fd fd(open("/fidltmp", O_DIRECTORY | O_RDONLY));
  ASSERT_GE(fd.get(), 0);

  // Create a file
  const char* filename = "file-a";
  fd.reset(openat(fd.get(), filename, O_CREAT | O_RDWR));
  ASSERT_GE(fd.get(), 0);
  const char* data = "hello";
  ssize_t datalen = strlen(data);
  ASSERT_EQ(write(fd.get(), data, datalen), datalen);
  fd.reset();

  zx_handle_t h, request;
  ASSERT_EQ(zx_channel_create(0, &h, &request), ZX_OK);
  ASSERT_EQ(fdio_service_connect("/fidltmp/file-a", request), ZX_OK);

  auto describe_result = fidl::WireCall<fio::File>(zx::unowned_channel(h))->Describe();
  ASSERT_EQ(describe_result.status(), ZX_OK);
  ASSERT_TRUE(describe_result.Unwrap()->info.is_file());
  ASSERT_EQ(describe_result.Unwrap()->info.file().event.get(), ZX_HANDLE_INVALID);
  zx_handle_close(h);

  sync_completion_t unmounted;
  memfs_free_filesystem(fs, &unmounted);
  sync_completion_wait(&unmounted, zx::duration::infinite().get());

  loop.Shutdown();
}

TEST(FidlTests, TestFidlOpenReadOnly) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  ASSERT_EQ(loop.StartThread(), ZX_OK);

  memfs_filesystem_t* fs;
  ASSERT_EQ(memfs_install_at(loop.dispatcher(), "/fidltmp-ro", &fs), ZX_OK);
  fbl::unique_fd fd(open("/fidltmp-ro", O_DIRECTORY | O_RDONLY));
  ASSERT_GE(fd.get(), 0);

  // Create a file
  const char* filename = "file-ro";
  fd.reset(openat(fd.get(), filename, O_CREAT | O_RDWR));
  ASSERT_GE(fd.get(), 0);
  fd.reset();

  zx_handle_t h, request;
  ASSERT_EQ(zx_channel_create(0, &h, &request), ZX_OK);
  ASSERT_EQ(fdio_open("/fidltmp-ro/file-ro", ZX_FS_RIGHT_READABLE, request), ZX_OK);

  auto result = fidl::WireCall<fio::File>(zx::unowned_channel(h))->GetFlags();
  ASSERT_EQ(result.status(), ZX_OK);
  ASSERT_EQ(result.Unwrap()->s, ZX_OK);
  ASSERT_EQ(result.Unwrap()->flags, ZX_FS_RIGHT_READABLE);
  zx_handle_close(h);

  sync_completion_t unmounted;
  memfs_free_filesystem(fs, &unmounted);
  sync_completion_wait(&unmounted, zx::duration::infinite().get());

  loop.Shutdown();
}

void QueryInfo(const char* path, fuchsia_io::wire::FilesystemInfo* info) {
  fbl::unique_fd fd(open(path, O_RDONLY | O_DIRECTORY));
  ASSERT_TRUE(fd);
  fdio_cpp::FdioCaller caller(std::move(fd));
  auto result = fidl::WireCall<fuchsia_io::Directory>(caller.channel())->QueryFilesystem();
  ASSERT_EQ(result.status(), ZX_OK);
  ASSERT_EQ(result.Unwrap()->s, ZX_OK);
  ASSERT_NOT_NULL(result.Unwrap()->info);
  *info = *(result.Unwrap()->info);
  const char* kFsName = "memfs";
  const char* name = reinterpret_cast<const char*>(info->name.data());
  ASSERT_EQ(strncmp(name, kFsName, strlen(kFsName)), 0, "Unexpected filesystem mounted");
  ASSERT_EQ(info->block_size, ZX_PAGE_SIZE);
  ASSERT_EQ(info->max_filename_size, NAME_MAX);
  ASSERT_EQ(info->fs_type, VFS_TYPE_MEMFS);
  ASSERT_NE(info->fs_id, 0);
  ASSERT_EQ(info->used_bytes % info->block_size, 0);
}

TEST(FidlTests, TestFidlQueryFilesystem) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);
  ASSERT_EQ(loop.StartThread(), ZX_OK);

  memfs_filesystem_t* fs;
  ASSERT_EQ(memfs_install_at(loop.dispatcher(), "/fidltmp-basic", &fs), ZX_OK);
  fbl::unique_fd fd(open("/fidltmp-basic", O_DIRECTORY | O_RDONLY));
  ASSERT_GE(fd.get(), 0);

  // Sanity checks
  fuchsia_io::wire::FilesystemInfo info;
  ASSERT_NO_FATAL_FAILURE(QueryInfo("/fidltmp-basic", &info));

  // These values are nonsense, but they're the nonsense we expect memfs to generate.
  ASSERT_EQ(info.total_bytes, UINT64_MAX);
  ASSERT_EQ(info.used_bytes, 0);

  sync_completion_t unmounted;
  memfs_free_filesystem(fs, &unmounted);
  sync_completion_wait(&unmounted, zx::duration::infinite().get());

  loop.Shutdown();
}

}  // namespace
