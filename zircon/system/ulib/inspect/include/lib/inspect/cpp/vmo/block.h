// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_INSPECT_CPP_VMO_BLOCK_H_
#define LIB_INSPECT_CPP_VMO_BLOCK_H_

#include <lib/inspect/cpp/vmo/limits.h>
#include <lib/stdcompat/optional.h>
#include <zircon/types.h>

#include <algorithm>
#include <cstdint>
#include <type_traits>

namespace inspect {
namespace internal {

enum class BlockType : uint8_t {
  kFree = 0,
  kReserved = 1,
  kHeader = 2,
  kNodeValue = 3,
  kIntValue = 4,
  kUintValue = 5,
  kDoubleValue = 6,
  kBufferValue = 7,
  kExtent = 8,
  kName = 9,
  kTombstone = 10,
  kArrayValue = 11,
  kLinkValue = 12,
  kBoolValue = 13,
  kStringReference = 14,
};

enum class PropertyBlockFormat : uint8_t {
  // The property is a UTF-8 string.
  kUtf8 = 0,

  // The property is a binary string of uint8_t.
  kBinary = 1
};

enum class ArrayBlockFormat : uint8_t {
  // The array stores N raw values in N slots.
  kDefault = 0,

  // The array is a linear histogram with N buckets and N+4 slots, which are:
  // - param_floor_value
  // - param_step_size
  // - underflow_bucket
  // - ...N buckets...
  // - overflow_bucket
  kLinearHistogram = 1,

  // The array is an exponential histogram with N buckets and N+5 slots, which are:
  // - param_floor_value
  // - param_initial_step
  // - param_step_multiplier
  // - underflow_bucket
  // - ...N buckets...
  // - overflow_bucket
  kExponentialHistogram = 2
};

enum class LinkBlockDisposition : uint8_t {
  // The linked sub-hierarchy root is a child of the LINK_VALUE's parent.
  kChild = 0,

  // The linked sub-hierarchy root's properties and children belong to the LINK_VALUE's parent.
  kInline = 1,
};

using BlockOrder = uint32_t;
using BlockIndex = uint64_t;

// Returns the smallest order such that (kMinOrderSize << order) >= size.
// Size must be non-zero.
constexpr BlockOrder FitOrder(size_t size) {
  auto ret = static_cast<size_t>(64 - __builtin_clzl(size - 1)) - kMinOrderShift;
  return static_cast<BlockOrder>(ret);
}

// Structure of the block header and payload.
struct Block final {
  union {
    uint64_t header;
    char header_data[8];
  };
  union {
    int64_t i64;
    uint64_t u64;
    double f64;
    char data[8];
  } payload;

  // Get the payload as a const char*.
  const char* payload_ptr() const { return static_cast<const char*>(payload.data); }

  // Get the payload as a char*.
  char* payload_ptr() { return static_cast<char*>(payload.data); }
};

static_assert(sizeof(Block) == 16, "Block header must be 16 bytes");
static_assert(sizeof(Block) == kMinOrderSize,
              "Minimum allocation size must exactly hold a block header");

// Describes the layout of a bit-field packed into a 64-bit word.
template <size_t begin, size_t end>
struct Field final {
  static_assert(begin < sizeof(uint64_t) * 8, "begin is out of bounds");
  static_assert(end < sizeof(uint64_t) * 8, "end is out of bounds");
  static_assert(begin <= end, "begin must not be larger than end");
  static_assert(end - begin + 1 < 64, "must be a part of a word, not a whole word");

  static constexpr uint64_t kMask = (uint64_t(1) << (end - begin + 1)) - 1;

  template <typename T>
  static constexpr uint64_t Make(T value) {
    return static_cast<uint64_t>(value) << begin;
  }

  template <typename U>
  static constexpr U Get(uint64_t word) {
    static_assert(sizeof(U) >= SizeInBytes(), "attempt to narrow value lossily");
    return static_cast<U>((word >> (begin % 64)) & kMask);
  }

  static constexpr void Set(uint64_t* word, uint64_t value) {
    *word = (*word & ~(kMask << begin)) | (value << begin);
  }

