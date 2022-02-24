// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_EXAMPLES_EMBEDDER_EXAMPLE_VIEW_PROVIDER_SERVICE_H_
#define SRC_UI_EXAMPLES_EMBEDDER_EXAMPLE_VIEW_PROVIDER_SERVICE_H_

#include <fuchsia/sys/cpp/fidl.h>
#include <fuchsia/ui/app/cpp/fidl.h>
#include <fuchsia/ui/views/cpp/fidl.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fit/function.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/ui/scenic/cpp/resources.h>
#include <lib/ui/scenic/cpp/session.h>
#include <lib/zx/eventpair.h>

namespace embedder {

// Parameters for creating a view.
struct ViewContext {
  sys::ComponentContext* component_context;
  fuchsia::ui::views::ViewToken token;
  fidl::InterfaceHandle<fuchsia::sys::ServiceProvider> incoming_services;
  fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> outgoing_services;
};

// A callback to create a view in response to a call to
// |ViewProvider.CreateView()|.
using ViewFactory = fit::function<void(ViewContext context)>;

// A basic implementation of the |ViewProvider| interface which Scenic clients
// can use to create and expose custom Views to other Scenic clients.
class ExampleViewProviderService : public fuchsia::ui::app::ViewProvider {
 public:
  ExampleViewProviderService(::sys::ComponentContext* component_ctx, ViewFactory factory);
  ~ExampleViewProviderService() override;
  ExampleViewProviderService(const ExampleViewProviderService&) = delete;
  ExampleViewProviderService& operator=(const ExampleViewProviderService&) = delete;

  // |fuchsia::ui::app::ViewProvider|
  void CreateView(zx::eventpair token,
                  fidl::InterfaceRequest<fuchsia::sys::ServiceProvider> incoming_services,
                  fidl::InterfaceHandle<fuchsia::sys::ServiceProvider> outgoing_services) override;

 private:
  fidl::BindingSet<fuchsia::ui::app::ViewProvider> bindings_;
  ::sys::ComponentContext* component_ctx_;
  ViewFactory view_factory_fn_;
};

}  // namespace embedder

#endif  // SRC_UI_EXAMPLES_EMBEDDER_EXAMPLE_VIEW_PROVIDER_SERVICE_H_
