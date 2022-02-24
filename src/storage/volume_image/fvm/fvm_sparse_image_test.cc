// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/volume_image/fvm/fvm_sparse_image.h"

#include <lib/fit/defer.h>
#include <lib/fit/function.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/storage/fvm/format.h"
#include "src/storage/fvm/fvm_sparse.h"
#include "src/storage/fvm/sparse_reader.h"
#include "src/storage/volume_image/address_descriptor.h"
#include "src/storage/volume_image/fvm/fvm_descriptor.h"
#include "src/storage/volume_image/fvm/options.h"
#include "src/storage/volume_image/options.h"
#include "src/storage/volume_image/utils/block_utils.h"
#include "src/storage/volume_image/utils/lz4_compressor.h"
#include "src/storage/volume_image/utils/reader.h"
#include "src/storage/volume_image/utils/writer.h"
#include "src/storage/volume_image/volume_descriptor.h"

namespace storage::volume_image {
namespace {

TEST(GetImageFlagsTest, MapsLz4CompressionCorrectly) {
  FvmOptions options;
  options.compression.schema = CompressionSchema::kLz4;

  auto flag = fvm_sparse_internal::GetImageFlags(options);
  EXPECT_EQ(flag & fvm::kSparseFlagLz4, fvm::kSparseFlagLz4);
}

TEST(GetImageFlagsTest, MapsNoCompressionCorrectly) {
  FvmOptions options;
  options.compression.schema = CompressionSchema::kNone;

  auto flag = fvm_sparse_internal::GetImageFlags(options);
  EXPECT_EQ(flag, 0u);
}

TEST(GetImageFlagsTest, MapsUnknownCompressionCorrectly) {
  FvmOptions options;
  options.compression.schema = static_cast<CompressionSchema>(-1);

  auto flag = fvm_sparse_internal::GetImageFlags(options);
  EXPECT_EQ(flag, 0u);
}

TEST(GetPartitionFlagsTest, MapsEncryptionCorrectly) {
  VolumeDescriptor descriptor;
  descriptor.encryption = EncryptionType::kZxcrypt;
  AddressDescriptor address;
  Partition partition(descriptor, address, nullptr);

  auto flag = fvm_sparse_internal::GetPartitionFlags(partition);
  EXPECT_EQ(flag & fvm::kSparseFlagZxcrypt, fvm::kSparseFlagZxcrypt);
}

TEST(GetPartitionFlagsTest, NoZeroFillIsSetWhenNoFillOptionsIsProvided) {
  VolumeDescriptor descriptor;
  AddressDescriptor address;
  address.mappings.push_back({});
  address.mappings.front().options[EnumAsString(AddressMapOption::kFill)] = 0;

  Partition partition(descriptor, address, nullptr);

  auto flag = fvm_sparse_internal::GetPartitionFlags(partition);
  EXPECT_EQ(flag & fvm::kSparseFlagZeroFillNotRequired, 0u);
}

TEST(GetPartitionFlagsTest, FlagMapsNoEncryptionCorrectly) {
  VolumeDescriptor descriptor = {};
  descriptor.encryption = EncryptionType::kNone;
  AddressDescriptor address = {};
  Partition partition(descriptor, address, nullptr);

  auto flag = fvm_sparse_internal::GetPartitionFlags(partition);
  EXPECT_EQ(flag & fvm::kSparseFlagZxcrypt, 0u);
}

TEST(GetPartitionFlagsTest, MapsUnknownEncryptionCorrectly) {
  VolumeDescriptor descriptor = {};
  AddressDescriptor address = {};
  Partition partition(descriptor, address, nullptr);

  auto expected_flag = fvm_sparse_internal::GetPartitionFlags(partition);
  descriptor.encryption = static_cast<EncryptionType>(-1);

  auto flag = fvm_sparse_internal::GetPartitionFlags(partition);
  EXPECT_EQ(flag, expected_flag);
}

constexpr std::string_view kSerializedVolumeImage1 = R"(
{
    "volume": {
      "magic": 11602964,
      "instance_guid": "04030201-0605-0807-1009-111213141516",
      "type_guid": "A4A3A2A1-B6B5-C8C7-D0D1-E0E1E2E3E4E5",
      "name": "partition-1",
      "block_size": 16,
      "encryption_type": "ENCRYPTION_TYPE_ZXCRYPT",
      "options" : [
        "OPTION_NONE",
        "OPTION_EMPTY"
      ]
    },
    "address": {
        "magic": 12526821592682033285,
        "mappings": [
          {
            "source": 20,
            "target": 8192,
            "count": 48
          },
          {
            "source": 180,
            "target": 0,
            "count": 52
          },
          {
            "source": 190,
            "target": 16384,
            "count": 20
          }
        ]
    }
})";

constexpr std::string_view kSerializedVolumeImage2 = R"(
{
    "volume": {
      "magic": 11602964,
      "instance_guid": "04030201-0605-0807-1009-111213141517",
      "type_guid": "A4A3A2A1-B6B5-C8C7-D0D1-E0E1E2E3E4E6",
      "name": "partition-2",
      "block_size": 32,
      "encryption_type": "ENCRYPTION_TYPE_ZXCRYPT",
      "options" : [
        "OPTION_NONE",
        "OPTION_EMPTY"
      ]
    },
    "address": {
        "magic": 12526821592682033285,
        "mappings": [
          {
            "source": 25,
            "target": 0,
            "count": 30
          },
          {
            "source": 250,
            "target": 327680,
            "count": 61
          }
        ]
    }
})";

// This struct represents a typed version of how the serialized contents of
// |SerializedVolumeImage1| and |SerializedVolumeImage2| would look.
struct SerializedSparseImage {
  fvm::SparseImage header;
  struct {
    fvm::PartitionDescriptor descriptor;
    fvm::ExtentDescriptor extents[3];
  } partition_1 __attribute__((packed));
  struct {
    fvm::PartitionDescriptor descriptor;
    fvm::ExtentDescriptor extents[2];
  } partition_2 __attribute__((packed));
  uint8_t extent_data[211];
} __attribute__((packed));

FvmDescriptor MakeFvmDescriptor() {
  FvmOptions options;
  options.compression.schema = CompressionSchema::kLz4;
  options.max_volume_size = 20 * (1 << 20);
  options.slice_size = fvm::kBlockSize;

  auto partition_1_result = Partition::Create(kSerializedVolumeImage1, nullptr);
  EXPECT_TRUE(partition_1_result.is_ok()) << partition_1_result.error();
  auto partition_2_result = Partition::Create(kSerializedVolumeImage2, nullptr);
  EXPECT_TRUE(partition_2_result.is_ok()) << partition_2_result.error();

  auto descriptor_result = FvmDescriptor::Builder()
                               .SetOptions(options)
                               .AddPartition(partition_1_result.take_value())
                               .AddPartition(partition_2_result.take_value())
                               .Build();
  EXPECT_TRUE(descriptor_result.is_ok()) << descriptor_result.error();
  return descriptor_result.take_value();
}

TEST(FvmSparseGenerateHeaderTest, MatchesFvmDescriptor) {
  FvmDescriptor descriptor = MakeFvmDescriptor();
  auto header = fvm_sparse_internal::GenerateHeader(descriptor);

  EXPECT_EQ(header.partition_count, descriptor.partitions().size());
  EXPECT_EQ(header.maximum_disk_size, descriptor.options().max_volume_size.value());
  EXPECT_EQ(descriptor.options().slice_size, header.slice_size);
  EXPECT_EQ(header.magic, fvm::kSparseFormatMagic);
  EXPECT_EQ(header.version, fvm::kSparseFormatVersion);
  EXPECT_EQ(header.flags, fvm_sparse_internal::GetImageFlags(descriptor.options()));

  uint64_t extent_count = 0;
  for (const auto& partition : descriptor.partitions()) {
    extent_count += partition.address().mappings.size();
  }
  uint64_t expected_header_length = sizeof(fvm::SparseImage) +
                                    sizeof(fvm::PartitionDescriptor) * header.partition_count +
                                    sizeof(fvm::ExtentDescriptor) * extent_count;
  EXPECT_EQ(header.header_length, expected_header_length);
}

TEST(FvmSparGeneratePartitionEntryTest, MatchesPartition) {
  FvmDescriptor descriptor = MakeFvmDescriptor();
  const auto& partition = *descriptor.partitions().begin();

  auto partition_entry_result =
      fvm_sparse_internal::GeneratePartitionEntry(descriptor.options().slice_size, partition);
  ASSERT_TRUE(partition_entry_result.is_ok()) << partition_entry_result.error();
  auto partition_entry = partition_entry_result.take_value();

  EXPECT_EQ(fvm::kPartitionDescriptorMagic, partition_entry.descriptor.magic);
  EXPECT_TRUE(memcmp(partition.volume().type.data(), partition_entry.descriptor.type,
                     partition.volume().type.size()) == 0);
  EXPECT_TRUE(memcmp(partition.volume().name.data(), partition_entry.descriptor.name,
                     partition.volume().name.size()) == 0);
  EXPECT_EQ(partition_entry.descriptor.flags, fvm_sparse_internal::GetPartitionFlags(partition));
  EXPECT_EQ(partition.address().mappings.size(), partition_entry.descriptor.extent_count);
}

TEST(FvmSparseCalculateUncompressedImageSizeTest, EmptyDescriptorIsHeaderSize) {
  FvmDescriptor descriptor;
  EXPECT_EQ(sizeof(fvm::SparseImage),
            fvm_sparse_internal::CalculateUncompressedImageSize(descriptor));
}

TEST(FvmSparseCalculateUncompressedImageSizeTest, ParitionsAndExtentsMatchesSerializedContent) {
  FvmDescriptor descriptor = MakeFvmDescriptor();
  uint64_t header_length = fvm_sparse_internal::GenerateHeader(descriptor).header_length;
  uint64_t data_length = 0;
  for (const auto& partition : descriptor.partitions()) {
    for (const auto& mapping : partition.address().mappings) {
      data_length += mapping.count;
    }
  }

  EXPECT_EQ(fvm_sparse_internal::CalculateUncompressedImageSize(descriptor),
            header_length + data_length);
}

