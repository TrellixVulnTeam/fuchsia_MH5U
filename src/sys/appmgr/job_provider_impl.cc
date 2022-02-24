// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.n

#include "src/sys/appmgr/job_provider_impl.h"

#include "src/sys/appmgr/realm.h"

namespace component {

JobProviderImpl::JobProviderImpl(Realm* realm) : realm_(realm) {}

void JobProviderImpl::GetJob(GetJobCallback callback) { callback(realm_->DuplicateJobForHub()); }

void JobProviderImpl::AddBinding(fidl::InterfaceRequest<fuchsia::sys::JobProvider> request) {
  bindings_.AddBinding(this, std::move(request));
}

}  // namespace component
