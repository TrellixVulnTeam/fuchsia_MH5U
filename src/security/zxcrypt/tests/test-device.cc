// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "test-device.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fidl/fuchsia.device/cpp/wire.h>
#include <fuchsia/device/c/fidl.h>
#include <fuchsia/hardware/ramdisk/c/fidl.h>
#include <inttypes.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>
#include <lib/fit/defer.h>
#include <lib/zircon-internal/debug.h>
#include <lib/zx/clock.h>
#include <lib/zx/fifo.h>
#include <lib/zx/vmo.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <threads.h>
#include <unistd.h>
#include <zircon/assert.h>
#include <zircon/types.h>

#include <memory>
#include <string_view>

#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>
#include <fbl/string.h>
#include <fbl/unique_fd.h>
#include <ramdevice-client/ramdisk.h>
#include <zxtest/zxtest.h>

#include "src/lib/storage/fs_management/cpp/fvm.h"
#include "src/lib/storage/fs_management/cpp/mount.h"
#include "src/security/zxcrypt/client.h"
#include "src/security/zxcrypt/fdio-volume.h"
#include "src/security/zxcrypt/volume.h"
#include "src/storage/fvm/format.h"

#define ZXDEBUG 0

namespace zxcrypt {
namespace testing {
namespace {

// No test step should take longer than this
const zx::duration kTimeout = zx::sec(3);

// FVM driver library
const char* kFvmDriver = "/boot/driver/fvm.so";

// Translates |result| into a zx_status_t.
zx_status_t ToStatus(ssize_t result) {
  return result < 0 ? static_cast<zx_status_t>(result) : ZX_OK;
}

}  // namespace

TestDevice::TestDevice() {
  memset(fvm_part_path_, 0, sizeof(fvm_part_path_));
  memset(&req_, 0, sizeof(req_));
}

TestDevice::~TestDevice() {
  Disconnect();
  DestroyRamdisk();
  if (need_join_) {
    int res;
    thrd_join(tid_, &res);
  }
}

void TestDevice::SetupDevmgr() {
  driver_integration_test::IsolatedDevmgr::Args args;

  // We explicitly bind drivers ourselves, and don't want the block watcher
  // racing with us to call Bind.
  args.disable_block_watcher = true;

  ASSERT_EQ(driver_integration_test::IsolatedDevmgr::Create(&args, &devmgr_), ZX_OK);
  fbl::unique_fd ctl;
  ASSERT_EQ(device_watcher::RecursiveWaitForFile(devmgr_.devfs_root(),
                                                 "sys/platform/00:00:2d/ramctl", &ctl),
            ZX_OK);
}

void TestDevice::Create(size_t device_size, size_t block_size, bool fvm, Volume::Version version) {
  ASSERT_LT(device_size, SSIZE_MAX);
  if (fvm) {
    ASSERT_NO_FATAL_FAILURE(CreateFvmPart(device_size, block_size));
  } else {
    ASSERT_NO_FATAL_FAILURE(CreateRamdisk(device_size, block_size));
  }

  crypto::digest::Algorithm digest;
  switch (version) {
    case Volume::kAES256_XTS_SHA256:
      digest = crypto::digest::kSHA256;
      break;
    default:
      digest = crypto::digest::kUninitialized;
      break;
  }

  size_t digest_len;
  key_.Clear();
  ASSERT_OK(crypto::digest::GetDigestLen(digest, &digest_len));
  ASSERT_OK(key_.Generate(digest_len));
}

void TestDevice::Bind(Volume::Version version, bool fvm) {
  ASSERT_NO_FATAL_FAILURE(Create(kDeviceSize, kBlockSize, fvm, version));

  zxcrypt::VolumeManager volume_manager(parent(), devfs_root());
  zx::channel zxc_client_chan;
  ASSERT_OK(volume_manager.OpenClient(kTimeout, zxc_client_chan));
  EncryptedVolumeClient volume_client(std::move(zxc_client_chan));
  ASSERT_OK(volume_client.Format(key_.get(), key_.len(), 0));

  ASSERT_NO_FATAL_FAILURE(Connect());
}

void TestDevice::BindFvmDriver() {
  // Binds the FVM driver to the active ramdisk_.
  fdio_t* io = fdio_unsafe_fd_to_io(ramdisk_get_block_fd(ramdisk_));
  ASSERT_NOT_NULL(io);
  auto resp = fidl::WireCall<fuchsia_device::Controller>(
                  zx::unowned_channel(fdio_unsafe_borrow_channel(io)))
                  ->Bind(fidl::StringView::FromExternal(kFvmDriver));
  zx_status_t status = resp.status();
  fdio_unsafe_release(io);
  ASSERT_EQ(status, ZX_OK);
  ASSERT_TRUE(resp->result.is_response());
}

void TestDevice::Rebind() {
  const char* sep = strrchr(ramdisk_get_path(ramdisk_), '/');
  ASSERT_NOT_NULL(sep);

  Disconnect();
  zxcrypt_.reset();
  fvm_part_.reset();

  if (strlen(fvm_part_path_) != 0) {
    // We need to explicitly rebind FVM here, since now that we're not
    // relying on the system-wide block-watcher, the driver won't rebind by
    // itself.
    fdio_t* io = fdio_unsafe_fd_to_io(ramdisk_get_block_fd(ramdisk_));
    ASSERT_NOT_NULL(io);
    zx_status_t call_status = ZX_OK;
    ;
    auto resp = fidl::WireCall<fuchsia_device::Controller>(
                    zx::unowned_channel(fdio_unsafe_borrow_channel(io)))
                    ->Rebind(fidl::StringView::FromExternal(kFvmDriver));
    zx_status_t status = resp.status();
    if (resp->result.is_err()) {
      call_status = resp->result.err();
    }
    fdio_unsafe_release(io);
    ASSERT_OK(status);
    ASSERT_OK(call_status);
    fbl::unique_fd dev_root = devfs_root();
    ASSERT_EQ(device_watcher::RecursiveWaitForFile(dev_root, fvm_part_path_, &fvm_part_), ZX_OK);
    parent_caller_.reset(fvm_part_.get());
  } else {
    ASSERT_EQ(ramdisk_rebind(ramdisk_), ZX_OK);
    parent_caller_.reset(ramdisk_get_block_fd(ramdisk_));
  }
  ASSERT_NO_FATAL_FAILURE(Connect());
}

void TestDevice::SleepUntil(uint64_t num, bool deferred) {
  fbl::AutoLock lock(&lock_);
  ASSERT_EQ(wake_after_, 0);
  ASSERT_NE(num, 0);
  wake_after_ = num;
  wake_deadline_ = zx::deadline_after(kTimeout);
  ASSERT_EQ(thrd_create(&tid_, TestDevice::WakeThread, this), thrd_success);
  need_join_ = true;
  if (deferred) {
    uint32_t flags = fuchsia_hardware_ramdisk_RAMDISK_FLAG_RESUME_ON_WAKE;
    ASSERT_OK(ramdisk_set_flags(ramdisk_, flags));
  }
  uint64_t sleep_after = 0;
  ASSERT_OK(ramdisk_sleep_after(ramdisk_, sleep_after));
}

void TestDevice::WakeUp() {
  if (need_join_) {
    fbl::AutoLock lock(&lock_);
    ASSERT_NE(wake_after_, 0);
    int res;
    ASSERT_EQ(thrd_join(tid_, &res), thrd_success);
    need_join_ = false;
    wake_after_ = 0;
    EXPECT_EQ(res, 0);
  }
}

int TestDevice::WakeThread(void* arg) {
  TestDevice* device = static_cast<TestDevice*>(arg);
  fbl::AutoLock lock(&device->lock_);

  // Always send a wake-up call; even if we failed to go to sleep.
  auto cleanup = fit::defer([&] { ramdisk_wake(device->ramdisk_); });

  // Loop until timeout, |wake_after_| txns received, or error getting counts
  ramdisk_block_write_counts_t counts;
  do {
    zx::nanosleep(zx::deadline_after(zx::msec(100)));
    if (device->wake_deadline_ < zx::clock::get_monotonic()) {
      printf("Received %lu of %lu transactions before timing out.\n", counts.received,
             device->wake_after_);
      return ZX_ERR_TIMED_OUT;
    }
    zx_status_t status = ramdisk_get_block_counts(device->ramdisk_, &counts);
    if (status != ZX_OK) {
      return status;
    }
  } while (counts.received < device->wake_after_);
  return ZX_OK;
}

void TestDevice::ReadFd(zx_off_t off, size_t len) {
  ASSERT_OK(ToStatus(lseek(off)));
  ASSERT_OK(ToStatus(read(off, len)));
  ASSERT_EQ(memcmp(as_read_.get() + off, to_write_.get() + off, len), 0);
}

void TestDevice::WriteFd(zx_off_t off, size_t len) {
  ASSERT_OK(ToStatus(lseek(off)));
  ASSERT_OK(ToStatus(write(off, len)));
}

void TestDevice::ReadVmo(zx_off_t off, size_t len) {
  ASSERT_OK(block_fifo_txn(BLOCKIO_READ, off, len));
  off *= block_size_;
  len *= block_size_;
  ASSERT_OK(vmo_read(off, len));
  ASSERT_EQ(memcmp(as_read_.get() + off, to_write_.get() + off, len), 0);
}

void TestDevice::WriteVmo(zx_off_t off, size_t len) {
  ASSERT_OK(vmo_write(off * block_size_, len * block_size_));
  ASSERT_OK(block_fifo_txn(BLOCKIO_WRITE, off, len));
}

void TestDevice::Corrupt(uint64_t blkno, key_slot_t slot) {
  uint8_t block[block_size_];

  fbl::unique_fd fd = parent();
  ASSERT_OK(ToStatus(::lseek(fd.get(), blkno * block_size_, SEEK_SET)));
  ASSERT_OK(ToStatus(::read(fd.get(), block, block_size_)));

  std::unique_ptr<FdioVolume> volume;
  ASSERT_OK(FdioVolume::Unlock(parent(), key_, 0, &volume));

  zx_off_t off;
  ASSERT_OK(volume->GetSlotOffset(slot, &off));
  int flip = 1U << (rand() % 8);
  block[off] ^= static_cast<uint8_t>(flip);

  ASSERT_OK(ToStatus(::lseek(fd.get(), blkno * block_size_, SEEK_SET)));
  ASSERT_OK(ToStatus(::write(fd.get(), block, block_size_)));
}

// Private methods

void TestDevice::CreateRamdisk(size_t device_size, size_t block_size) {
  fbl::AllocChecker ac;
  size_t count = fbl::round_up(device_size, block_size) / block_size;
  to_write_.reset(new (&ac) uint8_t[device_size]);
  ASSERT_TRUE(ac.check());
  for (size_t i = 0; i < device_size; ++i) {
    to_write_[i] = static_cast<uint8_t>(rand());
  }

  as_read_.reset(new (&ac) uint8_t[device_size]);
  ASSERT_TRUE(ac.check());
  memset(as_read_.get(), 0, block_size);

  fbl::unique_fd devfs_root_fd = devfs_root();
  ASSERT_EQ(ramdisk_create_at(devfs_root_fd.get(), block_size, count, &ramdisk_), ZX_OK);

  fbl::unique_fd ramdisk_ignored;
  device_watcher::RecursiveWaitForFile(devfs_root_fd, ramdisk_get_path(ramdisk_), &ramdisk_ignored);

  parent_caller_.reset(ramdisk_get_block_fd(ramdisk_));

  block_size_ = block_size;
  block_count_ = count;
}

void TestDevice::DestroyRamdisk() {
  if (ramdisk_ != nullptr) {
    ramdisk_destroy(ramdisk_);
    ramdisk_ = nullptr;
  }
}

// Creates a ramdisk, formats it, and binds to it.
void TestDevice::CreateFvmPart(size_t device_size, size_t block_size) {
  // Calculate total size of data + metadata.
  size_t slice_count = fbl::round_up(device_size, fvm::kBlockSize) / fvm::kBlockSize;
  fvm::Header fvm_header =
      fvm::Header::FromSliceCount(fvm::kMaxUsablePartitions, slice_count, fvm::kBlockSize);

  ASSERT_NO_FATAL_FAILURE(CreateRamdisk(fvm_header.fvm_partition_size, block_size));

  // Format the ramdisk as FVM
  ASSERT_OK(fs_management::FvmInit(ramdisk_get_block_fd(ramdisk_), fvm::kBlockSize));

  // Bind the FVM driver to the now-formatted disk
  ASSERT_NO_FATAL_FAILURE(BindFvmDriver());

  // Wait for the FVM driver to expose a block device, then open it
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/fvm", ramdisk_get_path(ramdisk_));
  fbl::unique_fd dev_root = devfs_root();
  fbl::unique_fd fvm_fd;
  ASSERT_EQ(device_watcher::RecursiveWaitForFile(dev_root, path, &fvm_fd), ZX_OK);

  // Allocate a FVM partition with the last slice unallocated.
  alloc_req_t req;
  memset(&req, 0, sizeof(alloc_req_t));
  req.slice_count = (kDeviceSize / fvm::kBlockSize) - 1;
  memcpy(req.type, zxcrypt_magic, sizeof(zxcrypt_magic));
  for (uint8_t i = 0; i < BLOCK_GUID_LEN; ++i) {
    req.guid[i] = i;
  }
  snprintf(req.name, BLOCK_NAME_LEN, "data");
  auto fvm_part_or =
      fs_management::FvmAllocatePartitionWithDevfs(dev_root.get(), fvm_fd.get(), &req);
  ASSERT_EQ(fvm_part_or.status_value(), ZX_OK);
  fvm_part_ = *std::move(fvm_part_or);
  parent_caller_.reset(fvm_part_.get());

  // Save the topological path for rebinding.  The topological path will be
  // consistent after rebinding the ramdisk, whereas the
  // /dev/class/block/[NNN] will issue a new number.
  size_t out_len;
  zx_status_t status;
  zx_status_t call_status;
  auto resp =
      fidl::WireCall<fuchsia_device::Controller>(zx::unowned_channel(parent_channel()->get()))
          ->GetTopologicalPath();
  status = resp.status();

  if (resp->result.is_err()) {
    call_status = resp->result.err();
  } else {
    call_status = ZX_OK;
    auto& r = resp->result.response();
    out_len = r.path.size();
    memcpy(fvm_part_path_, r.path.data(), r.path.size());
  }

  ASSERT_EQ(status, ZX_OK);
  ASSERT_EQ(call_status, ZX_OK);
  // Strip off the leading /dev/; because we use an isolated devmgr, we need
  // relative paths, but ControllerGetTopologicalPath returns an absolute path
  // with the assumption that devfs is rooted at /dev.
  static constexpr std::string_view kHeader = "/dev/";
  ASSERT_TRUE(out_len > kHeader.size());
  ASSERT_TRUE(kHeader ==
              std::string_view(fvm_part_path_, std::min(kHeader.size(), strlen(fvm_part_path_))));
  memmove(fvm_part_path_, fvm_part_path_ + kHeader.size(), out_len - kHeader.size());
  fvm_part_path_[out_len - kHeader.size()] = 0;
}

void TestDevice::Connect() {
  ZX_DEBUG_ASSERT(!zxcrypt_);

  volume_manager_.reset(new zxcrypt::VolumeManager(parent(), devfs_root()));
  zx::channel zxc_client_chan;
  ASSERT_OK(volume_manager_->OpenClient(kTimeout, zxc_client_chan));

  EncryptedVolumeClient volume_client(std::move(zxc_client_chan));
  zx_status_t rc;
  // Unseal may fail because the volume is already unsealed, so we also allow
  // ZX_ERR_INVALID_STATE here.  If we fail to unseal the volume, the
  // volume_->Open() call below will fail, so this is safe to ignore.
  rc = volume_client.Unseal(key_.get(), key_.len(), 0);
  ASSERT_TRUE(rc == ZX_OK || rc == ZX_ERR_BAD_STATE);
  ASSERT_OK(volume_manager_->OpenInnerBlockDevice(kTimeout, &zxcrypt_));
  zxcrypt_caller_.reset(zxcrypt_.get());

  fuchsia_hardware_block_BlockInfo block_info;
  zx_status_t status;
  ASSERT_OK(fuchsia_hardware_block_BlockGetInfo(zxcrypt_channel()->get(), &status, &block_info));
  ASSERT_OK(status);
  block_size_ = block_info.block_size;
  block_count_ = block_info.block_count;

  zx::fifo fifo;
  ASSERT_OK(fuchsia_hardware_block_BlockGetFifo(zxcrypt_channel()->get(), &status,
                                                fifo.reset_and_get_address()));
  ASSERT_OK(status);
  req_.group = 0;

  client_ = std::make_unique<block_client::Client>(std::move(fifo));

  // Create the vmo and get a transferable handle to give to the block server
  ASSERT_OK(zx::vmo::create(size(), 0, &vmo_));
  zx::vmo xfer_vmo;
  ASSERT_EQ(vmo_.duplicate(ZX_RIGHT_SAME_RIGHTS, &xfer_vmo), ZX_OK);
  fuchsia_hardware_block_VmoId vmoid;
  ASSERT_OK(fuchsia_hardware_block_BlockAttachVmo(zxcrypt_channel()->get(), xfer_vmo.release(),
                                                  &status, &vmoid));
  ASSERT_OK(status);
  req_.vmoid = vmoid.id;
}

void TestDevice::Disconnect() {
  if (volume_manager_) {
    zx::channel zxc_client_chan;
    volume_manager_->OpenClient(kTimeout, zxc_client_chan);
    if (zxc_client_chan) {
      EncryptedVolumeClient volume_client(std::move(zxc_client_chan));
      volume_client.Seal();
    }
  }

  if (client_) {
    zx_status_t status;
    fuchsia_hardware_block_BlockCloseFifo(zxcrypt_channel()->get(), &status);
    memset(&req_, 0, sizeof(req_));
    client_ = nullptr;
  }
  zxcrypt_.reset();
  volume_manager_.reset();
  block_size_ = 0;
  block_count_ = 0;
  vmo_.reset();
}

}  // namespace testing
}  // namespace zxcrypt