// Fake implementation for reader that delegates operations to a function after performing bound
// check.
class FakeReader : public Reader {
 public:
  explicit FakeReader(
      fit::function<fpromise::result<void, std::string>(uint64_t, cpp20::span<uint8_t>)> filler)
      : filler_(std::move(filler)) {}

  uint64_t length() const override { return 0; }

  fpromise::result<void, std::string> Read(uint64_t offset,
                                           cpp20::span<uint8_t> buffer) const final {
    return filler_(offset, buffer);
  }

 private:
  fit::function<fpromise::result<void, std::string>(uint64_t offset, cpp20::span<uint8_t>)> filler_;
};

// Fake writer implementations that writes into a provided buffer.
class BufferWriter : public Writer {
 public:
  explicit BufferWriter(cpp20::span<uint8_t> buffer) : buffer_(buffer) {}

  fpromise::result<void, std::string> Write(uint64_t offset,
                                            cpp20::span<const uint8_t> buffer) final {
    if (offset > buffer_.size() || offset + buffer.size() > buffer_.size()) {
      return fpromise::error("BufferWriter: Out of Range. Offset at " + std::to_string(offset) +
                             " and byte count is " + std::to_string(buffer.size()) +
                             " max size is " + std::to_string(buffer_.size()) + ".");
    }
    memcpy(buffer_.data() + offset, buffer.data(), buffer.size());
    return fpromise::ok();
  }

 private:
  cpp20::span<uint8_t> buffer_;
};

template <int shift>
fpromise::result<void, std::string> GetContents(uint64_t offset, cpp20::span<uint8_t> buffer) {
  for (uint64_t index = 0; index < buffer.size(); ++index) {
    buffer[index] = (offset + index + shift) % sizeof(uint64_t);
  }
  return fpromise::ok();
}

class SerializedImageContainer {
 public:
  SerializedImageContainer() : serialized_image_(new SerializedSparseImage()), writer_(AsSpan()) {}

  cpp20::span<uint8_t> AsSpan() {
    return cpp20::span<uint8_t>(reinterpret_cast<uint8_t*>(serialized_image_.get()),
                                sizeof(SerializedSparseImage));
  }

  const SerializedSparseImage& serialized_image() const { return *serialized_image_; }

  SerializedSparseImage& serialized_image() { return *serialized_image_; }

  BufferWriter& writer() { return writer_; }

  std::vector<cpp20::span<const uint8_t>> PartitionExtents(size_t index) {
    auto view = cpp20::span(serialized_image_->extent_data);
    if (index == 0) {
      return {view.subspan(0, 48), view.subspan(48, 52), view.subspan(100, 20)};
    }
    return {view.subspan(120, 30), view.subspan(150, 61)};
  }

 private:
  std::unique_ptr<SerializedSparseImage> serialized_image_ = nullptr;
  BufferWriter writer_;
};

FvmDescriptor MakeFvmDescriptorWithOptions(const FvmOptions& options) {
  auto partition_1_result =
      Partition::Create(kSerializedVolumeImage1, std::make_unique<FakeReader>(GetContents<1>));

  auto partition_2_result =
      Partition::Create(kSerializedVolumeImage2, std::make_unique<FakeReader>(GetContents<2>));

  FvmDescriptor::Builder builder;
  auto result = builder.SetOptions(options)
                    .AddPartition(partition_2_result.take_value())
                    .AddPartition(partition_1_result.take_value())
                    .Build();
  return result.take_value();
}

FvmOptions MakeOptions(uint64_t slice_size, CompressionSchema schema) {
  FvmOptions options;
  options.compression.schema = schema;
  options.max_volume_size = 20 * (1 << 20);
  options.slice_size = slice_size;

  return options;
}

std::vector<fvm_sparse_internal::PartitionEntry> GetExpectedPartitionEntries(
    const FvmDescriptor& descriptor, uint64_t slice_size) {
  std::vector<fvm_sparse_internal::PartitionEntry> partitions;
  for (const auto& partition : descriptor.partitions()) {
    auto partition_entry_result =
        fvm_sparse_internal::GeneratePartitionEntry(slice_size, partition);
    partitions.push_back(partition_entry_result.take_value());
  }
  return partitions;
}

// The testing::Field matchers lose track of alignment (because they involve casts to pointer types)
// and so we can end up relying on undefined-behaviour.  We can avoid that by wrapping the packed
// structures and aligning them.  Things might have been a little easier if fvm::SparseImage was a
// multiple of 8 bytes since it would have meant that fvm::PartitionDescriptor was 8 byte aligned
// when it immediately follows the header, but we are where we are.
template <typename T>
struct Aligner {
  struct alignas(8) Aligned : T {};

  Aligned operator()(const T& in) {
    Aligned out;
    memcpy(&out, &in, sizeof(T));
    return out;
  }
};

auto HeaderEq(const fvm::SparseImage& expected_header) {
  using Header = fvm::SparseImage;
  return testing::ResultOf(
      Aligner<Header>(),
      testing::AllOf(
          testing::Field(&Header::header_length, testing::Eq(expected_header.header_length)),
          testing::Field(&Header::flags, testing::Eq(expected_header.flags)),
          testing::Field(&Header::magic, testing::Eq(expected_header.magic)),
          testing::Field(&Header::partition_count, testing::Eq(expected_header.partition_count)),
          testing::Field(&Header::slice_size, testing::Eq(expected_header.slice_size)),
          testing::Field(&Header::maximum_disk_size,
                         testing::Eq(expected_header.maximum_disk_size)),
          testing::Field(&Header::version, testing::Eq(expected_header.version))));
}

auto PartitionDescriptorEq(const fvm::PartitionDescriptor& expected_descriptor) {
  using fvm::PartitionDescriptor;
  return testing::ResultOf(
      Aligner<PartitionDescriptor>(),
      testing::AllOf(
          testing::Field(&PartitionDescriptor::magic, testing::Eq(expected_descriptor.magic)),
          testing::Field(&PartitionDescriptor::flags, testing::Eq(expected_descriptor.flags)),
          testing::Field(&PartitionDescriptor::name,
                         testing::ElementsAreArray(expected_descriptor.name)),
          testing::Field(&PartitionDescriptor::type,
                         testing::ElementsAreArray(expected_descriptor.type))));
}

auto PartitionDescriptorMatchesEntry(
    const fvm_sparse_internal::PartitionEntry& expected_descriptor) {
  return PartitionDescriptorEq(expected_descriptor.descriptor);
}

[[maybe_unused]] auto ExtentDescriptorEq(const fvm::ExtentDescriptor& expected_descriptor) {
  using fvm::ExtentDescriptor;
  return testing::ResultOf(
      Aligner<ExtentDescriptor>(),
      testing::AllOf(
          testing::Field(&ExtentDescriptor::magic, testing::Eq(expected_descriptor.magic)),
          testing::Field(&ExtentDescriptor::slice_start,
                         testing::Eq(expected_descriptor.slice_start)),
          testing::Field(&ExtentDescriptor::slice_count,
                         testing::Eq(expected_descriptor.slice_count)),
          testing::Field(&ExtentDescriptor::extent_length,
                         testing::Eq(expected_descriptor.extent_length))));
}

MATCHER(ExtentDescriptorsAreEq, "Compares to Extent Descriptors") {
  auto [a, b] = arg;
  return testing::ExplainMatchResult(ExtentDescriptorEq(b), a, result_listener);
}

auto ExtentDescriptorsMatchesEntry(const fvm_sparse_internal::PartitionEntry& expected_entry) {
  return testing::Pointwise(ExtentDescriptorsAreEq(), expected_entry.extents);
}

TEST(FvmSparseWriteImageTest, DataUncompressedCompliesWithFormat) {
  SerializedImageContainer container;
  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kNone));
  auto header = fvm_sparse_internal::GenerateHeader(descriptor);

  std::vector<fvm_sparse_internal::PartitionEntry> expected_partition_entries =
      GetExpectedPartitionEntries(descriptor, descriptor.options().slice_size);

  auto write_result = FvmSparseWriteImage(descriptor, &container.writer());
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();
  ASSERT_EQ(write_result.value(), fvm_sparse_internal::CalculateUncompressedImageSize(descriptor));

  EXPECT_THAT(container.serialized_image().header, HeaderEq(header));

  // Check partition and entry descriptors.
  auto it = descriptor.partitions().begin();
  const auto& partition_1 = *it++;
  auto partition_1_entry_result =
      fvm_sparse_internal::GeneratePartitionEntry(descriptor.options().slice_size, partition_1);
  ASSERT_TRUE(partition_1_entry_result.is_ok()) << partition_1_entry_result.error();
  fvm_sparse_internal::PartitionEntry partition_1_entry = partition_1_entry_result.take_value();
  EXPECT_THAT(container.serialized_image().partition_1.descriptor,
              PartitionDescriptorMatchesEntry(partition_1_entry));
  EXPECT_THAT(container.serialized_image().partition_1.extents,
              ExtentDescriptorsMatchesEntry(partition_1_entry));

  const auto& partition_2 = *it++;
  auto partition_2_entry_result =
      fvm_sparse_internal::GeneratePartitionEntry(descriptor.options().slice_size, partition_2);
  ASSERT_TRUE(partition_2_entry_result.is_ok()) << partition_2_entry_result.error();
  fvm_sparse_internal::PartitionEntry partition_2_entry = partition_2_entry_result.take_value();
  EXPECT_THAT(container.serialized_image().partition_2.descriptor,
              PartitionDescriptorMatchesEntry(partition_2_entry));
  EXPECT_THAT(container.serialized_image().partition_2.extents,
              ExtentDescriptorsMatchesEntry(partition_2_entry));

  // Check data is correct.
  uint64_t partition_index = 0;
  for (const auto& partition : descriptor.partitions()) {
    auto read_content = partition_index == 0 ? GetContents<1> : GetContents<2>;
    std::vector<uint8_t> expected_content;
    uint64_t extent_index = 0;
    auto extents = container.PartitionExtents(partition_index);
    for (const auto& mapping : partition.address().mappings) {
      expected_content.resize(mapping.count, 0);
      ASSERT_TRUE(read_content(mapping.source, expected_content).is_ok());
      EXPECT_THAT(extents[extent_index], testing::ElementsAreArray(expected_content));
      extent_index++;
    }
    partition_index++;
  }
}

