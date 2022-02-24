// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/syslog/global.h>
#include <lib/zx/process.h>

#include <memory>

#include "export.h"
#include "fx_logger.h"
#include "lib/syslog/logger.h"

namespace syslog_internal {

__WEAK bool HasStructuredBackend() { return false; }
}  // namespace syslog_internal

namespace {

fx_logger_t* MakeDefaultLogger(bool connect) {
  char process_name[ZX_MAX_NAME_LEN] = "";
  const char* tag = process_name;

  zx_status_t status =
      zx::process::self()->get_property(ZX_PROP_NAME, process_name, sizeof(process_name));
  if (status != ZX_OK)
    process_name[0] = '\0';

  fx_logger_config_t config = {.min_severity = FX_LOG_SEVERITY_DEFAULT,
                               .console_fd = -1,
                               .log_service_channel = ZX_HANDLE_INVALID,
                               .tags = &tag,
                               .num_tags = 1};
  fx_logger_t* logger = NULL;
  status = fx_logger_create_internal(&config, &logger, connect);
  // Making the default logger should never fail.
  ZX_DEBUG_ASSERT(status == ZX_OK);
  return logger;
}

}  // namespace

fx_logger_t* get_or_create_global_logger(bool connect) {
  // Upon initialization, the default logger is either provided with a
  // socket connection, or a fallback file-descriptor (which it will use)
  // or it will be initialized to log to STDERR. This object is contructed on
  // the first call to this function and will be leaked on shutdown.
  static fx_logger_t* logger = MakeDefaultLogger(connect);
  return logger;
}

SYSLOG_EXPORT
fx_logger_t* fx_log_get_logger() { return get_or_create_global_logger(true); }

SYSLOG_EXPORT
zx_status_t fx_log_reconfigure(const fx_logger_config_t* config) {
  fx_logger_t* logger = get_or_create_global_logger(
      config->console_fd == -1 && config->log_service_channel == ZX_HANDLE_INVALID);
  return logger->Reconfigure(
      config, (config->console_fd == -1 && config->log_service_channel == ZX_HANDLE_INVALID &&
               config->log_sink_socket == ZX_HANDLE_INVALID));
}

// This is here to force a definition to be included here for C99.
extern inline bool fx_log_is_enabled(fx_log_severity_t severity);
