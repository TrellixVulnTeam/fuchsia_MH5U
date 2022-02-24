// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/graph/thread_priority.h"

#include <fuchsia/scheduler/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fpromise/result.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/channel.h>

namespace media_player {
namespace {

constexpr uint32_t kHighPriority = 23;
constexpr char kServicePathPrefix[] = "/svc/";
constexpr char kProfileName[] = "src/media/playback/mediaplayer";

fpromise::result<zx::unowned_profile, zx_status_t> GetHighPriorityProfile() {
  static zx::profile profile;
  static zx_status_t status = []() {
    zx::channel server_channel, client_channel;
    zx_status_t status = zx::channel::create(0u, &server_channel, &client_channel);
    if (status != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Failed to create a channel pair";
      return status;
    }

    status = fdio_service_connect(
        (std::string(kServicePathPrefix) + fuchsia::scheduler::ProfileProvider::Name_).c_str(),
        server_channel.release());
    if (status != ZX_OK) {
      FX_PLOGS(ERROR, status) << "Failed to connect to fuchsia.scheduler.ProfileProvider";
      return status;
    }

    fuchsia::scheduler::ProfileProvider_SyncProxy provider(std::move(client_channel));

    zx_status_t get_profile_result_status;
    status = provider.GetProfile(kHighPriority, kProfileName, &get_profile_result_status, &profile);

    if (status != ZX_OK) {
      FX_PLOGS(WARNING, status) << "Failed to call fuchsia.scheduler.GetProfile (normal in tests)";
      return status;
    }

    if (get_profile_result_status != ZX_OK) {
      FX_PLOGS(ERROR, get_profile_result_status) << "fuchsia.scheduler.GetProfile returned error";
      return get_profile_result_status;
    }

    return ZX_OK;
  }();

  if (status != ZX_OK) {
    return fpromise::error(status);
  }

  return fpromise::ok<zx::unowned_profile>(zx::unowned_profile{profile});
}

}  // namespace

// static
zx_status_t ThreadPriority::SetToHigh(zx::thread* thread) {
  auto result = GetHighPriorityProfile();
  if (result.is_error()) {
    return result.error();
  }

  zx_status_t status = thread ? thread->set_profile(*result.value(), 0)
                              : zx::thread::self()->set_profile(*result.value(), 0);

  if (status != ZX_OK) {
    FX_PLOGS(ERROR, status) << "Failed to set thread profile";
  }

  return status;
}

}  // namespace media_player
