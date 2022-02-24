// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define _ALL_SOURCE  // Expose the MTX_INIT extension.

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/zxio/null.h>
#include <lib/zxio/ops.h>
#include <sys/stat.h>
#include <threads.h>

#include "private.h"

namespace fio = fuchsia_io;

using zxio_vmofile_t = struct zxio_vmofile {
  // The |zxio_t| control structure for this object.
  zxio_t io;

  // The underlying VMO that stores the data.
  zx::vmo vmo;

  // The start of content within the VMO.
  //
  // This value is never changed.
  const zx_off_t start;

  // The size of the file in bytes.
  const zx_off_t size;

  // The current seek offset within the file.
  zx_off_t offset __TA_GUARDED(lock);

  mtx_t lock;

  fidl::WireSyncClient<fio::File> control;
};

static_assert(sizeof(zxio_vmofile_t) <= sizeof(zxio_storage_t),
              "zxio_vmofile_t must fit inside zxio_storage_t.");

static zx_status_t zxio_vmofile_close(zxio_t* io) {
  auto file = reinterpret_cast<zxio_vmofile_t*>(io);
  zx_status_t status = file->control->Close().status();
  file->~zxio_vmofile_t();
  return status;
}

static zx_status_t zxio_vmofile_release(zxio_t* io, zx_handle_t* out_handle) {
  auto file = reinterpret_cast<zxio_vmofile_t*>(io);

  mtx_lock(&file->lock);
  uint64_t offset = file->offset;
  mtx_unlock(&file->lock);

  const fidl::WireResult result = file->control->Seek(fio::wire::SeekOrigin::kStart, offset);
  if (!result.ok()) {
    return ZX_ERR_BAD_STATE;
  }
  const auto& response = result.value();
  switch (response.result.Which()) {
    case fio::wire::File2SeekResult::Tag::kErr:
      return ZX_ERR_BAD_STATE;
    case fio::wire::File2SeekResult::Tag::kResponse:
      *out_handle = file->control.TakeClientEnd().TakeChannel().release();
      return ZX_OK;
  }
}

static zx_status_t zxio_vmofile_reopen(zxio_t* io, zxio_reopen_flags_t zxio_flags,
                                       zx_handle_t* out_handle) {
  auto file = reinterpret_cast<zxio_vmofile_t*>(io);
  zx::status ends = fidl::CreateEndpoints<fio::Node>();
  if (ends.is_error()) {
    return ends.status_value();
  }
  uint32_t flags = fio::wire::kCloneFlagSameRights;
  if (zxio_flags & ZXIO_REOPEN_DESCRIBE) {
    flags |= fio::wire::kOpenFlagDescribe;
  }
  const fidl::WireResult result = file->control->Clone(flags, std::move(ends->server));
  if (!result.ok()) {
    return result.status();
  }
  *out_handle = ends->client.TakeChannel().release();
  return ZX_OK;
}

static zx_status_t zxio_vmofile_attr_get(zxio_t* io, zxio_node_attributes_t* out_attr) {
  auto file = reinterpret_cast<zxio_vmofile_t*>(io);
  *out_attr = {};
  ZXIO_NODE_ATTR_SET(*out_attr, protocols, ZXIO_NODE_PROTOCOL_FILE | ZXIO_NODE_PROTOCOL_MEMORY);
  ZXIO_NODE_ATTR_SET(*out_attr, abilities,
                     ZXIO_OPERATION_READ_BYTES | ZXIO_OPERATION_GET_ATTRIBUTES);
  ZXIO_NODE_ATTR_SET(*out_attr, content_size, file->size);
  return ZX_OK;
}

static zx_status_t zxio_vmofile_readv(zxio_t* io, const zx_iovec_t* vector, size_t vector_count,
                                      zxio_flags_t flags, size_t* out_actual) {
  if (flags) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  auto file = reinterpret_cast<zxio_vmofile_t*>(io);

  mtx_lock(&file->lock);
  zx_status_t status =
      zxio_vmo_do_vector(file->start, file->size, &file->offset, vector, vector_count, out_actual,
                         [&](void* buffer, zx_off_t offset, size_t capacity) {
                           return file->vmo.read(buffer, offset, capacity);
                         });
  mtx_unlock(&file->lock);
  return status;
}

static zx_status_t zxio_vmofile_readv_at(zxio_t* io, zx_off_t offset, const zx_iovec_t* vector,
                                         size_t vector_count, zxio_flags_t flags,
                                         size_t* out_actual) {
  if (flags) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  auto file = reinterpret_cast<zxio_vmofile_t*>(io);

  return zxio_vmo_do_vector(file->start, file->size, &offset, vector, vector_count, out_actual,
                            [&](void* buffer, zx_off_t offset, size_t capacity) {
                              return file->vmo.read(buffer, offset, capacity);
                            });
}

static zx_status_t zxio_vmofile_seek(zxio_t* io, zxio_seek_origin_t start, int64_t offset,
                                     size_t* out_offset) {
  auto file = reinterpret_cast<zxio_vmofile_t*>(io);

  mtx_lock(&file->lock);
  zx_off_t origin;
  switch (start) {
    case ZXIO_SEEK_ORIGIN_START:
      origin = 0;
      break;
    case ZXIO_SEEK_ORIGIN_CURRENT:
      origin = file->offset;
      break;
    case ZXIO_SEEK_ORIGIN_END:
      origin = file->size;
      break;
    default:
      mtx_unlock(&file->lock);
      return ZX_ERR_INVALID_ARGS;
  }
  zx_off_t at;
  if (add_overflow(origin, offset, &at)) {
    mtx_unlock(&file->lock);
    return ZX_ERR_INVALID_ARGS;
  }
  if (at > file->size) {
    mtx_unlock(&file->lock);
    return ZX_ERR_INVALID_ARGS;
  }
  file->offset = at;
  mtx_unlock(&file->lock);

  *out_offset = at;
  return ZX_OK;
}

