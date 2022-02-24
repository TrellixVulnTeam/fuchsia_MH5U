// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lib/svc/cpp/service_namespace.h"

#include <fcntl.h>
#include <lib/async/default.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <zircon/device/vfs.h>

#include <string_view>
#include <utility>

#include <fbl/unique_fd.h>

#include "src/lib/storage/vfs/cpp/service.h"

namespace component {

ServiceNamespace::ServiceNamespace() : directory_(fbl::MakeRefCounted<fs::PseudoDir>()) {}

ServiceNamespace::ServiceNamespace(fidl::InterfaceRequest<ServiceProvider> request)
    : ServiceNamespace() {
  AddBinding(std::move(request));
}

ServiceNamespace::ServiceNamespace(fbl::RefPtr<fs::PseudoDir> directory)
    : directory_(std::move(directory)) {}

ServiceNamespace::~ServiceNamespace() = default;

void ServiceNamespace::AddBinding(fidl::InterfaceRequest<ServiceProvider> request) {
  if (request)
    bindings_.AddBinding(this, std::move(request));
}

void ServiceNamespace::Close() { bindings_.CloseAll(); }

void ServiceNamespace::AddServiceForName(ServiceConnector connector,
                                         const std::string& service_name) {
  name_to_service_connector_[service_name] = std::move(connector);
  directory_->AddEntry(service_name,
                       fbl::MakeRefCounted<fs::Service>([this, service_name](zx::channel channel) {
                         ConnectCommon(service_name, std::move(channel));
                         return ZX_OK;
                       }));
}

void ServiceNamespace::RemoveServiceForName(const std::string& service_name) {
  auto it = name_to_service_connector_.find(service_name);
  if (it != name_to_service_connector_.end())
    name_to_service_connector_.erase(it);
  directory_->RemoveEntry(service_name);
}

void ServiceNamespace::Connect(std::string_view name, zx::channel channel) {
  ConnectCommon(std::string(name.data(), name.length()), std::move(channel));
}

void ServiceNamespace::ConnectToService(std::string service_name, zx::channel channel) {
  ConnectCommon(service_name, std::move(channel));
}

void ServiceNamespace::ConnectCommon(const std::string& service_name, zx::channel channel) {
  auto it = name_to_service_connector_.find(service_name);
  if (it != name_to_service_connector_.end())
    it->second(std::move(channel));
}

}  // namespace component