TEST(FvmSparseWriteImageTest, DataCompressedCompliesWithFormat) {
  SerializedImageContainer container;
  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kLz4));
  auto header = fvm_sparse_internal::GenerateHeader(descriptor);

  std::vector<fvm_sparse_internal::PartitionEntry> expected_partition_entries =
      GetExpectedPartitionEntries(descriptor, descriptor.options().slice_size);

  Lz4Compressor compressor = Lz4Compressor::Create(descriptor.options().compression).take_value();
  auto write_result = FvmSparseWriteImage(descriptor, &container.writer(), &compressor);
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();
  ASSERT_LE(write_result.value(), fvm_sparse_internal::CalculateUncompressedImageSize(descriptor));

  EXPECT_THAT(container.serialized_image().header, HeaderEq(header));
  uint64_t compressed_extents_size = write_result.value() - header.header_length;

  // Check partition and entry descriptors.
  auto it = descriptor.partitions().begin();
  const auto& partition_1 = *it++;
  auto partition_1_entry_result =
      fvm_sparse_internal::GeneratePartitionEntry(descriptor.options().slice_size, partition_1);
  ASSERT_TRUE(partition_1_entry_result.is_ok()) << partition_1_entry_result.error();
  fvm_sparse_internal::PartitionEntry partition_1_entry = partition_1_entry_result.take_value();
  EXPECT_THAT(container.serialized_image().partition_1.descriptor,
              PartitionDescriptorMatchesEntry(partition_1_entry));
  EXPECT_THAT(container.serialized_image().partition_1.extents,
              ExtentDescriptorsMatchesEntry(partition_1_entry));

  const auto& partition_2 = *it++;
  auto partition_2_entry_result =
      fvm_sparse_internal::GeneratePartitionEntry(descriptor.options().slice_size, partition_2);
  ASSERT_TRUE(partition_2_entry_result.is_ok()) << partition_2_entry_result.error();
  fvm_sparse_internal::PartitionEntry partition_2_entry = partition_2_entry_result.take_value();
  EXPECT_THAT(container.serialized_image().partition_2.descriptor,
              PartitionDescriptorMatchesEntry(partition_2_entry));
  EXPECT_THAT(container.serialized_image().partition_2.extents,
              ExtentDescriptorsMatchesEntry(partition_2_entry));

  // Decompress extent data.
  LZ4F_decompressionContext_t decompression_context = nullptr;
  auto release_decompressor = fit::defer([&decompression_context]() {
    if (decompression_context != nullptr) {
      LZ4F_freeDecompressionContext(decompression_context);
    }
  });
  auto create_return_code = LZ4F_createDecompressionContext(&decompression_context, LZ4F_VERSION);
  ASSERT_FALSE(LZ4F_isError(create_return_code)) << LZ4F_getErrorName(create_return_code);

  std::vector<uint8_t> decompressed_extents(
      sizeof(SerializedSparseImage) - offsetof(SerializedSparseImage, extent_data), 0);
  size_t decompressed_byte_count = decompressed_extents.size();
  size_t consumed_compressed_bytes = compressed_extents_size;
  auto decompress_return_code = LZ4F_decompress(
      decompression_context, decompressed_extents.data(), &decompressed_byte_count,
      container.serialized_image().extent_data, &consumed_compressed_bytes, nullptr);
  ASSERT_FALSE(LZ4F_isError(decompress_return_code));
  ASSERT_EQ(decompressed_byte_count, decompressed_extents.size());
  ASSERT_EQ(consumed_compressed_bytes, compressed_extents_size);

  // Copy the uncompressed data over the compressed data.
  memcpy(container.serialized_image().extent_data, decompressed_extents.data(),
         decompressed_extents.size());
  uint64_t partition_index = 0;
  for (const auto& partition : descriptor.partitions()) {
    auto read_content = partition_index == 0 ? GetContents<1> : GetContents<2>;
    std::vector<uint8_t> expected_content;
    uint64_t extent_index = 0;
    auto extents = container.PartitionExtents(partition_index);
    for (const auto& mapping : partition.address().mappings) {
      expected_content.resize(mapping.count, 0);
      ASSERT_TRUE(read_content(mapping.source, expected_content).is_ok());
      EXPECT_THAT(extents[extent_index], testing::ElementsAreArray(expected_content));
      extent_index++;
    }
    partition_index++;
  }
}

class ErrorWriter final : public Writer {
 public:
  ErrorWriter(uint64_t error_offset, std::string_view error)
      : error_(error), error_offset_(error_offset) {}
  ~ErrorWriter() final = default;

  fpromise::result<void, std::string> Write(
      [[maybe_unused]] uint64_t offset, [[maybe_unused]] cpp20::span<const uint8_t> buffer) final {
    if (offset >= error_offset_) {
      return fpromise::error(error_);
    }
    return fpromise::ok();
  }

 private:
  std::string error_;
  uint64_t error_offset_;
};

constexpr std::string_view kWriteError = "Write Error";
constexpr std::string_view kReadError = "Read Error";

TEST(FvmSparseWriteImageTest, WithReadErrorIsError) {
  FvmOptions options;
  options.compression.schema = CompressionSchema::kNone;
  options.max_volume_size = 20 * (1 << 20);
  options.slice_size = 8192;

  auto partition_1_result = Partition::Create(
      kSerializedVolumeImage1,
      std::make_unique<FakeReader>(
          []([[maybe_unused]] uint64_t offset, [[maybe_unused]] cpp20::span<uint8_t> buffer) {
            return fpromise::error(std::string(kReadError));
          }));
  ASSERT_TRUE(partition_1_result.is_ok()) << partition_1_result.error();

  FvmDescriptor::Builder builder;
  auto result = builder.SetOptions(options).AddPartition(partition_1_result.take_value()).Build();
  ASSERT_TRUE(result.is_ok()) << result.error();
  auto descriptor = result.take_value();

  // We only added a single partition, so, data should be at this offset.
  ErrorWriter writer(/**error_offset=**/ offsetof(SerializedSparseImage, partition_2), kWriteError);
  auto write_result = FvmSparseWriteImage(descriptor, &writer);
  ASSERT_TRUE(write_result.is_error());
  ASSERT_EQ(kReadError, write_result.error());
}

TEST(FvmSparseWriteImageTest, WithWriteErrorIsError) {
  ErrorWriter writer(/**error_offset=**/ 0, kWriteError);
  FvmOptions options;
  options.compression.schema = CompressionSchema::kNone;
  options.max_volume_size = 20 * (1 << 20);
  options.slice_size = 8192;

  auto partition_1_result =
      Partition::Create(kSerializedVolumeImage1, std::make_unique<FakeReader>(&GetContents<0>));
  ASSERT_TRUE(partition_1_result.is_ok()) << partition_1_result.error();

  FvmDescriptor::Builder builder;
  auto result = builder.SetOptions(options).AddPartition(partition_1_result.take_value()).Build();
  ASSERT_TRUE(result.is_ok()) << result.error();
  auto descriptor = result.take_value();

  auto write_result = FvmSparseWriteImage(descriptor, &writer);
  ASSERT_TRUE(write_result.is_error());
  ASSERT_EQ(kWriteError, write_result.error());
}

class BufferReader final : public Reader {
 public:
  template <typename T>
  BufferReader(uint64_t offset, const T* data)
      : image_offset_(offset), image_buffer_(reinterpret_cast<const uint8_t*>(data), sizeof(T)) {
    assert(image_buffer_.data() != nullptr);
  }

  template <typename T>
  BufferReader(uint64_t offset, const T* data, uint64_t length)
      : image_offset_(offset),
        image_buffer_(reinterpret_cast<const uint8_t*>(data), sizeof(T)),
        length_(offset + length) {
    assert(image_buffer_.data() != nullptr);
  }

  uint64_t length() const final { return length_; }

  fpromise::result<void, std::string> Read(uint64_t offset,
                                           cpp20::span<uint8_t> buffer) const final {
    // if no overlap zero the buffer.
    if (offset + buffer.size() < image_offset_ || offset > image_offset_ + image_buffer_.size()) {
      std::fill(buffer.begin(), buffer.end(), 0);
      return fpromise::ok();
    }

    size_t zeroed_bytes = 0;  // Zero anything before the header start.
    if (offset < image_offset_) {
      size_t distance_to_header = image_offset_ - offset;
      zeroed_bytes = std::min(distance_to_header, buffer.size());
      std::fill(buffer.begin(), buffer.begin() + zeroed_bytes, 0);
    }

    uint64_t copied_bytes = 0;
    if (zeroed_bytes < buffer.size()) {
      size_t distance_from_start = (image_offset_ > offset) ? 0 : offset - image_offset_;
      copied_bytes =
          std::min(buffer.size() - zeroed_bytes, image_buffer_.size() - distance_from_start);
      memcpy(buffer.data() + zeroed_bytes, image_buffer_.subspan(distance_from_start).data(),
             copied_bytes);
    }

    if (zeroed_bytes + copied_bytes < buffer.size()) {
      std::fill(buffer.begin() + zeroed_bytes + copied_bytes, buffer.end(), 0);
    }

    return fpromise::ok();
  }

 private:
  uint64_t image_offset_ = 0;
  cpp20::span<const uint8_t> image_buffer_;
  uint64_t length_ = std::numeric_limits<uint64_t>::max();
};