zx_status_t zxio_vmo_get_common(const zx::vmo& vmo, size_t content_size, uint32_t flags,
                                zx_handle_t* out_vmo, size_t* out_size) {
  if (out_vmo == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }

  // Ensure that we return a VMO handle with only the rights requested by the
  // client. For Vmofiles, the server side does not ever see the VMO_FLAG_*
  // options from the client because the VMO is returned in NodeInfo/Vmofile
  // rather than from a File.GetBuffer call.

  zx_rights_t rights = ZX_RIGHTS_BASIC | ZX_RIGHT_MAP | ZX_RIGHT_GET_PROPERTY;
  rights |= flags & fio::wire::kVmoFlagRead ? ZX_RIGHT_READ : 0;
  rights |= flags & fio::wire::kVmoFlagWrite ? ZX_RIGHT_WRITE : 0;
  rights |= flags & fio::wire::kVmoFlagExec ? ZX_RIGHT_EXECUTE : 0;

  if (flags & fio::wire::kVmoFlagPrivate) {
    // Allow SET_PROPERTY only if creating a private child VMO so that the user
    // can set ZX_PROP_NAME (or similar).
    rights |= ZX_RIGHT_SET_PROPERTY;

    uint32_t options = ZX_VMO_CHILD_SNAPSHOT_AT_LEAST_ON_WRITE;
    if (flags & fio::wire::kVmoFlagExec) {
      // Creating a SNAPSHOT_AT_LEAST_ON_WRITE child removes ZX_RIGHT_EXECUTE even if the
      // parent VMO has it, and we can't arbitrarily add EXECUTE here on the
      // client side. Adding CHILD_NO_WRITE still creates a snapshot and a new
      // VMO object, which e.g. can have a unique ZX_PROP_NAME value, but the
      // returned handle lacks WRITE and maintains EXECUTE.
      if (flags & fio::wire::kVmoFlagWrite) {
        return ZX_ERR_NOT_SUPPORTED;
      }
      options |= ZX_VMO_CHILD_NO_WRITE;
    }

    zx::vmo child_vmo;
    zx_status_t status = vmo.create_child(options, 0u, content_size, &child_vmo);
    if (status != ZX_OK) {
      return status;
    }

    // SNAPSHOT_AT_LEAST_ON_WRITE adds ZX_RIGHT_WRITE automatically, but we shouldn't return
    // a handle with that right unless requested using VMO_FLAG_WRITE.
    // TODO(fxbug.dev/36877): Supporting VMO_FLAG_PRIVATE & VMO_FLAG_WRITE for
    // Vmofiles is a bit weird and inconsistent. See bug for more info.
    zx::vmo result;
    status = child_vmo.replace(rights, &result);
    if (status != ZX_OK) {
      return status;
    }
    *out_vmo = result.release();
    if (out_size) {
      *out_size = content_size;
    }
    return ZX_OK;
  }

  // For !VMO_FLAG_PRIVATE (including VMO_FLAG_EXACT), we just duplicate another
  // handle to the Vmofile's VMO with appropriately scoped rights.
  zx::vmo result;
  zx_status_t status = vmo.duplicate(rights, &result);
  if (status != ZX_OK) {
    return status;
  }
  *out_vmo = result.release();
  if (out_size) {
    *out_size = content_size;
  }
  return ZX_OK;
}

static zx_status_t zxio_vmofile_vmo_get(zxio_t* io, uint32_t flags, zx_handle_t* out_vmo,
                                        size_t* out_size) {
  // Can't support Vmofiles with a non-zero start/offset, because we return just
  // a VMO with no other data - like a starting offset - to the user.
  // (Technically we could support any page aligned offset, but that's currently
  // unneeded.)
  auto file = reinterpret_cast<zxio_vmofile_t*>(io);
  if (file->start != 0) {
    return ZX_ERR_NOT_FOUND;
  }

  return zxio_vmo_get_common(file->vmo, file->size, flags, out_vmo, out_size);
}

static constexpr zxio_ops_t zxio_vmofile_ops = []() {
  zxio_ops_t ops = zxio_default_ops;
  ops.close = zxio_vmofile_close;
  ops.release = zxio_vmofile_release;
  ops.reopen = zxio_vmofile_reopen;
  ops.attr_get = zxio_vmofile_attr_get;
  ops.readv = zxio_vmofile_readv;
  ops.readv_at = zxio_vmofile_readv_at;
  ops.seek = zxio_vmofile_seek;
  ops.vmo_get = zxio_vmofile_vmo_get;
  return ops;
}();

zx_status_t zxio_vmofile_init(zxio_storage_t* storage, fidl::WireSyncClient<fio::File> control,
                              zx::vmo vmo, zx_off_t offset, zx_off_t length, zx_off_t seek) {
  auto file = new (storage) zxio_vmofile_t{
      .io = storage->io,
      .vmo = std::move(vmo),
      .start = offset,
      .size = length,
      .offset = std::min(seek, length),
      .lock = MTX_INIT,
      .control = std::move(control),
  };
  zxio_init(&file->io, &zxio_vmofile_ops);
  return ZX_OK;
}
