// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_EXCEPTIONS_HANDLER_REPORT_BUILDER_H_
#define SRC_DEVELOPER_FORENSICS_EXCEPTIONS_HANDLER_REPORT_BUILDER_H_

#include <fuchsia/feedback/cpp/fidl.h>
#include <lib/zx/process.h>
#include <lib/zx/thread.h>
#include <lib/zx/vmo.h>

#include <optional>
#include <string>

#include "src/developer/forensics/exceptions/handler/component_lookup.h"
#include "src/developer/forensics/exceptions/handler/minidump.h"

namespace forensics {
namespace exceptions {
namespace handler {

class CrashReportBuilder {
 public:
  CrashReportBuilder& SetProcess(const zx::process& process);
  CrashReportBuilder& SetThread(const zx::thread& thread);
  CrashReportBuilder& SetMinidump(zx::vmo minidump);
  CrashReportBuilder& SetExceptionReason(const std::optional<ExceptionReason>& exception_reason);
  CrashReportBuilder& SetComponentInfo(const ComponentInfo& component_info);
  CrashReportBuilder& SetExceptionExpired();
  CrashReportBuilder& SetProcessTerminated();
  CrashReportBuilder& SetGwpAsanExceptionType(std::string exception_type);

  const std::optional<std::string>& ProcessName() const;

  fuchsia::feedback::CrashReport Consume();

 private:
  std::optional<std::string> process_name_;
  std::optional<zx_koid_t> process_koid_;
  std::optional<zx_duration_t> process_uptime_{std::nullopt};
  std::optional<std::string> thread_name_;
  std::optional<zx_koid_t> thread_koid_;
  std::optional<zx::vmo> minidump_{std::nullopt};
  std::optional<ExceptionReason> exception_reason_{std::nullopt};
  std::optional<std::string> component_url_{std::nullopt};
  std::optional<std::string> realm_path_{std::nullopt};
  std::optional<std::string> gwp_asan_exception_type_;
  bool exception_expired_{false};
  bool process_already_terminated_{false};

  bool is_valid_{true};
};

}  // namespace handler
}  // namespace exceptions
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_EXCEPTIONS_HANDLER_REPORT_BUILDER_H_
