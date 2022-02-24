// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_PACKET_H_
#define SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_PACKET_H_

#include <fuchsia/hardware/ethernet/c/banjo.h>
#include <lib/operation/ethernet.h>
#include <lib/stdcompat/span.h>
#include <zircon/types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include <fbl/intrusive_double_list.h>
#include <fbl/slab_allocator.h>
#include <src/connectivity/wlan/lib/mlme/rust/c-binding/bindings.h>
#include <wlan/common/logging.h>
#include <wlan/mlme/wlan.h>

typedef struct ethernet_netbuf ethernet_netbuf_t;

namespace wlan {

// A Buffer is a type that points at bytes and knows how big it is. For now, it
// can also carry out-of-band control data.
class Buffer {
 public:
  virtual ~Buffer() {}
  virtual uint8_t* data() = 0;
  virtual uint8_t* ctrl() = 0;
  virtual size_t capacity() const = 0;
  virtual void clear(size_t len) = 0;
  enum Size {
    kSmall,
    kLarge,
    kHuge,
  };
};

// Huge buffers are used for sending lots of data between drivers and the
// wlanstack.
constexpr size_t kHugeSlabs = 2;
constexpr size_t kHugeBuffers = 8;
constexpr size_t kHugeBufferSize = 16384;
// Large buffers can hold the largest 802.11 MSDU, standard Ethernet MTU,
// or HT A-MSDU of size 3,839 bytes.
constexpr size_t kLargeSlabs = 20;
constexpr size_t kLargeBuffers = 32;
constexpr size_t kLargeBufferSize = 4096;
// Small buffers are for smaller control packets within the driver stack itself
// and for transfering small 802.11 frames as well.
constexpr size_t kSmallSlabs = 40;
constexpr size_t kSmallBuffers = 512;
constexpr size_t kSmallBufferSize = 256;

// TODO(eyw): Revisit SlabAllocator counter behavior in Zircon to remove the
// dependency on template
template <typename, typename, typename, bool>
class BufferDebugger;

template <typename SmallSlabAllocator, typename LargeSlabAllocator, typename HugeSlabAllocator>
class BufferDebugger<SmallSlabAllocator, LargeSlabAllocator, HugeSlabAllocator, true> {
 public:
  static void Fail(Buffer::Size size) {
    // TODO(eyw): Use a timer to throttle logging
    switch (size) {
      case Buffer::Size::kSmall:
        if (is_exhausted_small_) {
          return;
        }
        is_exhausted_small_ = true;
        debugbuf("Small buffer exhausted.\n");
        break;
      case Buffer::Size::kLarge:
        if (is_exhausted_large_) {
          return;
        }
        is_exhausted_large_ = true;
        debugbuf("Large buffer exhausted.\n");
        break;
      case Buffer::Size::kHuge:
        if (is_exhausted_huge_) {
          return;
        }
        is_exhausted_huge_ = true;
        debugbuf("Huge buffer exhausted.\n");
        break;
    }
    PrintCounters();
  }
  static void PrintCounters() {
    // 4 numbers for each allocator:
    // current buffers in use / historical maximum buffers in use /
    // current allocator capacity / maximum allocator capacity
    debugbuf(
        "usage(in_use/in_use_max/current_capacity/max_capacity)\n Small: "
        "%zu/%zu/%zu/%zu, "
        "Large: %zu/%zu/%zu/%zu, Huge: %zu/%zu/%zu/%zu\n",
        SmallSlabAllocator::obj_count(), SmallSlabAllocator::max_obj_count(),
        SmallSlabAllocator::slab_count() * kSmallBuffers, kSmallSlabs * kSmallBuffers,
        LargeSlabAllocator::obj_count(), LargeSlabAllocator::max_obj_count(),
        LargeSlabAllocator::slab_count() * kLargeBuffers, kLargeSlabs * kLargeBuffers,
        HugeSlabAllocator::obj_count(), HugeSlabAllocator::max_obj_count(),
        HugeSlabAllocator::slab_count() * kHugeBuffers, kHugeSlabs * kHugeBuffers);
  }