TEST(GeHeaderTest, FromReaderWithBadMagicIsError) {
  constexpr uint64_t kImageOffset = 12345678;
  fvm::SparseImage header = {};
  header.magic = fvm::kSparseFormatMagic - 1;
  header.version = fvm::kSparseFormatVersion;
  header.flags = fvm::kSparseFlagAllValid;
  header.header_length = sizeof(fvm::SparseImage);
  header.slice_size = 2 << 20;

  BufferReader reader(kImageOffset, &header);

  ASSERT_TRUE(fvm_sparse_internal::GetHeader(kImageOffset, reader).is_error());
}

TEST(GetHeaderTest, FromReaderWithVersionMismatchIsError) {
  constexpr uint64_t kImageOffset = 12345678;
  fvm::SparseImage header = {};
  header.magic = fvm::kSparseFormatMagic;
  header.version = fvm::kSparseFormatVersion - 1;
  header.flags = fvm::kSparseFlagAllValid;
  header.header_length = sizeof(fvm::SparseImage);
  header.slice_size = 2 << 20;

  BufferReader reader(kImageOffset, &header);

  ASSERT_TRUE(fvm_sparse_internal::GetHeader(kImageOffset, reader).is_error());
}

TEST(GetHeaderTest, FromReaderWithUnknownFlagIsError) {
  constexpr uint64_t kImageOffset = 12345678;
  fvm::SparseImage header = {};
  header.magic = fvm::kSparseFormatMagic;
  header.version = fvm::kSparseFormatVersion;
  header.flags = fvm::kSparseFlagAllValid;
  header.header_length = sizeof(fvm::SparseImage);
  header.slice_size = 2 << 20;

  // All bytes are set.
  header.flags = std::numeric_limits<decltype(fvm::SparseImage::flags)>::max();
  ASSERT_NE((header.flags & ~fvm::kSparseFlagAllValid), 0u)
      << "At least one flag must be unused for an invalid flag to be a possibility.";

  BufferReader reader(kImageOffset, &header);

  ASSERT_TRUE(fvm_sparse_internal::GetHeader(kImageOffset, reader).is_error());
}

TEST(GetHeaderTest, FromReaderWithZeroSliceSizeIsError) {
  constexpr uint64_t kImageOffset = 12345678;
  fvm::SparseImage header = {};
  header.magic = fvm::kSparseFormatMagic;
  header.version = fvm::kSparseFormatVersion;
  header.flags = fvm::kSparseFlagAllValid;
  header.header_length = sizeof(fvm::SparseImage);
  header.slice_size = 0;

  // All bytes are set.
  header.flags = std::numeric_limits<decltype(fvm::SparseImage::flags)>::max();
  ASSERT_NE((header.flags & ~fvm::kSparseFlagAllValid), 0u)
      << "At least one flag must be unused for an invalid flag to be a possibility.";

  BufferReader reader(kImageOffset, &header);

  ASSERT_TRUE(fvm_sparse_internal::GetHeader(kImageOffset, reader).is_error());
}

TEST(GetHeaderTest, FromReaderWithHeaderLengthTooSmallIsError) {
  constexpr uint64_t kImageOffset = 12345678;
  fvm::SparseImage header = {};
  header.magic = fvm::kSparseFormatMagic;
  header.version = fvm::kSparseFormatVersion;
  header.flags = fvm::kSparseFlagAllValid;
  header.header_length = sizeof(fvm::SparseImage) - 1;
  header.slice_size = 2 << 20;

  // All bytes are set.
  header.flags = std::numeric_limits<decltype(fvm::SparseImage::flags)>::max();
  ASSERT_NE((header.flags & ~fvm::kSparseFlagAllValid), 0u)
      << "At least one flag must be unused for an invalid flag to be a possibility.";

  BufferReader reader(kImageOffset, &header);

  ASSERT_TRUE(fvm_sparse_internal::GetHeader(kImageOffset, reader).is_error());
}

TEST(GetHeaderTest, FromValidReaderIsOk) {
  constexpr uint64_t kImageOffset = 12345678;
  fvm::SparseImage header = {};
  header.magic = fvm::kSparseFormatMagic;
  header.version = fvm::kSparseFormatVersion;
  header.header_length = 2048;
  header.flags = fvm::kSparseFlagLz4;
  header.maximum_disk_size = 12345;
  header.partition_count = 12345676889;
  header.slice_size = 9999;

  BufferReader reader(kImageOffset, &header);

  auto header_or = fvm_sparse_internal::GetHeader(kImageOffset, reader);
  ASSERT_TRUE(header_or.is_ok()) << header_or.error();
  ASSERT_THAT(header_or.value(), HeaderEq(header));
}

struct PartitionDescriptors {
  struct {
    fvm::PartitionDescriptor descriptor;
    fvm::ExtentDescriptor extents[2];
  } partition_1 __PACKED;
  struct {
    fvm::PartitionDescriptor descriptor;
    fvm::ExtentDescriptor extents[3];
  } partition_2 __PACKED;
} __PACKED;

PartitionDescriptors GetPartitions() {
  PartitionDescriptors partitions = {};
  std::string name = "somerandomname";
  std::array<uint8_t, sizeof(fvm::PartitionDescriptor::type)> guid = {1, 2,  3,  4,  5,  6,  7, 8,
                                                                      9, 10, 11, 12, 13, 14, 15};

  partitions.partition_1.descriptor.magic = fvm::kPartitionDescriptorMagic;
  partitions.partition_1.descriptor.flags = fvm::kSparseFlagZxcrypt;
  memcpy(partitions.partition_1.descriptor.name, name.data(), name.length());
  memcpy(partitions.partition_1.descriptor.type, guid.data(), guid.size());
  partitions.partition_1.descriptor.extent_count = 2;

  partitions.partition_1.extents[0].magic = fvm::kExtentDescriptorMagic;
  partitions.partition_1.extents[0].extent_length = 0;
  partitions.partition_1.extents[0].slice_start = 0;
  partitions.partition_1.extents[0].slice_count = 1;

  partitions.partition_1.extents[1].magic = fvm::kExtentDescriptorMagic;
  partitions.partition_1.extents[1].extent_length = 0;
  partitions.partition_1.extents[1].slice_start = 2;
  partitions.partition_1.extents[1].slice_count = 1;

  name = "somerandomname2";
  guid = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  partitions.partition_2.descriptor.magic = fvm::kPartitionDescriptorMagic;
  partitions.partition_2.descriptor.flags = fvm::kSparseFlagZxcrypt;
  memcpy(partitions.partition_2.descriptor.name, name.data(), name.length());
  memcpy(partitions.partition_2.descriptor.type, guid.data(), guid.size());
  partitions.partition_2.descriptor.extent_count = 3;

  partitions.partition_2.extents[0].magic = fvm::kExtentDescriptorMagic;
  partitions.partition_2.extents[0].extent_length = 0;
  partitions.partition_2.extents[0].slice_start = 0;
  partitions.partition_2.extents[0].slice_count = 1;

  partitions.partition_2.extents[1].magic = fvm::kExtentDescriptorMagic;
  partitions.partition_2.extents[1].extent_length = 0;
  partitions.partition_2.extents[1].slice_start = 1;
  partitions.partition_2.extents[1].slice_count = 1;

  partitions.partition_2.extents[2].magic = fvm::kExtentDescriptorMagic;
  partitions.partition_2.extents[2].extent_length = 0;
  partitions.partition_2.extents[2].slice_start = 2;
  partitions.partition_2.extents[2].slice_count = 1;

  return partitions;
}

fvm::SparseImage GetHeader() {
  fvm::SparseImage header = {};
  header.magic = fvm::kSparseFormatMagic;
  header.version = fvm::kSparseFormatVersion;
  header.header_length = sizeof(fvm::SparseImage) + sizeof(PartitionDescriptors);
  header.flags = fvm::kSparseFlagLz4;
  header.partition_count = 2;
  header.slice_size = 8192;
  header.maximum_disk_size = 0;

  return header;
}

TEST(GetPartitions, WithBadPartitionMagicIsError) {
  constexpr uint64_t kImageOffset = 123456;
  auto header = GetHeader();
  auto partitions = GetPartitions();
  partitions.partition_2.descriptor.magic = 0;

  // The partition data starts at a random spot.
  BufferReader reader(kImageOffset, &partitions);

  ASSERT_TRUE(fvm_sparse_internal::GetPartitions(kImageOffset, reader, header).is_error());
}

TEST(GetPartitionsTest, WithUnknownFlagIsError) {
  constexpr uint64_t kImageOffset = 123456;
  auto header = GetHeader();
  auto partitions = GetPartitions();
  partitions.partition_2.descriptor.flags =
      std::numeric_limits<decltype(fvm::PartitionDescriptor::flags)>::max();

  // The partition data starts at a random spot.
  BufferReader reader(kImageOffset, &partitions);

  ASSERT_TRUE(fvm_sparse_internal::GetPartitions(kImageOffset, reader, header).is_error());
}

TEST(GetPartitionsTest, WithBadExtentMagicIsError) {
  constexpr uint64_t kImageOffset = 123456;
  auto header = GetHeader();
  auto partitions = GetPartitions();
  partitions.partition_2.extents[0].magic = 0;

  // The partition data starts at a random spot.
  BufferReader reader(kImageOffset, &partitions);

  ASSERT_TRUE(fvm_sparse_internal::GetPartitions(kImageOffset, reader, header).is_error());
}

TEST(GetPartitionsTest, WithExtentLengthSliceCountMismatchIsError) {
  constexpr uint64_t kImageOffset = 123456;
  auto header = GetHeader();
  auto partitions = GetPartitions();
  partitions.partition_2.extents[0].extent_length = 2 * header.slice_size;
  partitions.partition_2.extents[0].slice_count = 1;
  // The partition data starts at a random spot.
  BufferReader reader(kImageOffset, &partitions);

  ASSERT_TRUE(fvm_sparse_internal::GetPartitions(kImageOffset, reader, header).is_error());
}

