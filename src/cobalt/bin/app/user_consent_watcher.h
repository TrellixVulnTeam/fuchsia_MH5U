// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_COBALT_BIN_APP_USER_CONSENT_WATCHER_H_
#define SRC_COBALT_BIN_APP_USER_CONSENT_WATCHER_H_

#include <fuchsia/settings/cpp/fidl.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/sys/cpp/service_directory.h>

#include <memory>

#include "src/lib/backoff/exponential_backoff.h"
#include "src/lib/fxl/functional/cancelable_callback.h"
#include "src/lib/fxl/macros.h"
#include "third_party/cobalt/src/public/cobalt_service_interface.h"
#include "zircon/system/ulib/async/include/lib/async/dispatcher.h"

namespace cobalt {

// Calls a callback with an updated CobaltServiceInterface::DataCollectionPolicy when the "user data
// sharing consent" option changes. The callback will be called once when the Watcher connects to
// the service (but not before), and each time the PrivacySettings change.
//
// In case of failure, e.g., loss of connection, error returned, the data collection policy is set
// to DO_NOT_UPLOAD regardless of its current state, and the connection to the service will be
// severed. Following an exponential backoff, the connection will be re-established.
//
// Wraps around fuchsia::settings::PrivacyPtr to handle establishing the connection, losing the
// connection, waiting for the callback, etc.
class UserConsentWatcher {
 public:
  // fuchsia.settings.Privacy is expected to be in |services|.
  UserConsentWatcher(
      async_dispatcher_t* dispatcher, inspect::Node inspect_node,
      std::shared_ptr<sys::ServiceDirectory> services,
      std::function<void(const CobaltServiceInterface::DataCollectionPolicy&)> callback);

  // Connects to fuchsia.settings.Privacy and watches for "user data sharing consent" changes.
  void StartWatching();

  // Whether the watcher is currently connected to fuchsia.settings.Privacy.
  //
  // Mostly for testing purposes.
  bool IsConnected() { return privacy_settings_ptr_.is_bound(); }

  // Mostly for testing purposes.
  const fuchsia::settings::PrivacySettings& privacy_settings() { return privacy_settings_; }

 private:
  void RestartWatching();
  void Watch();
  void ResetConsent();
  CobaltServiceInterface::DataCollectionPolicy GetDataCollectionPolicy();
  void Update();

  async_dispatcher_t* dispatcher_;
  inspect::Node inspect_node_;
  inspect::IntProperty watch_successes_;
  inspect::IntProperty watch_errors_;
  inspect::IntProperty current_policy_;
  const std::shared_ptr<sys::ServiceDirectory> services_;
  std::function<void(const CobaltServiceInterface::DataCollectionPolicy&)> callback_;

  fuchsia::settings::PrivacySettings privacy_settings_;
  fuchsia::settings::PrivacyPtr privacy_settings_ptr_;

  backoff::ExponentialBackoff backoff_;
  fxl::CancelableClosure reconnect_task_;

  FXL_DISALLOW_COPY_AND_ASSIGN(UserConsentWatcher);
};

}  // namespace cobalt

#endif  // SRC_COBALT_BIN_APP_USER_CONSENT_WATCHER_H_
