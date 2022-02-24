// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/fsl/types/type_converters.h"

namespace fidl {

std::string TypeConverter<std::string, fidl::StringPtr>::Convert(const fidl::StringPtr& value) {
  std::string result;
  if (value.has_value()) {
    result = *value;
  }
  return result;
}

fidl::StringPtr TypeConverter<fidl::StringPtr, std::string>::Convert(const std::string& value) {
  return std::string(value.c_str(), value.size());
}

}  // namespace fidl
