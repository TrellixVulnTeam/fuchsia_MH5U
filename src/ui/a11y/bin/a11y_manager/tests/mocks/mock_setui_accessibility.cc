// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mock_setui_accessibility.h"

#include <lib/sys/cpp/testing/component_context_provider.h>

namespace accessibility_test {

MockSetUIAccessibility::MockSetUIAccessibility(sys::testing::ComponentContextProvider* context)
    : first_watch_(true) {
  context->service_directory_provider()->AddService(bindings_.GetHandler(this));
}

MockSetUIAccessibility::~MockSetUIAccessibility() = default;

void MockSetUIAccessibility::Watch(WatchCallback callback) {
  num_watch_called_++;
  watchCallback_ = std::move(callback);

  // First call to Watch should return immediately.
  if (first_watch_) {
    watchCallback_(std::move(settings_));
    first_watch_ = false;
  }
}

void MockSetUIAccessibility::Set(fuchsia::settings::AccessibilitySettings settings,
                                 SetCallback callback) {
  if (watchCallback_) {
    watchCallback_(std::move(settings));
  } else {
    settings_ = std::move(settings);
    first_watch_ = true;
  }
  callback({});
}

}  // namespace accessibility_test
