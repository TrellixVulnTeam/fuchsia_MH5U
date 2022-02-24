// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_BIN_SESSIONMGR_STORY_RUNNER_MODULE_CONTEXT_IMPL_H_
#define SRC_MODULAR_BIN_SESSIONMGR_STORY_RUNNER_MODULE_CONTEXT_IMPL_H_

#include <fuchsia/modular/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_handle.h>
#include <lib/fidl/cpp/interface_request.h>

#include <string>

#include "src/lib/fxl/macros.h"
#include "src/modular/bin/sessionmgr/component_context_impl.h"
#include "src/modular/bin/sessionmgr/puppet_master/command_runners/operation_calls/add_mod_call.h"
#include "src/modular/lib/deprecated_service_provider/service_provider_impl.h"
#include "src/modular/lib/fidl/environment.h"

namespace modular {

class StoryControllerImpl;

// The dependencies of ModuleContextImpl common to all instances.
struct ModuleContextInfo {
  const ComponentContextInfo component_context_info;
  StoryControllerImpl* const story_controller_impl;
  Environment* session_environment;
};

// ModuleContextImpl keeps a single connection from a module instance in
// the story to a StoryControllerImpl. This way, requests that the module makes
// on its Story handle can be associated with the Module instance.
class ModuleContextImpl : fuchsia::modular::ModuleContext {
 public:
  // |module_data| identifies this particular module instance using the path of
  // modules that have ended up starting this module in the module_path
  // property. The last item in this list is this module's name. |module_path|
  // can be used to internally name resources that belong to this module
  // (message queues, Links).
  ModuleContextImpl(const ModuleContextInfo& info, fuchsia::modular::ModuleData module_data,
                    fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> service_provider_request);

  ~ModuleContextImpl() override;

 private:
  // |fuchsia::modular::ModuleContext|
  void RemoveSelfFromStory() override;

  // Identifies the module by its path, holds the URL of the running module, and
  // the link it was started with.
  fuchsia::modular::ModuleData module_data_;

  // Not owned. The StoryControllerImpl for the Story in which this Module lives.
  StoryControllerImpl* const story_controller_impl_;

  // The session environment
  Environment* session_environment_;

  ComponentContextImpl component_context_impl_;

  fidl::BindingSet<fuchsia::modular::ModuleContext> bindings_;

  // A service provider that represents the services to be added into an application's namespace.
  component::ServiceProviderImpl service_provider_impl_;

  // A directory that contains services passed to this module through
  // |ModuleData.additional_services|.
  //
  // Only valid when |module_data_.additional_services| is set and has a valid |host_directory|.
  std::unique_ptr<sys::ServiceDirectory> additional_services_dir_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ModuleContextImpl);
};

}  // namespace modular

#endif  // SRC_MODULAR_BIN_SESSIONMGR_STORY_RUNNER_MODULE_CONTEXT_IMPL_H_
