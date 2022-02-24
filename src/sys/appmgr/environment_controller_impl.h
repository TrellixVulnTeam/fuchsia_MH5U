// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_APPMGR_ENVIRONMENT_CONTROLLER_IMPL_H_
#define SRC_SYS_APPMGR_ENVIRONMENT_CONTROLLER_IMPL_H_

#include <fuchsia/sys/cpp/fidl.h>
#include <lib/async/cpp/wait.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fit/function.h>

#include <memory>

#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace component {
class Realm;

class EnvironmentControllerImpl : public fuchsia::sys::EnvironmentController {
 public:
  EnvironmentControllerImpl(fidl::InterfaceRequest<fuchsia::sys::EnvironmentController> request,
                            std::unique_ptr<Realm> realm);
  ~EnvironmentControllerImpl() override;

  Realm* realm() const { return realm_.get(); }

  // fuchsia::sys::EnvironmentController implementation:

  void Kill(KillCallback callback) override;

  void Detach() override;

  void OnCreated();

 private:
  // Returns self object extracted from parent realm.
  std::unique_ptr<EnvironmentControllerImpl> ExtractEnvironmentController();

  void Handler(async_dispatcher_t* dispatcher, async::WaitBase* wait, zx_status_t status,
               const zx_packet_signal* signal);

  fidl::Binding<fuchsia::sys::EnvironmentController> binding_;

  std::unique_ptr<Realm> realm_;

  async::WaitMethod<EnvironmentControllerImpl, &EnvironmentControllerImpl::Handler> wait_;

  FXL_DISALLOW_COPY_AND_ASSIGN(EnvironmentControllerImpl);
};

}  // namespace component

#endif  // SRC_SYS_APPMGR_ENVIRONMENT_CONTROLLER_IMPL_H_