TEST(GetPartitionsTest, WithOverlapingSlicesInPartitionExtentsIsError) {
  constexpr uint64_t kImageOffset = 123456;
  auto header = GetHeader();
  auto partitions = GetPartitions();

  partitions.partition_2.extents[0].slice_start = 1;
  partitions.partition_2.extents[0].slice_count = 4;

  partitions.partition_2.extents[1].slice_start = 8;
  partitions.partition_2.extents[1].slice_count = 2;

  auto& extent = partitions.partition_2.extents[2];

  // The partition data starts at a random spot.
  BufferReader reader(kImageOffset, &partitions);

  // Case 1:
  //    * extent overlaps before range.
  extent.slice_start = 0;
  extent.slice_count = 3;
  EXPECT_TRUE(fvm_sparse_internal::GetPartitions(kImageOffset, reader, header).is_error());

  // Case 2:
  //    * extent overlaps after range.
  extent.slice_start = 4;
  extent.slice_count = 2;
  EXPECT_TRUE(fvm_sparse_internal::GetPartitions(kImageOffset, reader, header).is_error());

  // Case 3:
  //    * extent overlaps in the middle of range
  extent.slice_start = 2;
  extent.slice_count = 1;
  EXPECT_TRUE(fvm_sparse_internal::GetPartitions(kImageOffset, reader, header).is_error());

  // Case 4:
  //    * extent overlaps multiple ranges
  extent.slice_start = 4;
  extent.slice_count = 8;
  EXPECT_TRUE(fvm_sparse_internal::GetPartitions(kImageOffset, reader, header).is_error());

  // Case 5:
  //    * extent covers same range
  extent.slice_start = 1;
  extent.slice_count = 4;
  EXPECT_TRUE(fvm_sparse_internal::GetPartitions(kImageOffset, reader, header).is_error());
}

TEST(GetPartitionsTest, WithValidReaderAndHeaderIsOk) {
  constexpr uint64_t kImageOffset = 123456;
  auto header = GetHeader();
  auto partitions = GetPartitions();

  // The partition data starts at a random spot.
  BufferReader reader(kImageOffset, &partitions);
  auto partitions_or = fvm_sparse_internal::GetPartitions(kImageOffset, reader, header);

  ASSERT_TRUE(partitions_or.is_ok()) << partitions_or.error();
  auto actual_partitions = partitions_or.take_value();

  ASSERT_EQ(actual_partitions.size(), 2u);
  EXPECT_THAT(partitions.partition_1.descriptor,
              PartitionDescriptorMatchesEntry(actual_partitions[0]));
  EXPECT_THAT(partitions.partition_1.extents, ExtentDescriptorsMatchesEntry(actual_partitions[0]));

  EXPECT_THAT(partitions.partition_2.descriptor,
              PartitionDescriptorMatchesEntry(actual_partitions[1]));
  EXPECT_THAT(partitions.partition_2.extents, ExtentDescriptorsMatchesEntry(actual_partitions[1]));
}

class FvmSparseReaderImpl final : public fvm::ReaderInterface {
 public:
  explicit FvmSparseReaderImpl(cpp20::span<const uint8_t> buffer) : buffer_(buffer) {}

  ~FvmSparseReaderImpl() final = default;

  zx_status_t Read(void* buf, size_t buf_size, size_t* size_actual) final {
    size_t bytes_to_read = std::min(buf_size, buffer_.size() - cursor_);
    memcpy(buf, buffer_.data() + cursor_, bytes_to_read);
    *size_actual = bytes_to_read;
    cursor_ += bytes_to_read;
    return ZX_OK;
  }

 private:
  cpp20::span<const uint8_t> buffer_;
  size_t cursor_ = 0;
};

TEST(FvmSparseWriteImageTest, WrittenImageIsCompatibleWithLegacyImplementation) {
  SerializedImageContainer container;
  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kNone));

  std::vector<fvm_sparse_internal::PartitionEntry> expected_partition_entries =
      GetExpectedPartitionEntries(descriptor, descriptor.options().slice_size);

  auto write_result = FvmSparseWriteImage(descriptor, &container.writer());
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();

  std::unique_ptr<FvmSparseReaderImpl> sparse_reader_impl(
      new FvmSparseReaderImpl(container.AsSpan()));
  std::unique_ptr<fvm::SparseReader> sparse_reader = nullptr;
  // This verifies metadata(header, partition descriptors and extent descriptors.)
  ASSERT_EQ(ZX_OK, fvm::SparseReader::Create(std::move(sparse_reader_impl), &sparse_reader));
  ASSERT_THAT(sparse_reader->Image(), Pointee(HeaderEq(container.serialized_image().header)));

  // Partition 1 metadata.
  {
    const auto& partition_descriptor = sparse_reader->Partitions()[0];
    const auto partition_extent_descriptors = cpp20::span<const fvm::ExtentDescriptor>(
        reinterpret_cast<const fvm::ExtentDescriptor*>(
            reinterpret_cast<const uint8_t*>(&partition_descriptor + 1)),
        3);

    EXPECT_THAT(partition_descriptor,
                PartitionDescriptorEq(container.serialized_image().partition_1.descriptor));
    EXPECT_THAT(partition_extent_descriptors,
                testing::Pointwise(ExtentDescriptorsAreEq(),
                                   container.serialized_image().partition_1.extents));
  }

  // Partition 2 metadata.
  {
    off_t partition_2_offset = sizeof(fvm::PartitionDescriptor) + 3 * sizeof(fvm::ExtentDescriptor);
    const auto& partition_descriptor = *reinterpret_cast<fvm::PartitionDescriptor*>(
        reinterpret_cast<uint8_t*>(sparse_reader->Partitions()) + partition_2_offset);
    const auto partition_extent_descriptors = cpp20::span<const fvm::ExtentDescriptor>(
        reinterpret_cast<const fvm::ExtentDescriptor*>(
            reinterpret_cast<const uint8_t*>(&partition_descriptor + 1)),
        2);
    EXPECT_THAT(partition_descriptor,
                PartitionDescriptorEq(container.serialized_image().partition_2.descriptor));
    EXPECT_THAT(partition_extent_descriptors,
                testing::Pointwise(ExtentDescriptorsAreEq(),
                                   container.serialized_image().partition_2.extents));
  }

  uint64_t partition_index = 0;
  for (const auto& partition : descriptor.partitions()) {
    std::vector<uint8_t> read_content;
    uint64_t extent_index = 0;
    auto extents = container.PartitionExtents(partition_index);
    for (const auto& mapping : partition.address().mappings) {
      read_content.resize(mapping.count, 0);
      size_t read_bytes = 0;
      ASSERT_EQ(sparse_reader->ReadData(read_content.data(), read_content.size(), &read_bytes),
                ZX_OK);
      EXPECT_THAT(read_content, testing::ElementsAreArray(extents[extent_index]));
      extent_index++;
    }
    partition_index++;
  }
}

TEST(FvmSparseWriteImageTest, WrittenCompressedImageIsCompatibleWithLegacyImplementation) {
  SerializedImageContainer container;
  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kLz4));

  std::vector<fvm_sparse_internal::PartitionEntry> expected_partition_entries =
      GetExpectedPartitionEntries(descriptor, descriptor.options().slice_size);

  Lz4Compressor compressor = Lz4Compressor::Create(descriptor.options().compression).take_value();
  auto write_result = FvmSparseWriteImage(descriptor, &container.writer(), &compressor);
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();

  std::unique_ptr<FvmSparseReaderImpl> sparse_reader_impl(
      new FvmSparseReaderImpl(container.AsSpan()));
  std::unique_ptr<fvm::SparseReader> sparse_reader = nullptr;
  // This verifies metadata(header, partition descriptors and extent descriptors.)
  ASSERT_EQ(ZX_OK, fvm::SparseReader::Create(std::move(sparse_reader_impl), &sparse_reader));
  ASSERT_THAT(sparse_reader->Image(), Pointee(HeaderEq(container.serialized_image().header)));

  // Partition 1 metadata.
  {
    const auto& partition_descriptor = sparse_reader->Partitions()[0];
    const auto partition_extent_descriptors = cpp20::span<const fvm::ExtentDescriptor>(
        reinterpret_cast<const fvm::ExtentDescriptor*>(
            reinterpret_cast<const uint8_t*>(&partition_descriptor + 1)),
        3);

    EXPECT_THAT(partition_descriptor,
                PartitionDescriptorEq(container.serialized_image().partition_1.descriptor));
    EXPECT_THAT(partition_extent_descriptors,
                testing::Pointwise(ExtentDescriptorsAreEq(),
                                   container.serialized_image().partition_1.extents));
  }

  // Partition 2 metadata.
  {
    off_t partition_2_offset = sizeof(fvm::PartitionDescriptor) + 3 * sizeof(fvm::ExtentDescriptor);
    const auto& partition_descriptor = *reinterpret_cast<fvm::PartitionDescriptor*>(
        reinterpret_cast<uint8_t*>(sparse_reader->Partitions()) + partition_2_offset);
    const auto partition_extent_descriptors = cpp20::span<const fvm::ExtentDescriptor>(
        reinterpret_cast<const fvm::ExtentDescriptor*>(
            reinterpret_cast<const uint8_t*>(&partition_descriptor + 1)),
        2);
    EXPECT_THAT(partition_descriptor,
                PartitionDescriptorEq(container.serialized_image().partition_2.descriptor));
    EXPECT_THAT(partition_extent_descriptors,
                testing::Pointwise(ExtentDescriptorsAreEq(),
                                   container.serialized_image().partition_2.extents));
  }

  // Check extent data.
  std::vector<uint8_t> read_content;
  std::vector<uint8_t> original_content;
  for (const auto& partition : descriptor.partitions()) {
    for (const auto& mapping : partition.address().mappings) {
      read_content.resize(mapping.count, 0);
      original_content.resize(mapping.count, 0);
      size_t read_bytes = 0;
      ASSERT_EQ(sparse_reader->ReadData(read_content.data(), read_content.size(), &read_bytes),
                ZX_OK);
      ASSERT_EQ(read_content.size(), read_bytes);
      auto read_result = partition.reader()->Read(
          mapping.source, cpp20::span(original_content.data(), original_content.size()));
      ASSERT_TRUE(read_result.is_ok()) << read_result.error();

      EXPECT_THAT(read_content, testing::ElementsAreArray(original_content));
    }
  }
}

