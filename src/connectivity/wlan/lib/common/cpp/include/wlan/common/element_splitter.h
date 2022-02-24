// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_ELEMENT_SPLITTER_H_
#define SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_ELEMENT_SPLITTER_H_

#include <lib/stdcompat/span.h>

#include <tuple>

#include <wlan/common/element_id.h>

namespace wlan {
namespace common {

class ElementIterator;

bool operator==(const ElementIterator& a, const ElementIterator& b);
bool operator!=(const ElementIterator& a, const ElementIterator& b);

class ElementIterator {
 public:
  friend bool wlan::common::operator==(const ElementIterator& a, const ElementIterator& b);
  friend bool wlan::common::operator!=(const ElementIterator& a, const ElementIterator& b);

  explicit ElementIterator(cpp20::span<const uint8_t> buffer);

  std::tuple<element_id::ElementId, cpp20::span<const uint8_t>> operator*() const;

  ElementIterator& operator++();

 private:
  cpp20::span<const uint8_t> remaining_;
};

class ElementSplitter {
 public:
  explicit ElementSplitter(cpp20::span<const uint8_t> buffer) : buffer_(buffer) {}

  ElementIterator begin() const { return ElementIterator(buffer_); }

  ElementIterator end() const { return ElementIterator(buffer_.subspan(buffer_.size())); }

 private:
  cpp20::span<const uint8_t> buffer_;
};

}  // namespace common
}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_ELEMENT_SPLITTER_H_
