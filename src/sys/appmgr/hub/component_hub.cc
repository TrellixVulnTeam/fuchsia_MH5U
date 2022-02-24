// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/appmgr/hub/component_hub.h"

#include <zircon/types.h>

#include <fbl/ref_ptr.h>

#include "src/lib/storage/vfs/cpp/pseudo_dir.h"
#include "src/sys/appmgr/component_controller_impl.h"
#include "src/sys/appmgr/hub/hub.h"
#include "src/sys/appmgr/realm.h"

namespace component {

ComponentHub::ComponentHub(fbl::RefPtr<fs::PseudoDir> root) : Hub(root) {}

ComponentHub::~ComponentHub() = default;

zx_status_t ComponentHub::AddIncomingServices(fbl::RefPtr<fs::Vnode> incoming_services) {
  zx_status_t status = EnsureInDir();
  if (status != ZX_OK) {
    return status;
  }
  if (!incoming_services) {
    return ZX_ERR_INVALID_ARGS;
  }
  return in_dir_->AddEntry("svc", std::move(incoming_services));
}

zx_status_t ComponentHub::AddPackageHandle(fbl::RefPtr<fs::Vnode> package_handle) {
  zx_status_t status = EnsureInDir();
  if (status != ZX_OK) {
    return status;
  }
  if (!package_handle) {
    return ZX_ERR_INVALID_ARGS;
  }
  return in_dir_->AddEntry("pkg", std::move(package_handle));
}

zx_status_t ComponentHub::EnsureInDir() {
  if (in_dir_) {
    return ZX_OK;
  }
  in_dir_ = fbl::MakeRefCounted<fs::PseudoDir>();
  return AddEntry("in", in_dir_);
}

}  // namespace component