auto FvmHeaderEq(const fvm::Header& expected_header) {
  using Header = fvm::Header;
  return testing::AllOf(
      testing::Field(&Header::magic, testing::Eq(expected_header.magic)),
      testing::Field(&Header::allocation_table_size,
                     testing::Eq(expected_header.allocation_table_size)),
      testing::Field(&Header::fvm_partition_size, testing::Eq(expected_header.fvm_partition_size)),
      testing::Field(&Header::oldest_minor_version,
                     testing::Eq(expected_header.oldest_minor_version)),
      testing::Field(&Header::slice_size, testing::Eq(expected_header.slice_size)),
      testing::Field(&Header::vpartition_table_size,
                     testing::Eq(expected_header.vpartition_table_size)),
      testing::Field(&Header::hash, testing::ElementsAreArray(expected_header.hash)),
      testing::Field(&Header::pslice_count, testing::Eq(expected_header.pslice_count)),
      testing::Field(&Header::generation, testing::Eq(expected_header.generation)));
}

TEST(ConvertToFvmHeaderTest, WithNulloptIsOk) {
  constexpr uint64_t kMinSliceCount = 20;
  fvm::SparseImage sparse_header = GetHeader();
  auto expected_header = fvm::Header::FromSliceCount(fvm::kMaxUsablePartitions, kMinSliceCount,
                                                     sparse_header.slice_size);

  auto header_or =
      fvm_sparse_internal::ConvertToFvmHeader(sparse_header, kMinSliceCount, std::nullopt);
  ASSERT_TRUE(header_or.is_ok()) << header_or.error();
  auto header = header_or.take_value();

  EXPECT_THAT(header, FvmHeaderEq(expected_header)) << "Expected header: \n"
                                                    << expected_header.ToString() << "\n"
                                                    << "Header :\n"
                                                    << header.ToString();
}

TEST(ConvertToFvmHeaderTest, OverloadIsOk) {
  constexpr uint64_t kMinSliceCount = 20;
  fvm::SparseImage sparse_header = GetHeader();
  auto expected_header = fvm::Header::FromSliceCount(fvm::kMaxUsablePartitions, kMinSliceCount,
                                                     sparse_header.slice_size);

  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, kMinSliceCount);
  ASSERT_TRUE(header_or.is_ok()) << header_or.error();
  auto header = header_or.take_value();

  EXPECT_THAT(header, FvmHeaderEq(expected_header)) << "Expected header: \n"
                                                    << expected_header.ToString() << "\n"
                                                    << "Header :\n"
                                                    << header.ToString();
}

TEST(ConvertToFvmHeaderTest, WithTargetDiskIsOk) {
  constexpr uint64_t kMinSliceCount = 20;
  constexpr uint64_t kTargetVolumeSize = 20ull << 32;

  FvmOptions options;
  options.target_volume_size = kTargetVolumeSize;

  fvm::SparseImage sparse_header = GetHeader();
  auto expected_header = fvm::Header::FromDiskSize(fvm::kMaxUsablePartitions, kTargetVolumeSize,
                                                   sparse_header.slice_size);
  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, kMinSliceCount, options);
  ASSERT_TRUE(header_or.is_ok()) << header_or.error();
  auto header = header_or.take_value();

  EXPECT_THAT(header, FvmHeaderEq(expected_header)) << "Expected header: \n"
                                                    << expected_header.ToString() << "\n"
                                                    << "Header :\n"
                                                    << header.ToString();
}

TEST(ConvertToFvmHeaderTest, WithTooSmallTargetDiskIsError) {
  constexpr uint64_t kMinSliceCount = 16;
  constexpr uint64_t kTargetVolumeSize = 16ull << 20;

  FvmOptions options;
  options.target_volume_size = kTargetVolumeSize;

  fvm::SparseImage sparse_header = GetHeader();
  sparse_header.slice_size = 1ull << 20;
  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, kMinSliceCount, options);
  ASSERT_TRUE(header_or.is_error());
}

TEST(ConvertToFvmHeaderTest, WithTargetAndMaxVolumeSizeIsOk) {
  constexpr uint64_t kMinSliceCount = 20;
  constexpr uint64_t kTargetVolumeSize = 20ull << 32;
  constexpr uint64_t kMaxVolumeSize = 40ull << 32;

  FvmOptions options;
  options.target_volume_size = kTargetVolumeSize;
  options.max_volume_size = kMaxVolumeSize;

  fvm::SparseImage sparse_header = GetHeader();
  auto expected_header = fvm::Header::FromGrowableDiskSize(
      fvm::kMaxUsablePartitions, kTargetVolumeSize, kMaxVolumeSize, sparse_header.slice_size);
  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, kMinSliceCount, options);
  ASSERT_TRUE(header_or.is_ok()) << header_or.error();
  auto header = header_or.take_value();

  EXPECT_THAT(header, FvmHeaderEq(expected_header)) << "Expected header: \n"
                                                    << expected_header.ToString() << "\n"
                                                    << "Header :\n"
                                                    << header.ToString();
}

TEST(ConvertToFvmHeaderTest, WithTargetAndMaxVolumeOnSparseHeaderSizeIsOk) {
  constexpr uint64_t kMinSliceCount = 20;
  constexpr uint64_t kTargetVolumeSize = 20ull << 32;
  constexpr uint64_t kMaxVolumeSize = 40ull << 32;

  FvmOptions options;
  options.target_volume_size = kTargetVolumeSize;

  fvm::SparseImage sparse_header = GetHeader();
  sparse_header.maximum_disk_size = kMaxVolumeSize;
  auto expected_header = fvm::Header::FromGrowableDiskSize(
      fvm::kMaxUsablePartitions, kTargetVolumeSize, kMaxVolumeSize, sparse_header.slice_size);
  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, kMinSliceCount, options);
  ASSERT_TRUE(header_or.is_ok()) << header_or.error();
  auto header = header_or.take_value();

  EXPECT_THAT(header, FvmHeaderEq(expected_header)) << "Expected header: \n"
                                                    << expected_header.ToString() << "\n"
                                                    << "Header :\n"
                                                    << header.ToString();
}

TEST(ConvertToFvmHeaderTest, WithHMaxVolumeSizeInOptionsOverridesOneInsSparseHeader) {
  constexpr uint64_t kMinSliceCount = 20;
  constexpr uint64_t kTargetVolumeSize = 20ull << 32;
  constexpr uint64_t kMaxVolumeSize = 40ull << 32;

  FvmOptions options;
  options.target_volume_size = kTargetVolumeSize;
  options.max_volume_size = kMaxVolumeSize;

  fvm::SparseImage sparse_header = GetHeader();
  sparse_header.maximum_disk_size = kTargetVolumeSize;
  auto expected_header = fvm::Header::FromGrowableDiskSize(
      fvm::kMaxUsablePartitions, kTargetVolumeSize, kMaxVolumeSize, sparse_header.slice_size);
  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, kMinSliceCount, options);
  ASSERT_TRUE(header_or.is_ok()) << header_or.error();
  auto header = header_or.take_value();

  EXPECT_THAT(header, FvmHeaderEq(expected_header)) << "Expected header: \n"
                                                    << expected_header.ToString() << "\n"
                                                    << "Header :\n"
                                                    << header.ToString();
}

TEST(ConvertToFvmHeaderTest, WithHMaxVolumeSizeAndNoTargetVolumeSizeDefaultsToMinSliceCountSize) {
  constexpr uint64_t kMinSliceCount = 20;
  constexpr uint64_t kMaxVolumeSize = 40ull << 32;

  FvmOptions options;
  options.max_volume_size = kMaxVolumeSize;

  fvm::SparseImage sparse_header = GetHeader();

  // This accounts for 20 slices without reserved metadata, and is an initial fvm_partition_size.
  uint64_t expected_volume_size =
      fvm::Header::FromSliceCount(fvm::kMaxUsablePartitions, kMinSliceCount,
                                  sparse_header.slice_size)
          .fvm_partition_size;
  auto expected_header = fvm::Header::FromGrowableDiskSize(
      fvm::kMaxUsablePartitions, expected_volume_size, kMaxVolumeSize, sparse_header.slice_size);
  expected_header.SetSliceCount(kMinSliceCount);

  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, kMinSliceCount, options);
  ASSERT_TRUE(header_or.is_ok()) << header_or.error();
  auto header = header_or.take_value();

  EXPECT_THAT(header, FvmHeaderEq(expected_header)) << "Expected header: \n"
                                                    << expected_header.ToString() << "\n"
                                                    << "Header :\n"
                                                    << header.ToString();
}

TEST(ConvertToFvmHeaderTest, WithMaxVolumeSizeTooSmallIsError) {
  constexpr uint64_t kMinSliceCount = 20;
  constexpr uint64_t kMaxVolumeSize = 20 << 20;

  FvmOptions options;
  options.max_volume_size = kMaxVolumeSize;

  fvm::SparseImage sparse_header = GetHeader();
  // Emough space for 20 slices, but no metadata.
  sparse_header.slice_size = 1 << 20;
  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, kMinSliceCount, options);
  ASSERT_TRUE(header_or.is_error());
}

TEST(ConvertToFvmMetadataTest, WithNoPartitionsIsOk) {
  fvm::SparseImage sparse_header = GetHeader();
  sparse_header.partition_count = 0;
  sparse_header.header_length = sizeof(fvm::SparseImage);

  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, 100);
  EXPECT_TRUE(header_or.is_ok()) << header_or.error();
  auto header = header_or.take_value();

  auto metadata_or = fvm_sparse_internal::ConvertToFvmMetadata(
      header, cpp20::span<fvm_sparse_internal::PartitionEntry>());
  ASSERT_TRUE(metadata_or.is_ok()) << metadata_or.error();
  auto metadata = metadata_or.take_value();

  ASSERT_TRUE(metadata.CheckValidity());

  // The expected header has a zeroed hash, so we set this to zero for verification.
  auto actual_header = metadata.GetHeader();
  memset(actual_header.hash, 0, sizeof(fvm::Header::hash));
  EXPECT_THAT(actual_header, FvmHeaderEq(header));

  for (auto i = 1u; i < header.GetPartitionTableEntryCount(); ++i) {
    auto entry = metadata.GetPartitionEntry(i);
    EXPECT_TRUE(entry.IsFree());
  }
}

