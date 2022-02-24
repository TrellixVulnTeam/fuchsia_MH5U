// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/bin/root_presenter/media_buttons_handler.h"

#include <fuchsia/ui/input/cpp/fidl.h>
#include <fuchsia/ui/policy/cpp/fidl.h>
#include <zircon/types.h>

#include <src/lib/fostr/fidl/fuchsia/ui/input/formatting.h>
#include <src/ui/bin/root_presenter/constants.h>

using fuchsia::ui::policy::MediaButtonsListenerPtr;

namespace root_presenter {
namespace {

void ChattyReportLog(const fuchsia::ui::input::InputReport& report) {
  static uint32_t chatty = 0;
  if (chatty++ < ChattyMax()) {
    FX_LOGS(INFO) << "RP-MediaReport[" << chatty << "/" << ChattyMax() << "]: " << report;
  }
}

void ChattyEventLog(const fuchsia::ui::input::MediaButtonsEvent& event,
                    fuchsia::ui::policy::MediaButtonsListenerPtr& listener) {
  static uint32_t chatty = 0;
  if (chatty++ < ChattyMax()) {
    zx_koid_t koid = ZX_KOID_INVALID;

    {
      zx_info_handle_basic_t info{};
      size_t actual_count = 0;
      size_t avail_count = 0;
      zx_status_t status = listener.channel().get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info),
                                                       &actual_count, &avail_count);
      if (status == ZX_OK) {
        koid = info.koid;
      }
    }
    FX_LOGS(INFO) << "RP-MediaEvent[" << chatty << "/" << ChattyMax() << "]: dest=" << koid << ", "
                  << event;
  }
}

}  // namespace

bool MediaButtonsHandler::OnDeviceAdded(ui_input::InputDeviceImpl* input_device) {
  if (!input_device->descriptor()->media_buttons) {
    return false;
  }

  FX_VLOGS(1) << "MediaButtonsHandler::OnDeviceAdded: device_id=" << input_device->id();

  ui_input::OnMediaButtonsEventCallback callback = [this](fuchsia::ui::input::InputReport report) {
    OnEvent(std::move(report));
  };
  auto state = std::make_unique<ui_input::DeviceState>(
      input_device->id(), input_device->descriptor(), std::move(callback));

  ui_input::DeviceState* state_ptr = state.get();
  auto device_pair = std::make_pair(input_device, std::move(state));
  state_ptr->OnRegistered();
  device_states_by_id_.emplace(input_device->id(), std::move(device_pair));

  return true;
}

bool MediaButtonsHandler::OnReport(uint32_t device_id,
                                   fuchsia::ui::input::InputReport input_report) {
  ChattyReportLog(input_report);

  if (device_states_by_id_.count(device_id) == 0) {
    FX_VLOGS(1) << "OnReport: Unknown device " << device_id;
    return false;
  }

  ui_input::DeviceState* state = device_states_by_id_[device_id].second.get();
  fuchsia::math::Size unused;

  state->Update(std::move(input_report), unused);

  return true;
}

bool MediaButtonsHandler::OnDeviceRemoved(uint32_t device_id) {
  FX_VLOGS(1) << "MediaButtonsHandler::OnDeviceRemoved: device_id=" << device_id;
  if (device_states_by_id_.count(device_id) == 0) {
    FX_VLOGS(1) << "OnReport: Unknown device " << device_id;
    return false;
  }

  device_states_by_id_[device_id].second->OnUnregistered();
  device_states_by_id_.erase(device_id);

  return true;
}

fuchsia::ui::input::MediaButtonsEvent CreateMediaButtonsEvent(
    const fuchsia::ui::input::InputReport& report) {
  fuchsia::ui::input::MediaButtonsEvent event;
  int8_t volume_gain = 0;
  if (report.media_buttons->volume_up) {
    volume_gain++;
  }
  if (report.media_buttons->volume_down) {
    volume_gain--;
  }
  event.set_volume(volume_gain);
  event.set_mic_mute(report.media_buttons->mic_mute);
  event.set_camera_disable(report.media_buttons->camera_disable);
  event.set_pause(report.media_buttons->pause);
  return event;
}

void MediaButtonsHandler::OnEvent(fuchsia::ui::input::InputReport report) {
  FX_CHECK(report.media_buttons);
  for (auto& listener : old_media_buttons_listeners_) {
    fuchsia::ui::input::MediaButtonsEvent event = CreateMediaButtonsEvent(report);
    ChattyEventLog(event, listener);
    listener->OnMediaButtonsEvent(std::move(event));
  }

  for (auto& listener : media_buttons_listeners_) {
    fuchsia::ui::input::MediaButtonsEvent event = CreateMediaButtonsEvent(report);
    fit::function<void()> on_event_callback = [] {};
    ChattyEventLog(event, listener);
    listener->OnEvent(std::move(event), std::move(on_event_callback));
  }
}

void MediaButtonsHandler::RegisterListener(
    fidl::InterfaceHandle<fuchsia::ui::policy::MediaButtonsListener> listener_handle) {
  MediaButtonsListenerPtr listener;

  listener.Bind(std::move(listener_handle));

  // Auto-remove listeners if the interface closes.
  listener.set_error_handler([this, listener = listener.get()](zx_status_t status) {
    old_media_buttons_listeners_.erase(
        std::remove_if(old_media_buttons_listeners_.begin(), old_media_buttons_listeners_.end(),
                       [listener](const MediaButtonsListenerPtr& item) -> bool {
                         return item.get() == listener;
                       }),
        old_media_buttons_listeners_.end());
  });

  // Send the last seen report to the listener so they have the information
  // about the media button's state.
  for (auto it = device_states_by_id_.begin(); it != device_states_by_id_.end(); it++) {
    const ui_input::InputDeviceImpl* device_impl = it->second.first;
    const fuchsia::ui::input::InputReport* report = device_impl->LastReport();
    if (report) {
      listener->OnMediaButtonsEvent(CreateMediaButtonsEvent(*report));
    }
  }

  old_media_buttons_listeners_.push_back(std::move(listener));
}

void MediaButtonsHandler::RegisterListener2(
    fidl::InterfaceHandle<fuchsia::ui::policy::MediaButtonsListener> listener_handle) {
  MediaButtonsListenerPtr listener;

  listener.Bind(std::move(listener_handle));

  // Auto-remove listeners if the interface closes.
  listener.set_error_handler([this, listener = listener.get()](zx_status_t status) {
    media_buttons_listeners_.erase(
        std::remove_if(media_buttons_listeners_.begin(), media_buttons_listeners_.end(),
                       [listener](const MediaButtonsListenerPtr& item) -> bool {
                         return item.get() == listener;
                       }),
        media_buttons_listeners_.end());
  });

  // Send the last seen report to the listener so they have the information
  // about the media button's state.
  for (auto it = device_states_by_id_.begin(); it != device_states_by_id_.end(); it++) {
    const ui_input::InputDeviceImpl* device_impl = it->second.first;
    const fuchsia::ui::input::InputReport* report = device_impl->LastReport();
    if (report) {
      listener->OnMediaButtonsEvent(CreateMediaButtonsEvent(*report));
    }
  }

  media_buttons_listeners_.push_back(std::move(listener));
}
}  // namespace root_presenter
