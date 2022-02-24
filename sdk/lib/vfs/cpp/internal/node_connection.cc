// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/vfs/cpp/flags.h>
#include <lib/vfs/cpp/internal/node.h>
#include <lib/vfs/cpp/internal/node_connection.h>

#include <utility>

namespace vfs {
namespace internal {

NodeConnection::NodeConnection(uint32_t flags, vfs::internal::Node* vn)
    : Connection(flags), vn_(vn), binding_(this) {}

NodeConnection::~NodeConnection() = default;

zx_status_t NodeConnection::BindInternal(zx::channel request, async_dispatcher_t* dispatcher) {
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

void NodeConnection::Clone(uint32_t flags, fidl::InterfaceRequest<fuchsia::io::Node> object) {
  Connection::Clone(vn_, flags, object.TakeChannel(), binding_.dispatcher());
}

void NodeConnection::CloseDeprecated(CloseDeprecatedCallback callback) {
  Connection::CloseDeprecated(vn_, std::move(callback));
}

void NodeConnection::Close(CloseCallback callback) { Connection::Close(vn_, std::move(callback)); }

void NodeConnection::Describe(DescribeCallback callback) {
  Connection::Describe(vn_, std::move(callback));
}

void NodeConnection::Describe2(fuchsia::io::ConnectionInfoQuery query, Describe2Callback callback) {
  Connection::Describe2(vn_, std::move(query), std::move(callback));
}

void NodeConnection::SyncDeprecated(SyncDeprecatedCallback callback) {
  Connection::SyncDeprecated(vn_, std::move(callback));
}

void NodeConnection::Sync(SyncCallback callback) { Connection::Sync(vn_, std::move(callback)); }

void NodeConnection::GetAttr(GetAttrCallback callback) {
  Connection::GetAttr(vn_, std::move(callback));
}

void NodeConnection::SetAttr(uint32_t flags, fuchsia::io::NodeAttributes attributes,
                             SetAttrCallback callback) {
  Connection::SetAttr(vn_, flags, attributes, std::move(callback));
}

void NodeConnection::SendOnOpenEvent(zx_status_t status) {
  binding_.events().OnOpen(status, NodeInfoIfStatusOk(vn_, status));
}

void NodeConnection::GetFlags(GetFlagsCallback callback) {
  callback(ZX_OK, this->flags() & (Flags::kStatusFlags | Flags::kFsRights));
}

void NodeConnection::SetFlags(uint32_t flags, SetFlagsCallback callback) {
  callback(ZX_ERR_NOT_SUPPORTED);
}

void NodeConnection::QueryFilesystem(QueryFilesystemCallback callback) {
  callback(ZX_ERR_NOT_SUPPORTED, nullptr);
}

}  // namespace internal
}  // namespace vfs