TEST(ConvertToFvmMetadataTest, WithSinglePartitionsAndNoSlicesIsOk) {
  fvm::SparseImage sparse_header = GetHeader();
  sparse_header.partition_count = 1;
  sparse_header.header_length = sizeof(fvm::SparseImage) + sizeof(fvm::PartitionDescriptor);

  fvm_sparse_internal::PartitionEntry entry = {};
  entry.descriptor.flags = 0;
  entry.descriptor.magic = fvm::kPartitionDescriptorMagic;
  entry.descriptor.type[0] = 1;
  entry.descriptor.extent_count = 0;

  std::string_view kPartitionName = "mypartition";
  memcpy(entry.descriptor.name, kPartitionName.data(), kPartitionName.size());

  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, 100);
  EXPECT_TRUE(header_or.is_ok()) << header_or.error();
  auto header = header_or.take_value();

  auto metadata_or = fvm_sparse_internal::ConvertToFvmMetadata(
      header, cpp20::span<fvm_sparse_internal::PartitionEntry>(&entry, 1));
  ASSERT_TRUE(metadata_or.is_ok()) << metadata_or.error();
  auto metadata = metadata_or.take_value();

  ASSERT_TRUE(metadata.CheckValidity());

  // The expected header has a zeroed hash, so we set this to zero for verification.
  auto actual_header = metadata.GetHeader();
  memset(actual_header.hash, 0, sizeof(fvm::Header::hash));
  EXPECT_THAT(actual_header, FvmHeaderEq(header));

  int used_entries = 0;
  for (auto i = 1u; i < header.GetPartitionTableEntryCount(); ++i) {
    auto entry = metadata.GetPartitionEntry(i);
    if (i != 1) {
      EXPECT_TRUE(entry.IsFree());
      continue;
    }

    EXPECT_EQ(entry.type[0], 1);
    EXPECT_TRUE(kPartitionName.compare(entry.name()) == 0);
    EXPECT_EQ(entry.flags, 0u);
    for (auto& b : cpp20::span<uint8_t>(entry.type).subspan(1)) {
      EXPECT_EQ(b, 0);
    }
    EXPECT_EQ(entry.slices, 0u);
    used_entries++;
  }
  EXPECT_EQ(used_entries, 1);
}

TEST(ConvertToFvmMetadataTest, WithSinglePartitionsAndSlicesIsOk) {
  fvm::SparseImage sparse_header = GetHeader();
  sparse_header.partition_count = 1;
  sparse_header.header_length = sizeof(fvm::SparseImage) + sizeof(fvm::PartitionDescriptor);

  fvm_sparse_internal::PartitionEntry entry = {};
  entry.descriptor.flags = 0;
  entry.descriptor.magic = fvm::kPartitionDescriptorMagic;
  entry.descriptor.type[0] = 1;
  entry.descriptor.extent_count = 2;

  constexpr unsigned int kTotalSlices = 30;
  entry.extents.push_back(fvm::ExtentDescriptor{.magic = fvm::kExtentDescriptorMagic,
                                                .slice_start = 0,
                                                .slice_count = 5,
                                                .extent_length = 10});
  entry.extents.push_back(fvm::ExtentDescriptor{.magic = fvm::kExtentDescriptorMagic,
                                                .slice_start = 10,
                                                .slice_count = 25,
                                                .extent_length = 10});

  std::string_view kPartitionName = "mypartition";
  memcpy(entry.descriptor.name, kPartitionName.data(), kPartitionName.size());

  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, 100);
  EXPECT_TRUE(header_or.is_ok()) << header_or.error();
  auto header = header_or.take_value();

  auto metadata_or = fvm_sparse_internal::ConvertToFvmMetadata(
      header, cpp20::span<fvm_sparse_internal::PartitionEntry>(&entry, 1));
  ASSERT_TRUE(metadata_or.is_ok()) << metadata_or.error();
  auto metadata = metadata_or.take_value();

  ASSERT_TRUE(metadata.CheckValidity());

  // The expected header has a zeroed hash, so we set this to zero for verification.
  auto actual_header = metadata.GetHeader();
  memset(actual_header.hash, 0, sizeof(fvm::Header::hash));
  EXPECT_THAT(actual_header, FvmHeaderEq(header));

  int used_entries = 0;
  for (auto i = 1u; i < header.GetPartitionTableEntryCount(); ++i) {
    auto entry = metadata.GetPartitionEntry(i);
    if (i != 1) {
      EXPECT_TRUE(entry.IsFree());
      continue;
    }

    EXPECT_TRUE(kPartitionName.compare(entry.name()) == 0);
    EXPECT_EQ(entry.flags, 0u);
    EXPECT_EQ(entry.type[0], 1);
    for (auto& b : cpp20::span<uint8_t>(entry.type).subspan(1)) {
      EXPECT_EQ(b, 0);
    }
    EXPECT_EQ(entry.slices, kTotalSlices);
    used_entries++;
  }
  EXPECT_EQ(used_entries, 1);
}

TEST(ConvertToFvmMetadataTest, WithsMultiplePartitionsAndSlicesIsOk) {
  constexpr size_t kUsedPartitions = 4;
  fvm::SparseImage sparse_header = GetHeader();
  sparse_header.partition_count = kUsedPartitions;

  std::vector<fvm_sparse_internal::PartitionEntry> entries;

  auto get_expected_partition_name = [](auto index) {
    return std::string("partition") + std::to_string(index);
  };

  for (auto i = 0u; i < kUsedPartitions; ++i) {
    fvm_sparse_internal::PartitionEntry entry = {};
    entry.descriptor.magic = fvm::kPartitionDescriptorMagic;
    entry.descriptor.flags = 0;
    // Shifted so partition 1 has the ith value for first bit.
    entry.descriptor.type[0] = static_cast<uint8_t>(i + 1);

    memcpy(entry.descriptor.name, get_expected_partition_name(i).data(),
           get_expected_partition_name(i).size());

    entry.descriptor.extent_count = i + 1;

    size_t last_end = 0;
    for (auto j = 0u; j < entry.descriptor.extent_count; ++j) {
      entry.extents.push_back({
          .magic = fvm::kExtentDescriptorMagic,
          .slice_start = last_end,
          .slice_count = j + 1,
          .extent_length = 10,
      });
      last_end += j + 1;
    }
    entries.push_back(entry);
  }

  auto header_or = fvm_sparse_internal::ConvertToFvmHeader(sparse_header, 100);
  EXPECT_TRUE(header_or.is_ok()) << header_or.error();
  auto header = header_or.take_value();

  auto metadata_or = fvm_sparse_internal::ConvertToFvmMetadata(header, entries);
  ASSERT_TRUE(metadata_or.is_ok()) << metadata_or.error();
  auto metadata = metadata_or.take_value();

  ASSERT_TRUE(metadata.CheckValidity());

  // The expected header has a zeroed hash, so we set this to zero for verification.
  auto actual_header = metadata.GetHeader();
  memset(actual_header.hash, 0, sizeof(fvm::Header::hash));
  EXPECT_THAT(actual_header, FvmHeaderEq(header));
  size_t used_partitions = 0;
  for (auto i = 1u; i < header.GetPartitionTableEntryCount(); ++i) {
    auto entry = metadata.GetPartitionEntry(i);
    if (i > kUsedPartitions) {
      EXPECT_TRUE(entry.IsFree());
      continue;
    }

    auto expected_entry = entries[i - 1];
    auto actual_entry = metadata.GetPartitionEntry(i);

    EXPECT_TRUE(memcmp(actual_entry.type, expected_entry.descriptor.type,
                       sizeof(fvm::VPartitionEntry::type)) == 0);

    EXPECT_TRUE(memcmp(actual_entry.unsafe_name, expected_entry.descriptor.name,
                       sizeof(fvm::VPartitionEntry::unsafe_name)) == 0);
    // i-th partition has i-1 extents, and extent j has j+1 slices.
    // So expanding this we have 1(j==0) + 2(j==1) + 3 + ....+ (max(j) + 1)(max(j) = i)
    // Which yields, Sum 0 to i + 1 or j.
    size_t expected_slices = (i * (i + 1) / 2);
    EXPECT_EQ(actual_entry.slices, expected_slices);
    EXPECT_EQ(actual_entry.flags, 0u);
    EXPECT_EQ(actual_entry.type[0], static_cast<uint8_t>(i));
    for (auto& b : cpp20::span<uint8_t>(actual_entry.type).subspan(1)) {
      EXPECT_EQ(b, 0);
    }
    EXPECT_TRUE(entry.IsActive());
    used_partitions++;
  }

  EXPECT_EQ(used_partitions, kUsedPartitions);
}

TEST(FvmSparseDecompressImageTest, BadSparseImageHeaderIsError) {
  SerializedImageContainer container;
  std::vector<uint8_t> buffer;
  BufferWriter writer(buffer);
  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kNone));

  auto write_result = FvmSparseWriteImage(descriptor, &container.writer());
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();

  // Make the header invalid.
  container.serialized_image().header.magic = 0;

  auto decompress_or =
      FvmSparseDecompressImage(0, BufferReader(0, &container.serialized_image()), writer);

  EXPECT_TRUE(decompress_or.is_error());
}

TEST(FvmSparseDecompressImageTest, BadPartitionDescriptorIsError) {
  SerializedImageContainer container;
  std::vector<uint8_t> buffer;
  BufferWriter writer(buffer);
  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kNone));

  auto write_result = FvmSparseWriteImage(descriptor, &container.writer());
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();

  // Make the descriptor invalid.
  container.serialized_image().partition_1.descriptor.magic = 0;

  auto decompress_or =
      FvmSparseDecompressImage(0, BufferReader(0, &container.serialized_image()), writer);

  EXPECT_TRUE(decompress_or.is_error());
}

