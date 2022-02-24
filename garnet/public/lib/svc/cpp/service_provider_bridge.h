// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_SVC_CPP_SERVICE_PROVIDER_BRIDGE_H_
#define LIB_SVC_CPP_SERVICE_PROVIDER_BRIDGE_H_

#include <fuchsia/sys/cpp/fidl.h>
#include <lib/fit/function.h>
#include <lib/zx/channel.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <fbl/ref_ptr.h>

#include "lib/fidl/cpp/binding_set.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/weak_ptr.h"
#include "src/lib/storage/vfs/cpp/synchronous_vfs.h"
#include "src/lib/storage/vfs/cpp/vfs_types.h"

namespace component {

// ServiceProviderBridge is a bridge between a service provider and a service
// directory.
//
// The bridge takes a service provider to use as a backend and exposes both the
// service provider interface and the directory interface, which will make it
// easier to migrate clients to the directory interface.
class ServiceProviderBridge : public fuchsia::sys::ServiceProvider {
 public:
  ServiceProviderBridge();
  ~ServiceProviderBridge() override;

  using ServiceConnector = fit::function<void(zx::channel)>;

  void AddServiceForName(ServiceConnector connector, const std::string& service_name);

  template <typename Interface>
  void AddService(fidl::InterfaceRequestHandler<Interface> handler,
                  const std::string& service_name = Interface::Name_) {
    AddServiceForName(
        [handler = std::move(handler)](zx::channel channel) {
          handler(fidl::InterfaceRequest<Interface>(std::move(channel)));
        },
        service_name);
  }

  void set_backend(fuchsia::sys::ServiceProviderPtr backend) { backend_ = std::move(backend); }

  void set_backing_dir(zx::channel backing_dir) { backing_dir_ = std::move(backing_dir); }

  void AddBinding(fidl::InterfaceRequest<ServiceProvider> request);
  fidl::InterfaceHandle<ServiceProvider> AddBinding();

  bool ServeDirectory(zx::channel channel);

  zx::channel OpenAsDirectory();
  int OpenAsFileDescriptor();

 private:
  // A directory-like object which dynamically creates Service vnodes
  // for any file lookup.  Does not support enumeration since the actual
  // set of services available is not known by the bridge.
  class ServiceProviderDir final : public fs::Vnode {
   public:
    explicit ServiceProviderDir(fxl::WeakPtr<ServiceProviderBridge> bridge);
    ~ServiceProviderDir() final;

    // Overridden from |fs::Vnode|:
    fs::VnodeProtocolSet GetProtocols() const final;
    zx_status_t Lookup(std::string_view name, fbl::RefPtr<fs::Vnode>* out) final;
    zx_status_t GetAttributes(fs::VnodeAttributes* a) final;
    zx_status_t GetNodeInfoForProtocol(fs::VnodeProtocol protocol, fs::Rights rights,
                                       fs::VnodeRepresentation* representation) final;

   private:
    fxl::WeakPtr<ServiceProviderBridge> const bridge_;
  };

  // Overridden from |ServiceProvider|:
  void ConnectToService(std::string service_name, zx::channel channel) override;

  fs::SynchronousVfs vfs_;
  fidl::BindingSet<ServiceProvider> bindings_;
  fbl::RefPtr<ServiceProviderDir> directory_;

  std::map<std::string, ServiceConnector> name_to_service_connector_;
  fuchsia::sys::ServiceProviderPtr backend_;
  zx::channel backing_dir_;

  fxl::WeakPtrFactory<ServiceProviderBridge> weak_factory_;

  FXL_DISALLOW_COPY_AND_ASSIGN(ServiceProviderBridge);
};

}  // namespace component

#endif  // LIB_SVC_CPP_SERVICE_PROVIDER_BRIDGE_H_
