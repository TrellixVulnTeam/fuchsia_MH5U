// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_PERR_DESTINATION_PARSER_H_
#define SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_PERR_DESTINATION_PARSER_H_

#include <lib/stdcompat/span.h>

#include <wlan/common/buffer_reader.h>
#include <wlan/common/element.h>

namespace wlan {
namespace common {

struct ParsedPerrDestination {
  const PerrPerDestinationHeader* header;
  const MacAddr* ext_addr;  // null if absent
  const PerrPerDestinationTail* tail;
};

// Can be used to parse the destination fields of a PERR element.
// Example usage:
//
//     if (auto perr = ParsePerr(raw_element_body)) {
//         PerrDestinationParser parser(perr->destinations);
//         for (size_t i = 0; i < perr->header->num_destinations; ++i) {
//             auto dest = parser.Next();
//             if (!dest.has_value()) {
//                  return ...; // element is too short
//             }
//             ... handle *dest ...
//         }
//         if (parser->ExtraBytesLeft()) {
//             return ...; // element has extra bytes at the end
//         }
//     }
//
class PerrDestinationParser {
 public:
  explicit PerrDestinationParser(cpp20::span<const uint8_t> bytes);

  std::optional<ParsedPerrDestination> Next();

  bool ExtraBytesLeft() const;

 private:
  BufferReader reader_;
  bool incomplete_read_ = false;
};

}  // namespace common
}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_PERR_DESTINATION_PARSER_H_