TEST(FvmSparseDecompressImageTest, BadExtentDescriptorIsError) {
  SerializedImageContainer container;
  std::vector<uint8_t> buffer;
  BufferWriter writer(buffer);
  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kNone));

  auto write_result = FvmSparseWriteImage(descriptor, &container.writer());
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();

  // Make the descriptor invalid.
  container.serialized_image().partition_1.extents[0].magic = 0;

  auto decompress_or =
      FvmSparseDecompressImage(0, BufferReader(0, &container.serialized_image()), writer);

  EXPECT_TRUE(decompress_or.is_error());
}

TEST(FvmSparseDecompressImageTest, CompressedImageWithBadCompresseDataIsError) {
  SerializedImageContainer container;
  std::vector<uint8_t> buffer;
  BufferWriter writer(buffer);
  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kNone));

  auto write_result = FvmSparseWriteImage(descriptor, &container.writer());
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();

  // Claim the image is compressed, this will trigger a malformed framed error in Lz4.
  container.serialized_image().header.flags |= fvm::kSparseFlagLz4;

  auto decompress_or =
      FvmSparseDecompressImage(0, BufferReader(0, &container.serialized_image()), writer);

  EXPECT_TRUE(decompress_or.is_error());
}

TEST(FvmSparseDecompressImageTest, UncompressedImageReturnsFalse) {
  SerializedImageContainer container;
  std::vector<uint8_t> buffer;
  BufferWriter writer(buffer);
  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kNone));

  auto write_result = FvmSparseWriteImage(descriptor, &container.writer());
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();

  auto decompress_or =
      FvmSparseDecompressImage(0, BufferReader(0, &container.serialized_image()), writer);

  EXPECT_TRUE(decompress_or.is_ok()) << decompress_or.error();
  EXPECT_FALSE(decompress_or.value());
}

TEST(FvmSparseDecompressImageTest, CompressedImageReturnsTrueAndIsCorrect) {
  SerializedImageContainer compressed_container;
  SerializedImageContainer decompressed_container;
  SerializedImageContainer expected_container;

  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kLz4));
  auto decompressed_descriptor =
      MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kNone));

  Lz4Compressor compressor = Lz4Compressor::Create(descriptor.options().compression).take_value();

  // Write the compressed data that we will decompress later.
  auto write_result = FvmSparseWriteImage(descriptor, &compressed_container.writer(), &compressor);
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();

  // Write the decompressed version that we will compare against.
  write_result = FvmSparseWriteImage(decompressed_descriptor, &expected_container.writer());
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();

  // When decompressing this flag should remain, since the zeroes where already emitted as part of
  // the compressed image, and the were decompressed. In general, not keeping this flag, would
  // apply fill to all extents, even those who do not need it.
  expected_container.serialized_image().header.flags |= fvm::kSparseFlagZeroFillNotRequired;

  auto decompress_or = FvmSparseDecompressImage(
      0, BufferReader(0, &compressed_container.serialized_image(), sizeof(SerializedSparseImage)),
      decompressed_container.writer());

  EXPECT_TRUE(decompress_or.is_ok()) << decompress_or.error();
  EXPECT_TRUE(decompress_or.value());

  // Now compare the contents of the written decompressed image to the one of the generated
  // decompressed.
  EXPECT_THAT(decompressed_container.serialized_image().header,
              HeaderEq(expected_container.serialized_image().header));
  EXPECT_THAT(decompressed_container.serialized_image().partition_1.descriptor,
              PartitionDescriptorEq(expected_container.serialized_image().partition_1.descriptor));
  EXPECT_THAT(decompressed_container.serialized_image().partition_1.extents,
              testing::Pointwise(ExtentDescriptorsAreEq(),
                                 expected_container.serialized_image().partition_1.extents));
  EXPECT_THAT(decompressed_container.serialized_image().partition_2.descriptor,
              PartitionDescriptorEq(expected_container.serialized_image().partition_2.descriptor));
  EXPECT_THAT(decompressed_container.serialized_image().partition_2.extents,
              testing::Pointwise(ExtentDescriptorsAreEq(),
                                 expected_container.serialized_image().partition_2.extents));

  EXPECT_THAT(decompressed_container.serialized_image().extent_data,
              testing::ElementsAreArray(expected_container.serialized_image().extent_data));
}

TEST(FvmSparseReadImageTest, NullReaderIsError) {
  ASSERT_TRUE(FvmSparseReadImage(0, nullptr).is_error());
}

void CheckGeneratedDescriptor(const FvmDescriptor& actual_descriptor,
                              const FvmDescriptor& original_descriptor) {
  EXPECT_EQ(actual_descriptor.options().slice_size, original_descriptor.options().slice_size);
  EXPECT_EQ(actual_descriptor.options().max_volume_size,
            original_descriptor.options().max_volume_size);
  EXPECT_EQ(actual_descriptor.options().target_volume_size, std::nullopt);
  EXPECT_EQ(actual_descriptor.options().compression.schema, CompressionSchema::kNone);

  ASSERT_EQ(actual_descriptor.partitions().size(), original_descriptor.partitions().size());
  auto read_partition_it = actual_descriptor.partitions().begin();
  auto expected_partition_it = original_descriptor.partitions().begin();
  for (size_t i = 0; i < actual_descriptor.partitions().size(); ++i) {
    const auto& actual_partition = *(read_partition_it);
    const auto& expected_partition = *(expected_partition_it);

    EXPECT_EQ(actual_partition.volume().name, expected_partition.volume().name);
    EXPECT_EQ(actual_partition.volume().encryption, expected_partition.volume().encryption);
    EXPECT_THAT(actual_partition.volume().type,
                testing::ElementsAreArray(expected_partition.volume().type));
    EXPECT_THAT(actual_partition.volume().instance,
                testing::ElementsAreArray(fvm::kPlaceHolderInstanceGuid));

    // Verify that the target mappings are the same, and the contents of each mapping are the same.
    ASSERT_EQ(actual_partition.address().mappings.size(),
              expected_partition.address().mappings.size());

    // Check data content.
    std::vector<uint8_t> actual_data;
    std::vector<uint8_t> expected_data;

    for (size_t j = 0; j < actual_partition.address().mappings.size(); ++j) {
      const auto& actual_mapping = actual_partition.address().mappings[j];
      const auto& expected_mapping = expected_partition.address().mappings[j];

      EXPECT_EQ(actual_mapping.target, expected_mapping.target);
      EXPECT_EQ(actual_mapping.count, expected_mapping.count);

      // Calculate the number of bytes that the extent should have, based on the minimum number of
      // slices that it is requested.
      std::optional<uint64_t> expected_size =
          8192 * fvm::BlocksToSlices(8192, expected_partition.volume().block_size,
                                     storage::volume_image::GetBlockCount(
                                         expected_mapping.target, expected_mapping.count,
                                         expected_partition.volume().block_size));

      EXPECT_EQ(actual_mapping.size, expected_size);

      // Non compressed images will require zero filling.
      if (original_descriptor.options().compression.schema == CompressionSchema::kNone) {
        ASSERT_EQ(actual_mapping.options.size(), 1u);
        EXPECT_NE(actual_mapping.options.find(EnumAsString(AddressMapOption::kFill)),
                  actual_mapping.options.end());
      } else {
        ASSERT_EQ(actual_mapping.options.size(), 0u);
      }

      actual_data.resize(actual_mapping.count, 0);
      expected_data.resize(expected_mapping.count, 0);

      auto actual_read_result = actual_partition.reader()->Read(actual_mapping.source, actual_data);
      ASSERT_TRUE(actual_read_result.is_ok()) << actual_read_result.error();

      auto expected_read_result =
          expected_partition.reader()->Read(expected_mapping.source, expected_data);
      ASSERT_TRUE(expected_read_result.is_ok()) << expected_read_result.error();

      EXPECT_THAT(actual_data, testing::ElementsAreArray(expected_data));
    }

    read_partition_it++;
    expected_partition_it++;
  }
}

TEST(FvmSparseReadImageTest, CompressedImageIsOk) {
  SerializedImageContainer compressed_container;
  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kLz4));

  Lz4Compressor compressor = Lz4Compressor::Create(descriptor.options().compression).take_value();

  // Write the compressed data that we will decompress later.
  auto write_result = FvmSparseWriteImage(descriptor, &compressed_container.writer(), &compressor);
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();

  auto read_descriptor_or = FvmSparseReadImage(
      0, std::make_unique<BufferReader>(0, &compressed_container.serialized_image(),
                                        sizeof(SerializedSparseImage)));
  EXPECT_TRUE(read_descriptor_or.is_ok()) << read_descriptor_or.error();
  auto read_descriptor = read_descriptor_or.take_value();

  ASSERT_NO_FATAL_FAILURE(CheckGeneratedDescriptor(read_descriptor, descriptor));
}

TEST(FvmSparseReadImageTest, ReturnsFvmDescriptorAndIsCorrect) {
  SerializedImageContainer compressed_container;
  auto descriptor = MakeFvmDescriptorWithOptions(MakeOptions(8192, CompressionSchema::kNone));

  // Write the compressed data that we will decompress later.
  auto write_result = FvmSparseWriteImage(descriptor, &compressed_container.writer());
  ASSERT_TRUE(write_result.is_ok()) << write_result.error();

  auto read_descriptor_or = FvmSparseReadImage(
      0, std::make_unique<BufferReader>(0, &compressed_container.serialized_image(),
                                        sizeof(SerializedSparseImage)));
  EXPECT_TRUE(read_descriptor_or.is_ok());
  auto read_descriptor = read_descriptor_or.take_value();

  ASSERT_NO_FATAL_FAILURE(CheckGeneratedDescriptor(read_descriptor, descriptor));
}

}  // namespace
}  // namespace storage::volume_image
