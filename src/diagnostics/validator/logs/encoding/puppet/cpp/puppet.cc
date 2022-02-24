// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/diagnostics/stream/cpp/fidl.h>
#include <fuchsia/validate/logs/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/macros.h>

#include <string>

#include <sdk/lib/syslog/streams/cpp/encode.h>

class Puppet : public fuchsia::validate::logs::EncodingPuppet {
 public:
  explicit Puppet(std::unique_ptr<sys::ComponentContext> context) : context_(std::move(context)) {
    context_->outgoing()->AddPublicService(encoding_bindings_.GetHandler(this));
  }

  void Encode(fuchsia::diagnostics::stream::Record record, EncodeCallback callback) override {
    std::vector<uint8_t> buffer;
    streams::log_record(record, &buffer);
    fuchsia::mem::Buffer read_buffer;
    zx::vmo::create(buffer.size(), 0, &read_buffer.vmo);
    read_buffer.vmo.write(buffer.data(), 0, buffer.size());
    read_buffer.size = buffer.size();
    fuchsia::validate::logs::EncodingPuppet_Encode_Result result;
    fuchsia::validate::logs::EncodingPuppet_Encode_Response response{std::move(read_buffer)};
    result.set_response(std::move(response));
    callback(std::move(result));
  }

 private:
  std::unique_ptr<sys::ComponentContext> context_;
  fidl::BindingSet<fuchsia::validate::logs::EncodingPuppet> encoding_bindings_;
};

int main(int argc, const char** argv) {
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  Puppet puppet(sys::ComponentContext::CreateAndServeOutgoingDirectory());
  loop.Run();
}