 private:
  static bool is_exhausted_small_;
  static bool is_exhausted_large_;
  static bool is_exhausted_huge_;
};

template <typename SmallSlabAllocator, typename LargeSlabAllocator, typename HugeSlabAllocator>
bool BufferDebugger<SmallSlabAllocator, LargeSlabAllocator, HugeSlabAllocator,
                    true>::is_exhausted_small_ = false;

template <typename SmallSlabAllocator, typename LargeSlabAllocator, typename HugeSlabAllocator>
bool BufferDebugger<SmallSlabAllocator, LargeSlabAllocator, HugeSlabAllocator,
                    true>::is_exhausted_large_ = false;

template <typename SmallSlabAllocator, typename LargeSlabAllocator, typename HugeSlabAllocator>
bool BufferDebugger<SmallSlabAllocator, LargeSlabAllocator, HugeSlabAllocator,
                    true>::is_exhausted_huge_ = false;

template <typename SmallSlabAllocator, typename LargeSlabAllocator, typename HugeSlabAllocator>
class BufferDebugger<SmallSlabAllocator, LargeSlabAllocator, HugeSlabAllocator, false> {
 public:
  static void Fail(...) {}
  static void PrintCounters() {}
};

constexpr size_t kCtrlSize = 32;

namespace internal {
template <size_t BufferSize>
class FixedBuffer : public Buffer {
 public:
  uint8_t* data() override { return data_; }
  uint8_t* ctrl() override { return ctrl_; }
  size_t capacity() const override { return BufferSize; }
  void clear(size_t len) override {
    std::memset(data_, 0, std::min(BufferSize, len));
    std::memset(ctrl_, 0, kCtrlSize);
  }

 private:
  uint8_t data_[BufferSize];
  // Embedding the control data directly into the buffer is not ideal.
  // TODO(tkilbourn): replace this with a general solution.
  uint8_t ctrl_[kCtrlSize];
};
}  // namespace internal

constexpr size_t kSlabOverhead = 16;  // overhead for the slab allocator as a whole

template <size_t NumBuffers, size_t BufferSize>
class SlabBuffer;
template <size_t NumBuffers, size_t BufferSize>
using SlabBufferTraits = fbl::StaticSlabAllocatorTraits<
    std::unique_ptr<SlabBuffer<NumBuffers, BufferSize>>,
    sizeof(internal::FixedBuffer<BufferSize>) * NumBuffers + kSlabOverhead, ::fbl::Mutex,
    kBufferDebugEnabled ? fbl::SlabAllocatorOptions::EnableObjectCount
                        : fbl::SlabAllocatorOptions::None>;

// A SlabBuffer is an implementation of a Buffer that comes from a
// fbl::SlabAllocator. The size of the internal::FixedBuffer and the number of
// buffers is part of the typename of the SlabAllocator, so the SlabBuffer
// itself is also templated on these parameters.
template <size_t NumBuffers, size_t BufferSize>
class SlabBuffer final : public internal::FixedBuffer<BufferSize>,
                         public fbl::SlabAllocated<SlabBufferTraits<NumBuffers, BufferSize>> {};

using HugeBufferTraits = SlabBufferTraits<kHugeBuffers, kHugeBufferSize>;
using LargeBufferTraits = SlabBufferTraits<kLargeBuffers, kLargeBufferSize>;
using SmallBufferTraits = SlabBufferTraits<kSmallBuffers, kSmallBufferSize>;
using HugeBufferAllocator = fbl::SlabAllocator<HugeBufferTraits>;
using LargeBufferAllocator = fbl::SlabAllocator<LargeBufferTraits>;
using SmallBufferAllocator = fbl::SlabAllocator<SmallBufferTraits>;

// Gets a (slab allocated) Buffer with at least |len| bytes capacity.
std::unique_ptr<Buffer> GetBuffer(size_t len);

// A Packet wraps a buffer with information about the recipient/sender and
// length of the data within the buffer.
//
// Packets have their default move constructor invoked during testing, so their
// intrusive nodes need to permit moving.
class Packet
    : public fbl::DoublyLinkedListable<std::unique_ptr<Packet>, fbl::NodeOptions::AllowMove> {
 public:
  using value_type = uint8_t;
  using iterator = value_type*;
  using const_iterator = const value_type*;

  enum class Peer {
    kUnknown,
    kDevice,
    kWlan,
    kEthernet,
    kService,
  };

  Packet(std::unique_ptr<Buffer> buffer, size_t len);
  size_t Capacity() const { return buffer_->capacity(); }
  void clear() {
    ZX_DEBUG_ASSERT(!has_ext_data());
    buffer_->clear(len_);
    ctrl_len_ = 0;
  }

  void set_peer(Peer s) { peer_ = s; }
  Peer peer() const { return peer_; }

  const uint8_t* data() const { return buffer_->data(); }
  uint8_t* data() { return buffer_->data(); }

  size_t size() const { return len_; }

  iterator begin() { return data(); }
  const_iterator cbegin() const { return data(); }
  iterator end() { return begin() + size(); }
  const_iterator cend() const { return cbegin() + size(); }

  // Length can only be made shorter at this time.
  zx_status_t set_len(size_t len) {
    ZX_DEBUG_ASSERT(len <= len_);
    len_ = len;
    return ZX_OK;
  }
  size_t len() const { return len_; }

  template <typename T>
  const T* field(size_t offset) const {
    return FromBytes<T>(buffer_->data() + offset, len_ - offset);
  }

  template <typename T>
  T* mut_field(size_t offset) const {
    return FromBytes<T>(buffer_->data() + offset, len_ - offset);
  }

  template <typename T>
  bool has_ctrl_data() const {
    return ctrl_len_ == sizeof(T);
  }

  template <typename T>
  const T* ctrl_data() const {
    static_assert(std::is_standard_layout<T>::value, "Control data must have standard layout");
    static_assert(kCtrlSize >= sizeof(T), "Control data type too large for Buffer ctrl_data field");
    return FromBytes<T>(buffer_->ctrl(), ctrl_len_);
  }

  template <typename T>
  void CopyCtrlFrom(const T& t) {
    static_assert(std::is_standard_layout<T>::value, "Control data must have standard layout");
    static_assert(kCtrlSize >= sizeof(T), "Control data type too large for Buffer ctrl_data field");
    std::memcpy(buffer_->ctrl(), &t, sizeof(T));
    ctrl_len_ = sizeof(T);
  }

  zx_status_t CopyFrom(const void* src, size_t len, size_t offset);

  wlan_tx_packet_t AsWlanTxPacket();

  bool has_ext_data() const { return ext_data_.has_value(); }
  void set_ext_data(eth::BorrowedOperation<> netbuf, uint16_t offset) {
    ZX_DEBUG_ASSERT(!has_ext_data());
    ext_data_ = std::move(netbuf);
    ext_offset_ = offset;
  }
  std::optional<eth::BorrowedOperation<>>& ext_data() { return ext_data_; }
  uint16_t ext_offset() const { return ext_offset_; }

 private:
  std::unique_ptr<Buffer> buffer_;
  size_t len_ = 0;
  size_t ctrl_len_ = 0;
  Peer peer_ = Peer::kUnknown;
  std::optional<eth::BorrowedOperation<>> ext_data_;
  uint16_t ext_offset_ = 0;
};

mlme_in_buf_t IntoRustInBuf(std::unique_ptr<Packet> packet);
std::unique_ptr<Packet> FromRustOutBuf(mlme_out_buf_t buf);
bool IsBodyAligned(const Packet& pkt);

class PacketQueue {
 public:
  using PacketPtr = std::unique_ptr<Packet>;
  PacketQueue() = default;

