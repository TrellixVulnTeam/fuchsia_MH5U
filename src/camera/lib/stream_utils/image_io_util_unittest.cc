// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/lib/stream_utils/image_io_util.h"

#include <lib/syslog/cpp/macros.h>

#include <array>

#include <gtest/gtest.h>

#include "src/lib/files/file.h"

namespace camera {
namespace {

constexpr size_t kTestSize = 5;
constexpr std::array<uint8_t, kTestSize> kTestData = {1, 2, 3, 4, 5};
constexpr const char* kCacheDirPath = "/cache";

// Helper method to initialize an ImageIOUtil with one VmoBuffer filled with test data.
void CreateTestBufferCollection(fuchsia::sysmem::BufferCollectionInfo_2* buffer_collection_out) {
  zx::vmo vmo;

  zx_status_t status = zx::vmo::create(kTestSize, 0, &vmo);
  ASSERT_EQ(status, ZX_OK);
  // TODO(nzo): change this to use information from ImageFormat_2 instead.
  buffer_collection_out->settings.buffer_settings.size_bytes = kTestSize;

  status = vmo.write(kTestData.data(), 0, kTestSize);
  ASSERT_EQ(status, ZX_OK);

  buffer_collection_out->buffers[0].vmo = std::move(vmo);
  buffer_collection_out->buffer_count++;
}

TEST(ImageIOUtilTest, ConstructorSanity) {
  fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
  ASSERT_NO_FATAL_FAILURE(CreateTestBufferCollection(&buffer_collection));
  ASSERT_TRUE(ImageIOUtil::Create(&buffer_collection, kCacheDirPath).is_ok());
}

TEST(ImageIOUtilTest, ConstructorFailsWithEmptyBufferCollection) {
  fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
  ASSERT_TRUE(ImageIOUtil::Create(&buffer_collection, kCacheDirPath).is_error());
}

TEST(ImageIOUtilTest, RemoveFromDiskCorrectly) {
  fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
  ASSERT_NO_FATAL_FAILURE(CreateTestBufferCollection(&buffer_collection));
  // TODO(nzo): also requires a test to check for deleting nested files.
  auto image_io_util = ImageIOUtil::Create(&buffer_collection, kCacheDirPath).take_value();

  ASSERT_TRUE(files::WriteFile(image_io_util->GetFilepath(0),
                               reinterpret_cast<const char*>(kTestData.data()), kTestSize));

  zx_status_t status = image_io_util->DeleteImageData();
  ASSERT_EQ(status, ZX_OK);
}

TEST(ImageIOUtilTest, WriteToDiskCorrectly) {
  // TODO(nzo): also requires a test to check for writing multiple + nested files.
  fuchsia::sysmem::BufferCollectionInfo_2 buffer_collection;
  ASSERT_NO_FATAL_FAILURE(CreateTestBufferCollection(&buffer_collection));
  auto image_io_util = ImageIOUtil::Create(&buffer_collection, kCacheDirPath).take_value();

  zx_status_t status = image_io_util->WriteImageData(0);
  ASSERT_EQ(status, ZX_OK);

  ASSERT_TRUE(files::IsFile(image_io_util->GetFilepath(0)));

  std::vector<uint8_t> data;
  auto read_status = files::ReadFileToVector(image_io_util->GetFilepath(0), &data);
  ASSERT_TRUE(read_status);

  ASSERT_EQ(data.size(), kTestSize);
  for (uint32_t i = 0; i < kTestSize; ++i) {
    ASSERT_EQ(data[i], kTestData[i]);
  }
}

}  // namespace
}  // namespace camera
