// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/shell/interpreter/test/interpreter_test.h"

#include <lib/fdio/directory.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "fidl/fuchsia.shell/cpp/wire.h"
#include "fuchsia/sys/cpp/fidl.h"
#include "lib/async-loop/default.h"
#include "zircon/status.h"

// Adds an object to the builder with the names, values, and types as given in parallel arrays.
shell::console::AstBuilder::NodePair AddObject(
    shell::console::AstBuilder& builder, std::vector<std::string>& names,
    std::vector<shell::console::AstBuilder::NodeId>& values,
    std::vector<fuchsia_shell::wire::ShellType>&& types) {
  EXPECT_EQ(names.size(), values.size())
      << "Test incorrect - mismatch in keys and values for constructing object";
  EXPECT_EQ(names.size(), types.size())
      << "Test incorrect - mismatch in fields and types for constructing object";
  builder.OpenObject();
  for (size_t i = 0; i < names.size(); i++) {
    builder.AddField(names[i], values[i], std::move(types[i]));
  }
  return builder.CloseObject();
}

fuchsia_shell::wire::ExecuteResult InterpreterTestContext::GetResult() const {
  std::string string = error_stream.str();
  if (!string.empty()) {
    std::cout << string;
  }
  return result;
}

InterpreterTest::InterpreterTest()
    : loop_(&kAsyncLoopConfigAttachToCurrentThread),
      context_(sys::ComponentContext::CreateAndServeOutgoingDirectory()) {
  ::fidl::InterfaceHandle<fuchsia::io::Directory> directory;

  fuchsia::sys::LaunchInfo launch_info;
  launch_info.url = "fuchsia-pkg://fuchsia.com/shell_server#meta/shell_server.cmx";
  launch_info.directory_request = directory.NewRequest().TakeChannel();

  fuchsia::sys::LauncherPtr launcher;
  context_->svc()->Connect(launcher.NewRequest());
  launcher->CreateComponent(std::move(launch_info), controller_.NewRequest());

  shell_provider_ = std::make_unique<sys::ServiceDirectory>(std::move(directory));
}

void InterpreterTest::Finish(FinishAction action) {
  std::vector<std::string> no_errors;
  Finish(action, no_errors);
}

void InterpreterTest::Finish(FinishAction action, const std::vector<std::string>& expected_errors) {
  Run(action);
  // Shutdown the interpreter (that also closes the channel => we can't use it anymore after this
  // call).
  auto errors = shell()->Shutdown();
  // Checks if the errors are what we expected.
  bool ok = true;
  if (expected_errors.size() != errors->errors.count()) {
    ok = false;
  } else {
    for (size_t i = 0; i < expected_errors.size(); ++i) {
      if (expected_errors[i] != std::string(errors->errors[i].data(), errors->errors[i].size())) {
        ok = false;
        break;
      }
    }
  }
  if (!ok) {
    std::cout << "Shutdown incorrect\n";
    if (!expected_errors.empty()) {
      std::cout << "Expected:\n";
      for (const auto& error : expected_errors) {
        std::cout << "  " << error << '\n';
      }
      if (errors->errors.empty()) {
        std::cout << "Got no error\n";
      } else {
        std::cout << "Got:\n";
      }
    }
    for (const auto& error : errors->errors) {
      std::cout << "  " << std::string(error.data(), error.size()) << '\n';
    }
    ASSERT_TRUE(ok);
  }
  if (action != kError) {
    std::string global_errors = global_error_stream_.str();
    if (!global_errors.empty()) {
      std::cout << global_errors;
    }
  }
}