  // The size of a field in bytes. This will truncate the size of values which
  // are not byte aligned.
  static constexpr size_t SizeInBytes() { return ((end + 1) - begin) / 8; }
};

// Describes the base fields present for all blocks.
struct BlockFields {
  using Order = Field<0, 3>;
  using Type = Field<8, 15>;
};

struct HeaderBlockFields final : public BlockFields {
  using Version = Field<16, 31>;
  using MagicNumber = Field<32, 63>;
};

struct FreeBlockFields final : public BlockFields {
  using NextFreeBlock = Field<16, 39>;
};

// Describes the fields common to all value blocks.
struct ValueBlockFields final : public BlockFields {
  using ParentIndex = Field<16, 39>;
  using NameIndex = Field<40, 63>;
};

struct StringReferenceBlockFields final : public BlockFields {
  using NextExtentIndex = Field<16, 39>;
  using ReferenceCount = Field<40, 63>;
};

struct StringReferenceBlockPayload final {
  using TotalLength = Field<0, 31>;
};

struct PropertyBlockPayload final {
  using TotalLength = Field<0, 31>;
  using ExtentIndex = Field<32, 59>;
  using Flags = Field<60, 63>;
};

// Describes the fields for ARRAY_VALUE payloads.
struct ArrayBlockPayload final {
  using EntryType = Field<0, 3>;
  using Flags = Field<4, 7>;
  using Count = Field<8, 15>;
};

struct ExtentBlockFields final : public BlockFields {
  using NextExtentIndex = Field<16, 39>;
};

struct NameBlockFields final : public BlockFields {
  using Length = Field<16, 27>;
};

struct LinkBlockPayload final {
  using ContentIndex = Field<0, 19>;
  using Flags = Field<60, 63>;
};

constexpr BlockOrder GetOrder(const Block* block) {
  return BlockFields::Order::Get<BlockOrder>(block->header);
}

constexpr BlockType GetType(const Block* block) {
  return BlockFields::Type::Get<BlockType>(block->header);
}

constexpr size_t PayloadCapacity(BlockOrder order) {
  return OrderToSize(order) - sizeof(Block::header);
}

constexpr cpp17::optional<size_t> SizeForArrayPayload(const BlockType payload_type) {
  switch (payload_type) {
    case BlockType::kIntValue:
    case BlockType::kUintValue:
    case BlockType::kDoubleValue:
      return {sizeof(uint64_t)};
    case BlockType::kStringReference:
      return {sizeof(uint32_t)};

    default:
      return {};
  }
}

constexpr cpp17::optional<size_t> ArrayCapacity(BlockOrder order, BlockType type) {
  const auto size = SizeForArrayPayload(type);
  if (size.has_value()) {
    return (OrderToSize(order) - sizeof(Block::header) - sizeof(Block::payload)) / size.value();
  }

  return {};
}

constexpr size_t BlockSizeForPayload(size_t payload_size) {
  return std::max(payload_size + sizeof(Block::header), kMinOrderSize);
}

// For array types, get a pointer to a specific slot in the array.
// Because the return type is a pointer to a value inside the input block,
// the output type needs to be const if the input block is const.
// If the index is out of bounds, return nullptr.
// If the input block is not an array type, return nullptr.
template <typename T, typename B,
          // check that B is a Block* of some kind
          typename = std::enable_if_t<std::is_same<typename std::decay_t<B>, Block>::value>,
          // check that the return pointer is at least as const as the input block
          typename = std::enable_if_t<std::is_const<B>::value ? std::is_const<T>::value : true>>
constexpr T* GetArraySlot(B* block, size_t index) {
  if (GetType(block) != BlockType::kArrayValue) {
    return nullptr;
  }

  const auto capacity = ArrayCapacity(
      GetOrder(block), ArrayBlockPayload::EntryType::Get<BlockType>(block->payload.u64));
  if (!capacity.has_value()) {
    return nullptr;
  }

  if (index > capacity.value()) {
    return nullptr;
  }

  T* arr = reinterpret_cast<T*>(&block->payload);
  return arr + index + (sizeof(uint64_t) / sizeof(T)) /* skip inline payload */;
}

// Get a BlockIndex pointing to a string reference from a string array `Block`.
//
// This can't return a pointer because the VMO representation of block indexes is smaller
// than the C++ one, so a string reference's BlockIndex written directly to the array
// through the pointer would overwrite other data.
inline cpp17::optional<BlockIndex> GetArraySlotForString(const Block* block, size_t index) {
  const auto* tmp = GetArraySlot<const uint32_t>(block, index);
  if (tmp == nullptr) {
    return {};
  }

  return static_cast<const BlockIndex>(*tmp);
}

// Set the value of a string array at `index_into_array` with a `BlockIndex`, `value`,
// pointing to a string reference.
//
// Necessary to have a helper function instead of returning a mutable pointer into the array
// for the reasons noted on `GetArraySlotForString`.
inline void SetArraySlotForString(Block* block, size_t index_into_array, BlockIndex value) {
  auto* slot = GetArraySlot<uint32_t>(block, index_into_array);
  if (slot == nullptr) {
    return;
  }

  auto value_as_u32 = static_cast<uint32_t>(value);
  *slot = value_as_u32;
}

constexpr size_t kMaxPayloadSize = kMaxOrderSize - sizeof(Block::header);

}  // namespace internal
}  // namespace inspect

#endif  // LIB_INSPECT_CPP_VMO_BLOCK_H_
