// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/cobalt/bin/testapp/cobalt_testapp_logger.h"

#include <fuchsia/diagnostics/cpp/fidl.h>
#include <lib/syslog/cpp/macros.h>

#include <map>
#include <string>

#include <rapidjson/document.h>
#include <src/lib/fsl/vmo/strings.h>

#include "src/cobalt/bin/utils/status_utils.h"

namespace cobalt::testapp {

using ::cobalt::StatusToString;
using fuchsia::cobalt::Status;

bool CobaltTestAppLogger::LogEvent(uint32_t metric_id, uint32_t index) {
  Status status = Status::INTERNAL_ERROR;
  logger_->LogEvent(metric_id, index, &status);
  FX_VLOGS(1) << "LogEvent(" << index << ") => " << StatusToString(status);
  if (status != Status::OK) {
    FX_LOGS(ERROR) << "LogEvent() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogEventCount(uint32_t metric_id, uint32_t index,
                                        const std::string& component, int64_t count) {
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  logger_->LogEventCount(metric_id, index, component, 0, count, &status);
  FX_VLOGS(1) << "LogEventCount(" << index << ") => " << StatusToString(status);
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogEventCount() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogElapsedTime(uint32_t metric_id, uint32_t index,
                                         const std::string& component, int64_t elapsed_micros) {
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  logger_->LogElapsedTime(metric_id, index, component, elapsed_micros, &status);
  FX_VLOGS(1) << "LogElapsedTime() => " << StatusToString(status);
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogElapsedTime() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogFrameRate(uint32_t metric_id, uint32_t index,
                                       const std::string& component, float fps) {
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  logger_->LogFrameRate(metric_id, index, component, fps, &status);
  FX_VLOGS(1) << "LogFrameRate() => " << StatusToString(status);
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogFrameRate() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogMemoryUsage(uint32_t metric_id, uint32_t index,
                                         const std::string& component, int64_t bytes) {
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  logger_->LogMemoryUsage(metric_id, index, component, bytes, &status);
  FX_VLOGS(1) << "LogMemoryUsage() => " << StatusToString(status);
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogMemoryUsage() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogTimer(uint32_t metric_id, uint32_t start_time, uint32_t end_time,
                                   const std::string& timer_id, uint32_t timeout_s) {
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  logger_->StartTimer(metric_id, 0, "", timer_id, start_time, timeout_s, &status);
  logger_->EndTimer(timer_id, end_time, timeout_s, &status);

  FX_VLOGS(1) << "LogTimer("
              << "timer_id:" << timer_id << ", start_time:" << start_time
              << ", end_time:" << end_time << ") => " << StatusToString(status);
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogTimer() => " << StatusToString(status);
    return false;
  }

  return true;
}

bool CobaltTestAppLogger::LogIntHistogram(uint32_t metric_id, uint32_t index,
                                          const std::string& component,
                                          const std::map<uint32_t, uint64_t>& histogram_map) {
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  std::vector<fuchsia::cobalt::HistogramBucket> histogram;
  for (auto it = histogram_map.begin(); histogram_map.end() != it; it++) {
    fuchsia::cobalt::HistogramBucket entry;
    entry.index = it->first;
    entry.count = it->second;
    histogram.push_back(std::move(entry));
  }

  logger_->LogIntHistogram(metric_id, index, component, std::move(histogram), &status);
  FX_VLOGS(1) << "LogIntHistogram() => " << StatusToString(status);
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogIntHistogram() => " << StatusToString(status);
    return false;
  }

  return true;
}

bool CobaltTestAppLogger::LogCobaltEvent(fuchsia::cobalt::CobaltEvent event) {
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  logger_->LogCobaltEvent(std::move(event), &status);

  FX_VLOGS(1) << "LogCobaltEvent() => " << StatusToString(status);
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogCobaltEvent() => " << StatusToString(status);
    return false;
  }

  return true;
}

bool CobaltTestAppLogger::LogOccurrence(uint32_t metric_id, std::vector<uint32_t> indices,
                                        uint64_t count, ExperimentArm arm) {
  fuchsia::metrics::Status status = fuchsia::metrics::Status::INTERNAL_ERROR;
  fuchsia::metrics::MetricEventLoggerSyncPtr* metric_event_logger;
  switch (arm) {
    case kExperiment:
      metric_event_logger = &experimental_metric_event_logger_;
      break;
    case kControl:
      metric_event_logger = &control_metric_event_logger_;
      break;
    default:
      metric_event_logger = &metric_event_logger_;
  };
  (*metric_event_logger)->LogOccurrence(metric_id, count, indices, &status);
  FX_VLOGS(1) << "LogOccurrence(" << count << ") => " << StatusToString(status);
  if (status != fuchsia::metrics::Status::OK) {
    FX_LOGS(ERROR) << "LogOccurrence() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogInteger(uint32_t metric_id, std::vector<uint32_t> indices,
                                     int64_t value) {
  fuchsia::metrics::Status status = fuchsia::metrics::Status::INTERNAL_ERROR;
  metric_event_logger_->LogInteger(metric_id, value, indices, &status);
  FX_VLOGS(1) << "LogInteger(" << value << ") => " << StatusToString(status);
  if (status != fuchsia::metrics::Status::OK) {
    FX_LOGS(ERROR) << "LogInteger() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogIntegerHistogram(uint32_t metric_id, std::vector<uint32_t> indices,
                                              const std::map<uint32_t, uint64_t>& histogram_map) {
  fuchsia::metrics::Status status = fuchsia::metrics::Status::INTERNAL_ERROR;
  std::vector<fuchsia::metrics::HistogramBucket> histogram;
  for (auto it = histogram_map.begin(); histogram_map.end() != it; it++) {
    fuchsia::metrics::HistogramBucket entry;
    entry.index = it->first;
    entry.count = it->second;
    histogram.push_back(std::move(entry));
  }

  metric_event_logger_->LogIntegerHistogram(metric_id, std::move(histogram), indices, &status);
  FX_VLOGS(1) << "LogIntegerHistogram() => " << StatusToString(status);
  if (status != fuchsia::metrics::Status::OK) {
    FX_LOGS(ERROR) << "LogIntegerHistogram() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogString(uint32_t metric_id, std::vector<uint32_t> indices,
                                    const std::string& string_value) {
  fuchsia::metrics::Status status = fuchsia::metrics::Status::INTERNAL_ERROR;
  metric_event_logger_->LogString(metric_id, string_value, indices, &status);
  FX_VLOGS(1) << "LogString(" << string_value << ") => " << StatusToString(status);
  if (status != fuchsia::metrics::Status::OK) {
    FX_LOGS(ERROR) << "LogString() => " << StatusToString(status);
    return false;
  }
  return true;
}

bool CobaltTestAppLogger::LogCustomMetricsTestProto(uint32_t metric_id,
                                                    const std::string& query_val,
                                                    const int64_t wait_time_val,
                                                    const uint32_t response_code_val) {
  fuchsia::cobalt::Status status = fuchsia::cobalt::Status::INTERNAL_ERROR;
  std::vector<fuchsia::cobalt::CustomEventValue> parts(3);
  parts.at(0).dimension_name = "query";
  parts.at(0).value.set_string_value(query_val);
  parts.at(1).dimension_name = "wait_time_ms";
  parts.at(1).value.set_int_value(wait_time_val);
  parts.at(2).dimension_name = "response_code";
  parts.at(2).value.set_index_value(response_code_val);
  logger_->LogCustomEvent(metric_id, std::move(parts), &status);
  FX_VLOGS(1) << "LogCustomEvent(query=" << query_val << ", wait_time_ms=" << wait_time_val
              << ", response_code=" << response_code_val << ") => " << StatusToString(status);
  if (status != fuchsia::cobalt::Status::OK) {
    FX_LOGS(ERROR) << "LogCustomEvent() => " << StatusToString(status);
    return false;
  }

  return true;
}

bool CobaltTestAppLogger::CheckForSuccessfulSend() {
  if (!use_network_) {
    FX_LOGS(INFO) << "Not using the network because --no_network_for_testing "
                     "was passed.";
    return true;
  }

  bool send_success = false;
  FX_VLOGS(1) << "Invoking RequestSendSoon() now...";
  (*cobalt_controller_)->RequestSendSoon(&send_success);
  FX_VLOGS(1) << "RequestSendSoon => " << send_success;
  return send_success;
}

std::string CobaltTestAppLogger::GetInspectJson() const {
  fuchsia::diagnostics::BatchIteratorSyncPtr iterator;
  fuchsia::diagnostics::StreamParameters stream_parameters;
  stream_parameters.set_data_type(fuchsia::diagnostics::DataType::INSPECT);
  stream_parameters.set_stream_mode(fuchsia::diagnostics::StreamMode::SNAPSHOT);
  stream_parameters.set_format(fuchsia::diagnostics::Format::JSON);

  {
    std::vector<fuchsia::diagnostics::SelectorArgument> args;
    args.emplace_back();
    args[0].set_raw_selector(cobalt_under_test_moniker_ + ":root");

    fuchsia::diagnostics::ClientSelectorConfiguration client_selector_config;
    client_selector_config.set_selectors(std::move(args));
    stream_parameters.set_client_selector_configuration(std::move(client_selector_config));
  }
  (*inspect_archive_)->StreamDiagnostics(std::move(stream_parameters), iterator.NewRequest());

  fuchsia::diagnostics::BatchIterator_GetNext_Result out_result;
  zx_status_t status = iterator->GetNext(&out_result);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to get the Inspect diagnostics data: " << status;
    return "";
  }
  if (out_result.is_err()) {
    FX_LOGS(ERROR) << "Inspect diagnostics Reader Error returned: "
                   << (out_result.err() == fuchsia::diagnostics::ReaderError::IO);
    return "";
  }
  if (out_result.response().batch.empty()) {
    FX_LOGS(ERROR) << "Inspect diagnostics returned empty response.";
    return "";
  }
  // Should be at most one component.
  ZX_ASSERT(out_result.response().batch.size() <= 1);
  if (!out_result.response().batch.empty()) {
    std::string json;
    fsl::StringFromVmo(out_result.response().batch[0].json(), &json);
    return json;
  }
  return "";
}

}  // namespace cobalt::testapp
