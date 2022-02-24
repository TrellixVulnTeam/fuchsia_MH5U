// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_SHARED_LOGGING_FILE_LINE_FUNCTION_H_
#define SRC_DEVELOPER_DEBUG_SHARED_LOGGING_FILE_LINE_FUNCTION_H_

#include <string>

namespace debug {

#define FROM_HERE ::debug::FileLineFunction(__FILE__, __LINE__, __FUNCTION__)
#define FROM_HERE_NO_FUNC ::debug::FileLineFunction(__FILE__, __LINE__)

class FileLineFunction {
 public:
  FileLineFunction();
  FileLineFunction(std::string file, int line, std::string function = "");
  ~FileLineFunction();

  bool is_valid() const { return !file_.empty() && line_ > 0; }

  const std::string& file() const { return file_; }
  int line() const { return line_; }
  const std::string& function() const { return function_; }

  std::string ToString() const;

 private:
  std::string file_;
  std::string function_;
  int line_ = 0;
};

// Comparison function for use in set and map.
bool operator<(const FileLineFunction& a, const FileLineFunction& b);

bool operator==(const FileLineFunction& a, const FileLineFunction& b);
bool operator!=(const FileLineFunction& a, const FileLineFunction& b);

}  // namespace debug

#endif  // SRC_DEVELOPER_DEBUG_SHARED_LOGGING_FILE_LINE_FUNCTION_H_