  bool is_empty() const { return queue_.is_empty(); }
  size_t size() const { return size_; }
  void clear() {
    queue_.clear();
    size_ = 0;
  }

  void Enqueue(PacketPtr packet) {
    ZX_DEBUG_ASSERT(packet != nullptr);
    queue_.push_front(std::move(packet));
    size_++;
  }
  void UndoEnqueue() {
    ZX_DEBUG_ASSERT(!is_empty());
    queue_.pop_front();
    size_--;
  }

  PacketPtr Dequeue() {
    auto packet = queue_.pop_back();
    if (packet) {
      size_--;
    }
    return packet;
  }

  PacketQueue Drain() { return {std::move(queue_), std::exchange(size_, 0)}; }

 private:
  PacketQueue(fbl::DoublyLinkedList<PacketPtr>&& queue, size_t size)
      : queue_(std::move(queue)), size_(size) {}
  fbl::DoublyLinkedList<PacketPtr> queue_;
  size_t size_ = 0;
};

// Gets a Packet setup for a specific use-case, backed by a slab allocated
// Buffer with at least |len| bytes capacity.
std::unique_ptr<Packet> GetEthPacket(size_t len);
std::unique_ptr<Packet> GetWlanPacket(size_t len);
std::unique_ptr<Packet> GetSvcPacket(size_t len);

extern mlme_buffer_provider_ops_t rust_buffer_provider;

}  // namespace wlan

// Declaration of static slab allocators.
FWD_DECL_STATIC_SLAB_ALLOCATOR(::wlan::HugeBufferTraits);
FWD_DECL_STATIC_SLAB_ALLOCATOR(::wlan::LargeBufferTraits);
FWD_DECL_STATIC_SLAB_ALLOCATOR(::wlan::SmallBufferTraits);

#endif  // SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_PACKET_H_
