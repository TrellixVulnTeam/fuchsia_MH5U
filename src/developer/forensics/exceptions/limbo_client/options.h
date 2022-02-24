// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_EXCEPTIONS_LIMBO_CLIENT_OPTIONS_H_
#define SRC_DEVELOPER_FORENSICS_EXCEPTIONS_LIMBO_CLIENT_OPTIONS_H_

#include <zircon/status.h>

#include <string>

namespace forensics {
namespace exceptions {

class LimboClient;

// Returns |nullptr| on error.
using OptionFunction = zx_status_t (*)(LimboClient*, const std::vector<const char*>& argv,
                                       std::ostream&);
OptionFunction ParseArgs(int argc, const char* argv[], std::ostream&);

}  // namespace exceptions
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_EXCEPTIONS_LIMBO_CLIENT_OPTIONS_H_
