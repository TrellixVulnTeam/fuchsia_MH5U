// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_COBALT_BIN_TESTAPP_TEST_CONSTANTS_H_
#define SRC_COBALT_BIN_TESTAPP_TEST_CONSTANTS_H_

#include <string>

#include "src/cobalt/bin/testapp/testapp_metrics_registry.cb.h"

namespace cobalt::testapp {

// error_occurred metric constants.
const uint32_t kErrorOccurredIndicesToUse[] = {0, 1, 2, 9};
const uint32_t kErrorOccurredInvalidIndex = 18;

// Common metric constants.
const std::string kApplicationComponentNames[] = {"audio_core", "logger", "scheduler", "scenic",
                                                  "unknown"};

// file_system_cache_misses metric constants.
const uint32_t kFileSystemCacheMissesIndices[] = {0, 1};
const std::string kFileSystemCacheMissesComponentNames[] = {"blobfs", "minfs", "thinfs", "",
                                                            "unknown"};
const int64_t kFileSystemCacheMissesCountMax = 2;

// update_duration metric constants.
const int32_t kUpdateDurationIndices[] = {0, 1, 2};
const std::string kUpdateDurationComponentNames[] = {"DownloadPayload", "VerifyUpdate", "",
                                                     "unknown"};
const int64_t kUpdateDurationValues[] = {-1, 0, 1, 2, 10, 37, 158, 702};

// game_frame_rate metric constants.
const int32_t kGameFrameRateIndices[] = {0, 1};
const std::string kGameFrameRateComponentNames[] = {"Forest", "City", "", "unknown"};
const float kGameFrameRateValues[] = {0.50, 1.23, 7.999, 8.0, 64.003, 415.235, 600.001};

// application_memory metric constants
const int32_t kApplicationMemoryIndices[] = {0, 1, 2};
const int64_t kApplicationMemoryValues[] = {0, 1000, 4000, 16000, 128000, 512000};

// power_usage metric constants.
const int32_t kPowerUsageIndices[] = {0, 1};
const int32_t kPowerUsageBuckets = 52;

// bandwidth_usage metric constants.
const int32_t kBandwidthUsageIndices[] = {0, 1};
const int64_t kBandwidthUsageBuckets = 22;

// features_active metric constants.
const int32_t kFeaturesActiveIndices[] = {0, 1, 2, 3, 9};
const int32_t kFeaturesActiveInvalidIndex = 20;

// connection_attempts metric constants.
const int32_t kConnectionAttemptsIndices[] = {0, 1};
const std::string kConnectionAttemptsComponentNames[] = {"HostA", "HostB", "HostC"};
const int kConnectionAttemptsNumWindowSizes = 2;

// streaming_time metric constants.
const int32_t kStreamingTimeIndices[] = {0, 1, 2};
const std::string kStreamingTimeComponentNames[] = {"ModuleA", "ModuleB", "ModuleC"};
const int kStreamingTimeNumWindowSizes = 2;

// update_duration_new metric constants.
const uint32_t kUpdateDurationNewErrorNameIndices[] = {0, 1, 2};
const uint32_t kUpdateDurationNewStageIndices[] = {0, 1};
const int64_t kUpdateDurationNewValues[] = {-1, 0, 1, 10, 702};

// streaming_time_new metric constants.
const uint32_t kStreamingTimeNewTypeIndices[] = {0, 1, 2};
const uint32_t kStreamingTimeNewModuleNameIndices[] = {0, 1};
const int64_t kStreamingTimeNewValues[] = {0, 100, 1000, 10000};

// application_memory_new metric constants
const uint32_t kApplicationMemoryNewMemoryTypeIndices[] = {0, 1, 2};
const uint32_t kApplicationMemoryNewApplicationNameIndices[] = {0, 1};
const int64_t kApplicationMemoryNewValues[] = {0, 1000, 4000, 16000, 128000, 512000};

// features_active_new metric constants.
const uint32_t kFeaturesActiveNewSkillIndices[] = {0, 1, 2, 3};
const int64_t kFeaturesActiveNewCounts[] = {1, 2, 10, 42};

// file_system_cache_misses_new metric constants.
const uint32_t kFileSystemCacheMissesNewEncryptionStateIndices[] = {0, 1};
const uint32_t kFileSystemCacheMissesNewFileSystemTypeIndices[] = {0, 1, 2};
const int64_t kFileSystemCacheMissesNewCounts[] = {0, 1, 100, 1000};

// connection_attempts_new metric constants.
const int32_t kConnectionAttemptsNewStatusIndices[] = {0, 1};
const int32_t kConnectionAttemptsNewHostNameIndices[] = {0, 1, 2};
const int64_t kConnectionAttemptsNewCounts[] = {0, 1, 100, 1000};

// power_usage_new metric constants.
const uint32_t kPowerUsageNewApplicationStateIndices[] = {0, 1};
const uint32_t kPowerUsageNewApplicationNameIndices[] = {0, 1};
const int32_t kPowerUsageNewBuckets = 52;

// bandwidth_usage_new metric constants.
const uint32_t kBandwidthUsageNewApplicationStateIndices[] = {0, 1};
const uint32_t kBandwidthUsageNewApplicationNameIndices[] = {0, 1};
const int64_t kBandwidthUsageNewBuckets = 22;

// error_occurred_new metric constants.
const uint32_t kErrorOccurredNewIndicesToUse[] = {0, 1, 2, 9};
const uint32_t kErrorOccurredNewCounts[] = {0, 1, 100, 1000};

// error_occurred_components metric constants.
const uint32_t kErrorOccurredComponentsStatusIndices[] = {0, 1, 2};

// The number of locally aggregated observations that should be generated for
// each locally aggregated report in the test_app2 project for a day, assuming
// that no events were logged for locally aggregated reports on that day.
//
// These numbers are calculated as follows:
//
// features_active_unique_devices: 20 UniqueActivesObservations
//                  (10 event codes * 2 window sizes)
// connection_attempts_per_device_count: 1 ReportParticipationObservation
// connection_attempts_per_device_histogram: 1 ReportParticipationObservation
// streaming_time_per_device_total: 1 ReportParticipationObservation
// streaming_time_per_device_histogram: 1 ReportParticipationObservation
std::map<std::pair<uint32_t, uint32_t>, uint64_t> kNumAggregatedObservations = {
    {{cobalt_registry::kFeaturesActiveMetricId,
      cobalt_registry::kFeaturesActiveFeaturesActiveUniqueDevicesReportId},
     20},
    {{cobalt_registry::kConnectionAttemptsMetricId,
      cobalt_registry::kConnectionAttemptsConnectionAttemptsPerDeviceCountReportId},
     1},
    {{cobalt_registry::kConnectionAttemptsMetricId,
      cobalt_registry::kConnectionAttemptsConnectionAttemptsPerDeviceHistogramReportId},
     1},
    {{cobalt_registry::kStreamingTimeMetricId,
      cobalt_registry::kStreamingTimeStreamingTimePerDeviceTotalReportId},
     1},
    {{cobalt_registry::kStreamingTimeMetricId,
      cobalt_registry::kStreamingTimeStreamingTimePerDeviceHistogramReportId},
     1}};

}  // namespace cobalt::testapp

#endif  // SRC_COBALT_BIN_TESTAPP_TEST_CONSTANTS_H_
