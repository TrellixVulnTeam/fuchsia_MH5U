// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SYSROOT_ZIRCON_DEVICE_VFS_H_
#define SYSROOT_ZIRCON_DEVICE_VFS_H_

#include <zircon/types.h>

// NOTE: All the defines here with the exception of ZX_FS_RIGHTS and ZX_FS_RIGHTS_SPACE are
// mirrored from the constants in fuchsia.io, and their values must be kept in sync. These values
// are redefined here as this header is used for both Fuchsia and host builds.
//
// The FIDL definition is the source of truth:
// https://fuchsia.googlesource.com/fuchsia/+/HEAD/sdk/fidl/fuchsia.io/
// Refer to link for documentation on detailed semantics of the flags.
//
// Static assertions in sdk/lib/fdio/unistd.cc ensure that the constants below remain in alignment
// with the values defined in fuchsia.io.

// Rights
#define ZX_FS_RIGHT_READABLE 0x00000001U
#define ZX_FS_RIGHT_WRITABLE 0x00000002U
#define ZX_FS_RIGHT_EXECUTABLE 0x00000008U
// All known rights.
#define ZX_FS_RIGHTS (ZX_FS_RIGHT_READABLE | ZX_FS_RIGHT_WRITABLE | ZX_FS_RIGHT_EXECUTABLE)
// A mask for all possible rights including future extensions.
#define ZX_FS_RIGHTS_SPACE 0x0000FFFFU
// NOTE: Reserving lower 16 bits for future rights extensions. Flags should start at 0x00010000.

// Flags
#define ZX_FS_FLAG_CREATE 0x00010000U
#define ZX_FS_FLAG_EXCLUSIVE 0x00020000U
#define ZX_FS_FLAG_TRUNCATE 0x00040000U
#define ZX_FS_FLAG_DIRECTORY 0x00080000U
#define ZX_FS_FLAG_APPEND 0x00100000U
#define ZX_FS_FLAG_NOREMOTE 0x00200000U
#define ZX_FS_FLAG_VNODE_REF_ONLY 0x00400000U
#define ZX_FS_FLAG_DESCRIBE 0x00800000U
#define ZX_FS_FLAG_NOT_DIRECTORY 0x02000000U
#define ZX_FS_FLAG_CLONE_SAME_RIGHTS 0x04000000U
#define ZX_FS_FLAG_POSIX_WRITABLE 0x08000000U
#define ZX_FS_FLAG_POSIX_EXECUTABLE 0x10000000U

// Watch event messages are sent via the provided channel and take the form:
// { uint8_t event; uint8_t namelen; uint8_t name[namelen]; }
// Multiple events may arrive in one message, one after another.
// Names do not include a terminating null.
typedef struct {
  uint8_t event;
  uint8_t len;
  char name[];
} vfs_watch_msg_t;

#define VFS_TYPE_BLOBFS 0x9e694d21ul
#define VFS_TYPE_FATFS 0xce694d21ul
#define VFS_TYPE_MINFS 0x6e694d21ul
#define VFS_TYPE_MEMFS 0x3e694d21ul
#define VFS_TYPE_FACTORYFS 0x1e694d21ul
#define VFS_TYPE_FXFS 0x73667866ul
#define VFS_TYPE_F2FS 0xfe694d21ul

#endif  // SYSROOT_ZIRCON_DEVICE_VFS_H_
