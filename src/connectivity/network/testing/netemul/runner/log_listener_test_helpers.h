// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_RUNNER_LOG_LISTENER_TEST_HELPERS_H_
#define SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_RUNNER_LOG_LISTENER_TEST_HELPERS_H_

#include <fuchsia/logger/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>

#include <gtest/gtest.h>

namespace netemul {
namespace testing {

constexpr uint64_t kDummyTid = 0xAA;
constexpr uint64_t kDummyPid = 0xBB;
constexpr int64_t kDummyTime = 0xCCAACC;
constexpr int32_t kDummySeverity = static_cast<int32_t>(fuchsia::logger::LogLevelFilter::FATAL);

class TestListener : public fuchsia::logger::LogListenerSafe {
 public:
  using ObserverCallback = fit::function<void(const fuchsia::logger::LogMessage&)>;
  explicit TestListener(fidl::InterfaceRequest<fuchsia::logger::LogListenerSafe> req)
      : binding_(this, std::move(req)) {}

  TestListener() : binding_(this) {}

  void Log(fuchsia::logger::LogMessage log, LogCallback received) override {
    if (observer_callback_) {
      observer_callback_(log);
    }
    messages_.emplace_back(std::move(log));
    received();
  }

  void LogMany(std::vector<fuchsia::logger::LogMessage> log, LogManyCallback received) override {
    for (auto& l : log) {
      Log(std::move(l), []() {});
    }
    received();
  }

  void Done() override {}

  std::vector<fuchsia::logger::LogMessage>& messages() { return messages_; }

  void SetObserver(ObserverCallback observer) { observer_callback_ = std::move(observer); }

 private:
  fidl::Binding<fuchsia::logger::LogListenerSafe> binding_;
  std::vector<fuchsia::logger::LogMessage> messages_;
  ObserverCallback observer_callback_;
};

// Create a test log message.
fuchsia::logger::LogMessage CreateLogMessage(std::vector<std::string> tags, std::string message);

}  // namespace testing
}  // namespace netemul

#endif  // SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_RUNNER_LOG_LISTENER_TEST_HELPERS_H_
