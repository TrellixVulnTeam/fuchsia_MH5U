// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_APPMGR_JOB_PROVIDER_IMPL_H_
#define SRC_SYS_APPMGR_JOB_PROVIDER_IMPL_H_

#include <fuchsia/sys/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>

#include <fbl/ref_counted.h>

#include "src/lib/fxl/macros.h"

namespace component {

class Realm;

// An implementation of |JobProvider|, which implements a method to return
// a realm's job handle.
class JobProviderImpl : public fuchsia::sys::JobProvider, public fbl::RefCounted<JobProviderImpl> {
 public:
  // Constructs a job provider which will return the job of the given realm.
  explicit JobProviderImpl(Realm* realm);

  void GetJob(GetJobCallback callback) override;

  void AddBinding(fidl::InterfaceRequest<fuchsia::sys::JobProvider> request);

 private:
  fidl::BindingSet<fuchsia::sys::JobProvider> bindings_;
  Realm* const realm_;  // Not owned.

  FXL_DISALLOW_COPY_AND_ASSIGN(JobProviderImpl);
};

}  // namespace component

#endif  // SRC_SYS_APPMGR_JOB_PROVIDER_IMPL_H_
