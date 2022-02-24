// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/fdio/directory.h>
#include <lib/fidl/llcpp/array.h>
#include <lib/fidl/llcpp/connect_service.h>
#include <lib/fpromise/result.h>
#include <lib/service/llcpp/service.h>
#include <zircon/device/vfs.h>

namespace service {

zx::status<fidl::ClientEnd<fuchsia_io::Directory>> OpenServiceRoot(const char* path) {
  return ::service::Connect<fuchsia_io::Directory>(path);
}

namespace internal {

zx::status<zx::channel> ConnectRaw(const char* path) {
  zx::channel client_end, server_end;
  if (zx_status_t status = zx::channel::create(0, &client_end, &server_end); status != ZX_OK) {
    return zx::error(status);
  }
  if (zx::status<> status = ConnectRaw(std::move(server_end), path); status.is_error()) {
    return status.take_error();
  }
  return zx::ok(std::move(client_end));
}

zx::status<> ConnectRaw(zx::channel server_end, const char* path) {
  if (zx_status_t status = fdio_service_connect(path, server_end.release()); status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok();
}

zx::status<zx::channel> ConnectAtRaw(fidl::UnownedClientEnd<fuchsia_io::Directory> svc_dir,
                                     const char* protocol_name) {
  zx::channel client_end, server_end;
  if (zx_status_t status = zx::channel::create(0, &client_end, &server_end); status != ZX_OK) {
    return zx::error(status);
  }
  if (zx::status<> status = ConnectAtRaw(svc_dir, std::move(server_end), protocol_name);
      status.is_error()) {
    return status.take_error();
  }
  return zx::ok(std::move(client_end));
}

zx::status<> ConnectAtRaw(fidl::UnownedClientEnd<fuchsia_io::Directory> svc_dir,
                          zx::channel server_end, const char* protocol_name) {
  if (zx_status_t status =
          fdio_service_connect_at(svc_dir.handle()->get(), protocol_name, server_end.release());
      status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok();
}

zx::status<zx::channel> CloneRaw(zx::unowned_channel&& node) {
  zx::channel client_end, server_end;
  zx_status_t status = zx::channel::create(0, &client_end, &server_end);
  if (status != ZX_OK) {
    return zx::error(status);
  }
  if (zx::status<> status = CloneRaw(std::move(node), std::move(server_end)); status.is_error()) {
    return status.take_error();
  }
  return zx::ok(std::move(client_end));
}

zx::status<> CloneRaw(zx::unowned_channel&& node, zx::channel server_end) {
  if (zx_status_t status = fdio_service_clone_to(node->get(), server_end.release());
      status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok();
}

}  // namespace internal

}  // namespace service

namespace service {

namespace {

constexpr uint64_t kMaxFilename = fuchsia_io::wire::kMaxFilename;

// Max path length will be two path components, separated by a file separator.
constexpr uint64_t kMaxPath = (2 * kMaxFilename) + 1;

zx::status<fidl::StringView> ValidateAndJoinPath(fidl::Array<char, kMaxPath>* buffer,
                                                 fidl::StringView service,
                                                 fidl::StringView instance) {
  if (service.empty() || service.size() > kMaxFilename) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }
  if (instance.size() > kMaxFilename) {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }
  if (service[0] == '/') {
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  const uint64_t path_size = service.size() + instance.size() + 1;
  ZX_ASSERT(path_size <= kMaxPath);

  char* path_cursor = buffer->data();
  memcpy(path_cursor, service.data(), service.size());
  path_cursor += service.size();
  *path_cursor++ = '/';
  memcpy(path_cursor, instance.data(), instance.size());
  return zx::ok(fidl::StringView::FromExternal(buffer->data(), path_size));
}

}  // namespace

namespace internal {

zx::status<> DirectoryOpenFunc(zx::unowned_channel dir, fidl::StringView path, zx::channel remote) {
  constexpr uint32_t flags =
      fuchsia_io::wire::kOpenRightReadable | fuchsia_io::wire::kOpenRightWritable;
  fidl::UnownedClientEnd<fuchsia_io::Directory> dir_end(dir);
  fidl::ServerEnd<fuchsia_io::Node> node_end(std::move(remote));
  fidl::WireResult<fuchsia_io::Directory::Open> result =
      fidl::WireCall<fuchsia_io::Directory>(dir_end)->Open(flags, 0755u, path, std::move(node_end));
  return zx::make_status(result.status());
}

}  // namespace internal

zx::status<> OpenNamedServiceAt(fidl::UnownedClientEnd<fuchsia_io::Directory> dir,
                                cpp17::string_view service, cpp17::string_view instance,
                                zx::channel remote) {
  fidl::Array<char, kMaxPath> path_buffer;
  zx::status<fidl::StringView> path_result =
      ValidateAndJoinPath(&path_buffer, fidl::StringView::FromExternal(service),
                          fidl::StringView::FromExternal(instance));
  if (!path_result.is_ok()) {
    return path_result.take_error();
  }
  return internal::DirectoryOpenFunc(dir.channel(), std::move(path_result.value()),
                                     std::move(remote));
}

}  // namespace service
