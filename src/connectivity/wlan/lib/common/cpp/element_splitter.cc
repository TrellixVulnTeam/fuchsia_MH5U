// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <wlan/common/element.h>
#include <wlan/common/element_splitter.h>
#include <wlan/common/from_bytes.h>

namespace wlan {
namespace common {

static void SkipIfTooShort(cpp20::span<const uint8_t>* span) {
  const ElementHeader* header = FromBytes<ElementHeader>(*span);
  if (header == nullptr || header->len + sizeof(ElementHeader) > span->size()) {
    // If there is not enough remaining bytes to hold the full element,
    // move the iterator to the end
    *span = cpp20::span{span->data() + span->size(), static_cast<size_t>(0u)};
  }
}

ElementIterator::ElementIterator(cpp20::span<const uint8_t> buffer) : remaining_(buffer) {
  SkipIfTooShort(&remaining_);
}

std::tuple<element_id::ElementId, cpp20::span<const uint8_t>> ElementIterator::operator*() const {
  const ElementHeader* header = FromBytes<ElementHeader>(remaining_);
  ZX_ASSERT(header != nullptr);
  return {
      static_cast<element_id::ElementId>(header->id),
      remaining_.subspan(sizeof(ElementHeader), header->len),
  };
}

ElementIterator& ElementIterator::operator++() {
  const ElementHeader* header = FromBytes<ElementHeader>(remaining_);
  ZX_ASSERT(header != nullptr);
  remaining_ = remaining_.subspan(sizeof(ElementHeader) + header->len);
  SkipIfTooShort(&remaining_);
  return *this;
}

static bool SpansHaveSameEnd(cpp20::span<const uint8_t> a, cpp20::span<const uint8_t> b) {
  return a.data() + a.size() == b.data() + b.size();
}

bool operator==(const ElementIterator& a, const ElementIterator& b) {
  // Comparing iterators from different containers is not legal
  ZX_DEBUG_ASSERT(SpansHaveSameEnd(a.remaining_, b.remaining_));
  return a.remaining_.data() == b.remaining_.data();
}

bool operator!=(const ElementIterator& a, const ElementIterator& b) { return !(a == b); }

}  // namespace common
}  // namespace wlan
