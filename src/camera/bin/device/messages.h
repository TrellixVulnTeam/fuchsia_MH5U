// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_BIN_DEVICE_MESSAGES_H_
#define SRC_CAMERA_BIN_DEVICE_MESSAGES_H_

namespace camera {

constexpr auto kDeviceAlreadyBound =
    "This device already has active clients. New clients may connect via existing clients using "
    "the camera3.Device.Rebind method.";

constexpr auto kStreamAlreadyBound =
    "This stream already has active clients. New clients may connect via existing clients using "
    "the camera3.Stream.Rebind method.";

}  // namespace camera

#endif  // SRC_CAMERA_BIN_DEVICE_MESSAGES_H_
