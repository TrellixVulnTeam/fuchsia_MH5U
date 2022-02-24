// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_FEEDBACK_REBOOT_LOG_REBOOT_REASON_H_
#define SRC_DEVELOPER_FORENSICS_FEEDBACK_REBOOT_LOG_REBOOT_REASON_H_

#include <fuchsia/feedback/cpp/fidl.h>

#include <optional>
#include <string>

#include "src/developer/forensics/utils/cobalt/metrics.h"

namespace forensics {
namespace feedback {

// Feedback's internal representation of why a device rebooted.
//
// These values should not be used to understand why a device has rebooted outside of this
// component.
enum class RebootReason {
  // We could not make a reboot reason out of the reboot log.
  kNotParseable,
  kGenericGraceful,
  kCold,
  // The device spontaneously rebooted, e.g., brief loss of power.
  kSpontaneous,
  kKernelPanic,
  kOOM,
  kHardwareWatchdogTimeout,
  kSoftwareWatchdogTimeout,
  kBrownout,
  kRootJobTermination,
  kUserRequest,
  kSystemUpdate,
  kRetrySystemUpdate,
  kZbiSwap,
  kHighTemperature,
  kSessionFailure,
  kSysmgrFailure,
  kCriticalComponentFailure,
  kFdr,
};

std::string ToString(RebootReason reason);

// Whether the reason justifies a crash report.
bool IsCrash(RebootReason reason);

// Whether the reason is deemed fatal.
bool IsFatal(RebootReason reason);

// Whether the reboot is graceful, ungraceful or undetermined.
std::optional<bool> OptionallyGraceful(RebootReason reason);

cobalt::LastRebootReason ToCobaltLastRebootReason(RebootReason reason);
std::string ToCrashProgramName(RebootReason reason);
std::optional<fuchsia::feedback::RebootReason> ToFidlRebootReason(RebootReason reason);

// Creates a crash signature for |reason|.
//
// Note: |critical_process| is only supported for |kRootJobTermination|.
std::string ToCrashSignature(RebootReason reason,
                             const std::optional<std::string>& critical_process = std::nullopt);

}  // namespace feedback
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_FEEDBACK_REBOOT_LOG_REBOOT_REASON_H_
