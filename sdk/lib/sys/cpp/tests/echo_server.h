// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_SYS_CPP_TESTS_ECHO_SERVER_H_
#define LIB_SYS_CPP_TESTS_ECHO_SERVER_H_

#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_request.h>

#include <test/placeholders/cpp/fidl.h>

namespace {

class EchoImpl : public test::placeholders::Echo {
 public:
  void EchoString(fidl::StringPtr value, EchoStringCallback callback) override {
    callback(std::move(value));
  }
  fidl::InterfaceRequestHandler<test::placeholders::Echo> GetHandler(
      async_dispatcher_t* dispatcher) {
    return bindings_.GetHandler(this, dispatcher);
  }

  void AddBinding(zx::channel request, async_dispatcher_t* dispatcher) {
    bindings_.AddBinding(this, fidl::InterfaceRequest<test::placeholders::Echo>(std::move(request)),
                         dispatcher);
  }

 private:
  fidl::BindingSet<test::placeholders::Echo> bindings_;
};

}  // namespace

#endif  // LIB_SYS_CPP_TESTS_ECHO_SERVER_H_
