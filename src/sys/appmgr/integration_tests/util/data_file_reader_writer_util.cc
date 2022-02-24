// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/appmgr/integration_tests/util/data_file_reader_writer_util.h"

namespace component {
namespace testing {

using test::appmgr::integration::DataFileReaderWriterPtr;

fidl::StringPtr DataFileReaderWriterUtil::ReadFileSync(const DataFileReaderWriterPtr& util,
                                                       std::string path) {
  bool done = false;
  fidl::StringPtr result;
  util->ReadFile(path, [&](fidl::StringPtr contents) {
    done = true;
    result = contents;
  });
  RunLoopUntil([&] { return done; });
  return result;
}

zx_status_t DataFileReaderWriterUtil::WriteFileSync(const DataFileReaderWriterPtr& util,
                                                    std::string path, std::string contents) {
  bool done = false;
  zx_status_t result;
  util->WriteFile(path, contents, [&](zx_status_t write_result) {
    done = true;
    result = write_result;
  });
  RunLoopUntil([&] { return done; });
  return result;
}

fidl::StringPtr DataFileReaderWriterUtil::ReadTmpFileSync(const DataFileReaderWriterPtr& util,
                                                          std::string path) {
  bool done = false;
  fidl::StringPtr result;
  util->ReadTmpFile(path, [&](fidl::StringPtr contents) {
    done = true;
    result = contents;
  });
  RunLoopUntil([&] { return done; });
  return result;
}

zx_status_t DataFileReaderWriterUtil::WriteTmpFileSync(const DataFileReaderWriterPtr& util,
                                                       std::string path, std::string contents) {
  bool done = false;
  zx_status_t result;
  util->WriteTmpFile(path, contents, [&](zx_status_t write_result) {
    done = true;
    result = write_result;
  });
  RunLoopUntil([&] { return done; });
  return result;
}

}  // namespace testing
}  // namespace component
