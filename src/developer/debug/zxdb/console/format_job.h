// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_CONSOLE_FORMAT_JOB_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_CONSOLE_FORMAT_JOB_H_

#include <string>

#include "src/developer/debug/zxdb/client/job.h"
#include "src/developer/debug/zxdb/console/output_buffer.h"

namespace zxdb {

class ConsoleContext;

OutputBuffer FormatJob(ConsoleContext* context, const Job* job);

// Formats all jobs as a table. The number of spaces given by |indent| will be
// added to the left.
OutputBuffer FormatJobList(ConsoleContext* context, int indent = 0);

const char* JobStateToString(Job::State state);

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_CONSOLE_FORMAT_JOB_H_
