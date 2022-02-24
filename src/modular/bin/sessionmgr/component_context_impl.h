// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_BIN_SESSIONMGR_COMPONENT_CONTEXT_IMPL_H_
#define SRC_MODULAR_BIN_SESSIONMGR_COMPONENT_CONTEXT_IMPL_H_

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/fidl/cpp/string.h>

#include <string>

#include "src/lib/fxl/macros.h"

namespace modular {

class AgentRunner;

// The parameters of component context that do not vary by instance.
struct ComponentContextInfo {
  AgentRunner* const agent_runner;
  std::vector<std::string> session_agents;
};

// Implements the fuchsia::modular::ComponentContext interface, which is
// provided to modules and agents. The interface is public, because the class
// doesn't contain the Bindings for this interface. TODO(mesch): Move
// bindings into the class.
class ComponentContextImpl : public fuchsia::modular::ComponentContext {
 public:
  // * A component instance ID identifies a particular instance of a component;
  //   for modules, this is the module path in their story. For agents, it is
  //   the agent URL.
  // * A component URL is the origin from which the executable associated with
  //   the component was fetched from.
  explicit ComponentContextImpl(const ComponentContextInfo& info, std::string component_instance_id,
                                std::string component_url);

  ~ComponentContextImpl() override;

  const std::string& component_instance_id() { return component_instance_id_; }

  void Connect(fidl::InterfaceRequest<fuchsia::modular::ComponentContext> request);
  fuchsia::modular::ComponentContextPtr NewBinding();

 private:
  // |fuchsia::modular::ComponentContext|
  void DeprecatedConnectToAgent(
      std::string url,
      fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> incoming_services_request,
      fidl::InterfaceRequest<fuchsia::modular::AgentController> agent_controller_request) override;

  // |fuchsia::modular::ComponentContext|
  void DeprecatedConnectToAgentService(fuchsia::modular::AgentServiceRequest request) override;

  // Returns true if the agent url is a session agent.
  bool AgentIsSessionAgent(const std::string& agent_url);

  AgentRunner* const agent_runner_;
  std::vector<std::string> session_agents_;

  const std::string component_instance_id_;
  const std::string component_url_;

  fidl::BindingSet<fuchsia::modular::ComponentContext> bindings_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ComponentContextImpl);
};

}  // namespace modular

#endif  // SRC_MODULAR_BIN_SESSIONMGR_COMPONENT_CONTEXT_IMPL_H_
