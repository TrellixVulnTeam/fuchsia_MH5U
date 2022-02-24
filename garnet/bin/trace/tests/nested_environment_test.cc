// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Check that the test runs in a nested environment named "environment_name".

#include <lib/syslog/cpp/macros.h>
#include <lib/zx/job.h>
#include <lib/zx/object.h>
#include <lib/zx/process.h>
#include <zircon/status.h>

#include <string>

#include <task-utils/walker.h>

#include "garnet/bin/trace/tests/integration_test_utils.h"

// Trace integration test that verifies that it is executed
// under the nested environment created in trace record and returns with
// EXIT_SUCCESS if it is the case. Otherwise, returns with EXIT_FAILURE.
//
// The expected structure of jobs is:
//   j:...       environment_name             # the job for the test environment
//     j:...                                  # the default job for this package

namespace tracing::test {

namespace {

const char kNestedEnvironmentTestProviderName[] = "nested-environment-test";

// The name of the environment under which this trace benchmark will be
// executed. It is defined in trace_integration_tests.cc.
const char kEnvironmentName[] = "environment_name";

// A |TaskEnumerator| used to find the trace environment.
//
// After calling |WalkRootJobTree()| or |WalkJobTree()| the iteration will stop
// with status:
// - ZX_ERR_STOP if trace environment koid (given in the constructor) was
//   successfully found and the name matched the expected one.
// - ZX_OK if the given koid was not found.
// - ZX_ERR_BAD_STATE if the koid was found, but didn't match the expected one.
class Walker final : public TaskEnumerator {
 public:
  // Creates a new Walker. |trace_env_koid| is the koid the trace environment's
  // job is expected to have.
  Walker(zx_koid_t trace_env_koid) : trace_env_koid_(trace_env_koid) {}

  ~Walker() = default;

  // TaskEnumerator:
  zx_status_t OnJob(int /*depth*/, zx_handle_t task, zx_koid_t koid,
                    zx_koid_t /*parent_koid*/) override {
    if (koid != trace_env_koid_) {
      return ZX_OK;
    }

    char task_name[ZX_MAX_NAME_LEN];
    FX_CHECK(zx::unowned<zx::job>(task)->get_property(ZX_PROP_NAME, task_name, ZX_MAX_NAME_LEN) ==
             ZX_OK);
    std::string name(task_name);
    if (name == kEnvironmentName) {
      // The test finished successfully: the current task was the expected koid
      // and name. Stop the iteration over the jobs and processes by returning a
      // status different than ZK_OK.
      return ZX_ERR_STOP;
    }
    // The koid was found, but doesn't correspond to the trace environment. Quit
    // with an error.
    FX_LOGS(ERROR) << "Expected to find " << kEnvironmentName
                   << " as the parent process. Instead found: " << name;
    return ZX_ERR_BAD_STATE;
  }

 protected:
  // TaskEnumerator:
  bool has_on_job() const override { return true; }

 private:
  zx_koid_t trace_env_koid_;
};

bool RunNestedEnvironmentTest(size_t buffer_size_in_mb, const std::string& buffering_mode) {
  zx_info_handle_basic_t info;
  zx_status_t status =
      zx::job::default_job()->get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to get default job's information.";
    return EXIT_FAILURE;
  }

  // |info.related_koid| is the koid of the default job's parent object. The
  // Walker will use this koid to verify that it corresponds to the nested
  // environment.
  zx_koid_t trace_env_koid = info.related_koid;
  Walker walker(trace_env_koid);

  status = walker.WalkRootJobTree();
  if (status == ZX_OK) {
    FX_LOGS(ERROR) << "Failed to find parent job with koid " << trace_env_koid;
    return false;
  }

  if (status == ZX_ERR_BAD_STATE) {
    return false;
  }
  FX_CHECK(status == ZX_ERR_STOP) << status;
  return true;
}

bool VerifyNestedEnvironmentTest(size_t buffer_size_in_mb, const std::string& buffering_mode,
                                 const std::string& test_output_file) {
  // No verification of the generated trace is required for this test.
  return true;
}

const IntegrationTest kNestedEnvironmentIntegrationTest = {
    kNestedEnvironmentTestProviderName,
    &RunNestedEnvironmentTest,     // for run command
    &VerifyNestedEnvironmentTest,  // for verify command
};

}  // namespace

const IntegrationTest* LookupTest(const std::string& test_name) {
  if (test_name == kNestedEnvironmentTestProviderName) {
    return &kNestedEnvironmentIntegrationTest;
  }
  return nullptr;
}

}  // namespace tracing::test
