// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#ifndef SRC_DEVELOPER_FORENSICS_EXCEPTIONS_JSON_UTILS_H_
#define SRC_DEVELOPER_FORENSICS_EXCEPTIONS_JSON_UTILS_H_

#include <set>
#include <string>

namespace forensics {
namespace exceptions {

// |content| is the string contents of the config file loaded for ExceptionBroker.
// It is expected to be an object and this function will attempt to load the "filters" array
// within it.
std::set<std::string> ExtractFilters(const std::string& content);

}  // namespace exceptions
}  // namespace forensics

#endif  // SRC_DEVELOPER_FORENSICS_EXCEPTIONS_JSON_UTILS_H_
