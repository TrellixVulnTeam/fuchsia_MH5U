// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_A11Y_LIB_SEMANTICS_TESTS_SEMANTIC_TREE_PARSER_H_
#define SRC_UI_A11Y_LIB_SEMANTICS_TESTS_SEMANTIC_TREE_PARSER_H_

#include <fuchsia/accessibility/semantics/cpp/fidl.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "rapidjson/document.h"
#include "src/lib/json_parser/json_parser.h"

namespace accessibility_test {

class SemanticTreeParser {
 public:
  // This function parses file at file_path to create list of Semantic Tree
  // nodes. Nodes parsed from the file are appended to the given vector.
  // Function assumes passed pointer to point to a read std::vector<Node>.
  // Function returns false, if the parsing of the file fails.
  bool ParseSemanticTree(const std::string& file_path,
                         std::vector<fuchsia::accessibility::semantics::Node>* semantic_tree);

 private:
  json::JSONParser json_parser_;
};

}  // namespace accessibility_test

#endif  // SRC_UI_A11Y_LIB_SEMANTICS_TESTS_SEMANTIC_TREE_PARSER_H_