void InterpreterTest::Run(FinishAction action) {
  class EventHandler : public fidl::WireSyncEventHandler<fuchsia_shell::Shell> {
   public:
    EventHandler(InterpreterTest* test, FinishAction action) : test_(test), action_(action) {}

    const std::string& msg() const { return msg_; }
    bool done() const { return done_; }
    bool ok() const { return ok_; }

    void OnError(fidl::WireEvent<fuchsia_shell::Shell::OnError>* event) override {
      if (action_ == kError) {
        done_ = true;
      }
      if (event->context_id == 0) {
        test_->global_error_stream_
            << std::string(event->error_message.data(), event->error_message.size()) << "\n";
      } else {
        InterpreterTestContext* context = test_->GetContext(event->context_id);
        if (context == nullptr) {
          msg_ = "context == nullptr in on_error";
          ok_ = false;
        } else {
          for (const auto& location : event->locations) {
            if (location.has_node_id()) {
              context->error_stream << "node " << location.node_id().file_id << ':'
                                    << location.node_id().node_id << ' ';
            }
          }
          context->error_stream << std::string(event->error_message.data(),
                                               event->error_message.size())
                                << "\n";
        }
      }
    }

    void OnDumpDone(fidl::WireEvent<fuchsia_shell::Shell::OnDumpDone>* event) override {
      if (action_ == kDump) {
        done_ = true;
      }
      InterpreterTestContext* context = test_->GetContext(event->context_id);
      if (context == nullptr) {
        msg_ = "context == nullptr in on_dump_done";
        ok_ = false;
      }
    }

    void OnExecutionDone(fidl::WireEvent<fuchsia_shell::Shell::OnExecutionDone>* event) override {
      if (action_ != kExecute) {
        msg_ = "Expected action: kExecute was: " + std::to_string(action_);
        ok_ = false;
        return;
      }
      done_ = true;

      InterpreterTestContext* context = test_->GetContext(event->context_id);
      if (context == nullptr) {
        msg_ = "context == nullptr in on_execution_done";
        ok_ = false;
        return;
      }
      context->result = event->result;
    }

    void OnTextResult(fidl::WireEvent<fuchsia_shell::Shell::OnTextResult>* event) override {
      if (action_ == kTextResult) {
        done_ = true;
      }
      InterpreterTestContext* context = test_->GetContext(event->context_id);
      if (context == nullptr) {
        msg_ = "context == nullptr in on_text_result";
        ok_ = false;
        return;
      }
      std::string result_string(event->result.data(), event->result.size());
      if (test_->last_text_result_partial_) {
        if (test_->text_results_.empty()) {
          msg_ = "text results empty";
          ok_ = false;
          return;
        }
        test_->text_results_.back() += result_string;
      } else {
        test_->text_results_.emplace_back(std::move(result_string));
      }
      test_->last_text_result_partial_ = event->partial_result;
    }

    void OnResult(fidl::WireEvent<fuchsia_shell::Shell::OnResult>* event) override {
      InterpreterTestContext* context = test_->GetContext(event->context_id);
      if (context == nullptr) {
        msg_ = "context == nullptr in on_text_result";
        ok_ = false;
        return;
      }
      if (event->partial_result) {
        msg_ = " partial results not supported";
        ok_ = false;
        return;
      }
      shell::common::DeserializeResult deserialize;
      test_->results_.emplace_back(deserialize.Deserialize(event->nodes));
    }

    zx_status_t Unknown() override { return ZX_ERR_NOT_SUPPORTED; }

   private:
    InterpreterTest* const test_;
    const FinishAction action_;
    std::string msg_;
    bool done_ = false;
    bool ok_ = true;
  };

  EventHandler event_handler(this, action);
  while (!event_handler.done()) {
    ::fidl::Result result = shell_.HandleOneEvent(event_handler);
    ASSERT_TRUE(result.ok() && event_handler.ok()) << event_handler.msg();
  }
}

InterpreterTestContext* InterpreterTest::CreateContext() {
  uint64_t id = ++last_context_id_;
  auto context = std::make_unique<InterpreterTestContext>(id);
  auto result = context.get();
  contexts_.emplace(id, std::move(context));
  return result;
}

InterpreterTestContext* InterpreterTest::GetContext(uint64_t context_id) {
  auto result = contexts_.find(context_id);
  if (result == contexts_.end()) {
    return nullptr;
  }
  return result->second.get();
}

void InterpreterTest::SetUp() {
  auto endpoints = fidl::CreateEndpoints<fuchsia_shell::Shell>();
  shell_ = fidl::WireSyncClient<fuchsia_shell::Shell>(std::move(endpoints->client));

  // Reset context ids.
  last_context_id_ = 0;
  // Resets the global error stream for the test (to be able to run multiple tests).
  global_error_stream_.str() = "";

  // Creates a new connection to the server.
  ASSERT_EQ(ZX_OK, shell_provider_->Connect("fuchsia.shell.Shell",
                                            std::move(endpoints->server.channel())));
}
