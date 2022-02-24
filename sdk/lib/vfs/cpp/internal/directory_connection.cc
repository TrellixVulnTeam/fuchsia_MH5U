// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/vfs/cpp/flags.h>
#include <lib/vfs/cpp/internal/directory.h>
#include <lib/vfs/cpp/internal/directory_connection.h>

#include <utility>

namespace vfs {
namespace internal {

DirectoryConnection::DirectoryConnection(uint32_t flags, vfs::internal::Directory* vn)
    : Connection(flags), vn_(vn), binding_(this) {}

DirectoryConnection::~DirectoryConnection() = default;

zx_status_t DirectoryConnection::BindInternal(zx::channel request, async_dispatcher_t* dispatcher) {
  if (binding_.is_bound()) {
    return ZX_ERR_BAD_STATE;
  }
  zx_status_t status = binding_.Bind(std::move(request), dispatcher);
  if (status != ZX_OK) {
    return status;
  }
  binding_.set_error_handler([this](zx_status_t status) { vn_->Close(this); });
  return ZX_OK;
}

void DirectoryConnection::AdvisoryLock(fuchsia::io::AdvisoryLockRequest request,
                                       AdvisoryLockCallback callback) {
  callback(fuchsia::io::AdvisoryLocking_AdvisoryLock_Result::WithErr(ZX_ERR_NOT_SUPPORTED));
}

void DirectoryConnection::Clone(uint32_t flags, fidl::InterfaceRequest<fuchsia::io::Node> object) {
  Connection::Clone(vn_, flags, object.TakeChannel(), binding_.dispatcher());
}

void DirectoryConnection::CloseDeprecated(CloseDeprecatedCallback callback) {
  Connection::CloseDeprecated(vn_, std::move(callback));
}

void DirectoryConnection::Close(CloseCallback callback) {
  Connection::Close(vn_, std::move(callback));
}

void DirectoryConnection::Describe(DescribeCallback callback) {
  Connection::Describe(vn_, std::move(callback));
}

void DirectoryConnection::Describe2(fuchsia::io::ConnectionInfoQuery query,
                                    Describe2Callback callback) {
  Connection::Describe2(vn_, std::move(query), std::move(callback));
}

void DirectoryConnection::SyncDeprecated(SyncDeprecatedCallback callback) {
  Connection::SyncDeprecated(vn_, std::move(callback));
}

void DirectoryConnection::Sync(SyncCallback callback) {
  Connection::Sync(vn_, std::move(callback));
}

void DirectoryConnection::GetAttr(GetAttrCallback callback) {
  Connection::GetAttr(vn_, std::move(callback));
}

void DirectoryConnection::SetAttr(uint32_t flags, fuchsia::io::NodeAttributes attributes,
                                  SetAttrCallback callback) {
  Connection::SetAttr(vn_, flags, attributes, std::move(callback));
}

void DirectoryConnection::Open(uint32_t flags, uint32_t mode, std::string path,
                               fidl::InterfaceRequest<fuchsia::io::Node> object) {
  vn_->Open(flags, this->flags(), mode, path.data(), path.length(), object.TakeChannel(),
            binding_.dispatcher());
}

void DirectoryConnection::Unlink(std::string name, fuchsia::io::UnlinkOptions options,
                                 UnlinkCallback callback) {
  callback(fpromise::error(ZX_ERR_NOT_SUPPORTED));
}

void DirectoryConnection::ReadDirents(uint64_t max_bytes, ReadDirentsCallback callback) {
  uint64_t new_offset = 0, out_bytes = 0;
  std::vector<uint8_t> vec(max_bytes);
  zx_status_t status = vn_->Readdir(offset(), vec.data(), max_bytes, &new_offset, &out_bytes);
  ZX_DEBUG_ASSERT(out_bytes <= max_bytes);
  vec.resize(out_bytes);
  if (status == ZX_OK) {
    set_offset(new_offset);
  }
  callback(status, std::move(vec));
}

void DirectoryConnection::Rewind(RewindCallback callback) {
  set_offset(0);
  callback(ZX_OK);
}

void DirectoryConnection::GetToken(GetTokenCallback callback) {
  callback(ZX_ERR_NOT_SUPPORTED, zx::handle());
}

void DirectoryConnection::Rename(std::string src, zx::event dst_parent_token, std::string dst,
                                 RenameCallback callback) {
  callback(fpromise::error(ZX_ERR_NOT_SUPPORTED));
}

void DirectoryConnection::Link(std::string src, zx::handle dst_parent_token, std::string dst,
                               LinkCallback callback) {
  callback(ZX_ERR_NOT_SUPPORTED);
}

void DirectoryConnection::Watch(uint32_t mask, uint32_t options, zx::channel watcher,
                                WatchCallback callback) {
  // TODO: Implement watch.
  callback(ZX_ERR_NOT_SUPPORTED);
}

void DirectoryConnection::SendOnOpenEvent(zx_status_t status) {
  binding_.events().OnOpen(status, NodeInfoIfStatusOk(vn_, status));
}

void DirectoryConnection::GetFlags(GetFlagsCallback callback) {
  callback(ZX_OK, this->flags() & (Flags::kStatusFlags | Flags::kFsRights));
}

void DirectoryConnection::SetFlags(uint32_t flags, SetFlagsCallback callback) {
  callback(ZX_ERR_NOT_SUPPORTED);
}

}  // namespace internal
}  // namespace vfs
