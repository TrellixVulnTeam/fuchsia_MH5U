// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string.h>

#include <sstream>

#include <gtest/gtest.h>

#include "src/lib/storage/block_client/cpp/fake_block_device.h"
#include "src/storage/f2fs/f2fs.h"
#include "unit_lib.h"

namespace f2fs {
namespace {

using block_client::FakeBlockDevice;

constexpr uint64_t kMkfsBlockCount = 819200;
constexpr uint32_t kMkfsBlockSize = 512;

const MkfsOptions default_option;

enum class ArgType {
  Label,
  SegsPerSec,
  SecsPerZone,
  Extension,
  Heap,
  OP,
};

void AddArg(std::vector<const char *> &argv, ArgType t, const char *val) {
  switch (t) {
    case ArgType::Label:
      argv.push_back("-l");
      break;
    case ArgType::SegsPerSec:
      argv.push_back("-s");
      break;
    case ArgType::SecsPerZone:
      argv.push_back("-z");
      break;
    case ArgType::Extension:
      argv.push_back("-e");
      break;
    case ArgType::Heap:
      argv.push_back("-a");
      break;
    case ArgType::OP:
      argv.push_back("-o");
      break;
  }
  argv.push_back(val);
}

__UNUSED void PrintArg(std::vector<const char *> &argv) {
  std::string str_args;

  for (auto &i : argv) {
    str_args += i;
    str_args += " ";
  }
  FX_LOGS(INFO) << "mkfs arg: " << str_args;
}

void DoMkfs(std::unique_ptr<Bcache> bcache, std::vector<const char *> &argv, bool expect_success,
            std::unique_ptr<Bcache> *out) {
  MkfsOptions mkfs_options;

  zx_status_t status;
  zx::status<std::unique_ptr<Bcache>> make_return;
  if (status = ParseOptions(static_cast<int>(argv.size()), const_cast<char **>(argv.data()),
                            mkfs_options);
      status == ZX_OK) {
    make_return = Mkfs(mkfs_options, std::move(bcache));
    status = make_return.status_value();
  }

  if (expect_success) {
    ASSERT_EQ(status, ZX_OK);
    *out = std::move(*make_return);
  } else {
    ASSERT_NE(status, ZX_OK);
    *out = nullptr;
  }
}

void ReadSuperblock(Bcache *bc, Superblock *sb) { ASSERT_EQ(LoadSuperblock(bc, sb), ZX_OK); }

void ReadCheckpoint(Bcache *bc, Superblock &sb, Checkpoint *ckp) {
  char buf[4096];
  ASSERT_EQ(bc->Readblk(sb.segment0_blkaddr, buf), ZX_OK);
  memcpy(ckp, buf, sizeof(Checkpoint));
}

void VerifyLabel(Superblock &sb, const char *label) {
  std::string vol_label(label);
  std::u16string volume_name;
  AsciiToUnicode(vol_label, &volume_name);

  uint16_t volume_name_arr[512];
  volume_name.copy(reinterpret_cast<char16_t *>(volume_name_arr), vol_label.size());
  volume_name_arr[vol_label.size()] = '\0';
  ASSERT_EQ(memcmp(sb.volume_name, volume_name_arr, vol_label.size() + 1), 0);
}

void VerifySegsPerSec(Superblock &sb, uint32_t segs_per_sec) {
  ASSERT_EQ(sb.segs_per_sec, segs_per_sec);
}

void VerifySecsPerZone(Superblock &sb, uint32_t secs_per_zone) {
  ASSERT_EQ(sb.secs_per_zone, secs_per_zone);
}

void VerifyExtensionList(Superblock &sb, const char *extensions) {
  ASSERT_LE(sb.extension_count, static_cast<uint32_t>(kMaxExtension));
  uint32_t extension_iter = 0;
  for (const char *ext_item : kMediaExtList) {
    ASSERT_LT(extension_iter, sb.extension_count);
    ASSERT_EQ(strcmp(reinterpret_cast<char *>(sb.extension_list[extension_iter++]), ext_item), 0);
  }

  ASSERT_EQ(extension_iter, sizeof(kMediaExtList) / sizeof(const char *));

  std::istringstream iss(extensions);
  std::string token;

  while (std::getline(iss, token, ',')) {
    ASSERT_LE(extension_iter, sb.extension_count);
    ASSERT_EQ(strcmp(reinterpret_cast<char *>(sb.extension_list[extension_iter++]), token.c_str()),
              0);
    if (extension_iter >= kMaxExtension)
      break;
  }

  ASSERT_EQ(extension_iter, sb.extension_count);
}

void VerifyHeapBasedAllocation(Superblock &sb, Checkpoint &ckp, bool is_heap_based) {
  uint32_t total_zones =
      ((LeToCpu(sb.segment_count_main) - 1) / sb.segs_per_sec) / sb.secs_per_zone;
  ASSERT_GT(total_zones, static_cast<uint32_t>(6));

  uint32_t cur_seg[6];
  if (is_heap_based) {
    cur_seg[static_cast<int>(CursegType::kCursegHotNode)] =
        (total_zones - 1) * sb.segs_per_sec * sb.secs_per_zone +
        ((sb.secs_per_zone - 1) * sb.segs_per_sec);
    cur_seg[static_cast<int>(CursegType::kCursegWarmNode)] =
        cur_seg[static_cast<int>(CursegType::kCursegHotNode)] - sb.segs_per_sec * sb.secs_per_zone;
    cur_seg[static_cast<int>(CursegType::kCursegColdNode)] =
        cur_seg[static_cast<int>(CursegType::kCursegWarmNode)] - sb.segs_per_sec * sb.secs_per_zone;
    cur_seg[static_cast<int>(CursegType::kCursegHotData)] =
        cur_seg[static_cast<int>(CursegType::kCursegColdNode)] - sb.segs_per_sec * sb.secs_per_zone;
    cur_seg[static_cast<int>(CursegType::kCursegColdData)] = 0;
    cur_seg[static_cast<int>(CursegType::kCursegWarmData)] =
        cur_seg[static_cast<int>(CursegType::kCursegColdData)] + sb.segs_per_sec * sb.secs_per_zone;
  } else {
    cur_seg[static_cast<int>(CursegType::kCursegHotNode)] = 0;
    cur_seg[static_cast<int>(CursegType::kCursegWarmNode)] =
        cur_seg[static_cast<int>(CursegType::kCursegHotNode)] + sb.segs_per_sec * sb.secs_per_zone;
    cur_seg[static_cast<int>(CursegType::kCursegColdNode)] =
        cur_seg[static_cast<int>(CursegType::kCursegWarmNode)] + sb.segs_per_sec * sb.secs_per_zone;
    cur_seg[static_cast<int>(CursegType::kCursegHotData)] =
        cur_seg[static_cast<int>(CursegType::kCursegColdNode)] + sb.segs_per_sec * sb.secs_per_zone;
    cur_seg[static_cast<int>(CursegType::kCursegColdData)] =
        cur_seg[static_cast<int>(CursegType::kCursegHotData)] + sb.segs_per_sec * sb.secs_per_zone;
    cur_seg[static_cast<int>(CursegType::kCursegWarmData)] =
        cur_seg[static_cast<int>(CursegType::kCursegColdData)] + sb.segs_per_sec * sb.secs_per_zone;
  }

  ASSERT_EQ(ckp.cur_node_segno[0], cur_seg[static_cast<int>(CursegType::kCursegHotNode)]);
  ASSERT_EQ(ckp.cur_node_segno[1], cur_seg[static_cast<int>(CursegType::kCursegWarmNode)]);
  ASSERT_EQ(ckp.cur_node_segno[2], cur_seg[static_cast<int>(CursegType::kCursegColdNode)]);
  ASSERT_EQ(ckp.cur_data_segno[0], cur_seg[static_cast<int>(CursegType::kCursegHotData)]);
  ASSERT_EQ(ckp.cur_data_segno[1], cur_seg[static_cast<int>(CursegType::kCursegWarmData)]);
  ASSERT_EQ(ckp.cur_data_segno[2], cur_seg[static_cast<int>(CursegType::kCursegColdData)]);
}

void VerifyOP(Superblock &sb, Checkpoint &ckp, uint32_t op_ratio) {
  uint32_t overprov_segment_count =
      CpuToLe((LeToCpu(sb.segment_count_main) - LeToCpu(ckp.rsvd_segment_count)) * op_ratio / 100 +
              LeToCpu(ckp.rsvd_segment_count));
  ASSERT_EQ(ckp.overprov_segment_count, overprov_segment_count);
}

TEST(FormatFilesystemTest, MkfsOptionsLabel) {
  auto device = std::make_unique<FakeBlockDevice>(kMkfsBlockCount, kMkfsBlockSize);
  std::unique_ptr<Bcache> bc;
  bool readonly_device = false;
  ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

  char label[20];
  const char *default_label = "F2FS";

  // Check default label is written when there is no arg for label
  std::vector<const char *> argv = {"mkfs"};
  DoMkfs(std::move(bc), argv, true, &bc);

  Superblock sb = {};
  ReadSuperblock(bc.get(), &sb);
  VerifyLabel(sb, default_label);

  // Try with max size label (16)
  argv.clear();
  argv.push_back("mkfs");
  memcpy(label, "0123456789abcde", 16);
  AddArg(argv, ArgType::Label, label);
  DoMkfs(std::move(bc), argv, true, &bc);
  ReadSuperblock(bc.get(), &sb);
  VerifyLabel(sb, label);

  // Check failure with label size larger than max size
  argv.clear();
  argv.push_back("mkfs");
  memcpy(label, "0123456789abcdef", 17);
  AddArg(argv, ArgType::Label, label);
  DoMkfs(std::move(bc), argv, false, &bc);
}

TEST(FormatFilesystemTest, MkfsOptionsSegsPerSec) {
  auto device = std::make_unique<FakeBlockDevice>(kMkfsBlockCount, kMkfsBlockSize);
  std::unique_ptr<Bcache> bc;
  bool readonly_device = false;
  ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

  // Check default value
  std::vector<const char *> argv = {"mkfs"};
  DoMkfs(std::move(bc), argv, true, &bc);
  Superblock sb = {};
  ReadSuperblock(bc.get(), &sb);
  VerifySegsPerSec(sb, default_option.segs_per_sec);

  // Try with various values
  const uint32_t segs_per_sec_list[] = {1, 2, 4, 8};
  for (uint32_t segs_per_sec : segs_per_sec_list) {
    FX_LOGS(INFO) << "segs_per_sec = " << segs_per_sec;
    argv.clear();
    argv.push_back("mkfs");
    std::string tmp(std::to_string(segs_per_sec).c_str());
    AddArg(argv, ArgType::SegsPerSec, tmp.data());
    DoMkfs(std::move(bc), argv, true, &bc);
    ReadSuperblock(bc.get(), &sb);
    VerifySegsPerSec(sb, segs_per_sec);
  }

  // Check failure with zero
  argv.clear();
  argv.push_back("mkfs");
  std::string tmp("0");
  AddArg(argv, ArgType::SegsPerSec, tmp.data());
  DoMkfs(std::move(bc), argv, false, &bc);
}

TEST(FormatFilesystemTest, MkfsOptionsSecsPerZone) {
  auto device = std::make_unique<FakeBlockDevice>(kMkfsBlockCount, kMkfsBlockSize);
  std::unique_ptr<Bcache> bc;
  bool readonly_device = false;
  ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

  // Check default value
  std::vector<const char *> argv = {"mkfs"};
  DoMkfs(std::move(bc), argv, true, &bc);
  Superblock sb = {};
  ReadSuperblock(bc.get(), &sb);
  VerifySecsPerZone(sb, default_option.secs_per_zone);

  // Try with various values
  const uint32_t secs_per_zone_list[] = {1, 2, 4, 8};
  for (uint32_t secs_per_zone : secs_per_zone_list) {
    FX_LOGS(INFO) << "secs_per_zone = " << secs_per_zone;
    argv.clear();
    argv.push_back("mkfs");
    std::string tmp(std::to_string(secs_per_zone).c_str());
    AddArg(argv, ArgType::SecsPerZone, tmp.data());
    DoMkfs(std::move(bc), argv, true, &bc);
    ReadSuperblock(bc.get(), &sb);
    VerifySecsPerZone(sb, secs_per_zone);
  }

  // Check failure with zero
  argv.clear();
  argv.push_back("mkfs");
  char tmp[20];
  strcpy(tmp, "0");
  AddArg(argv, ArgType::SecsPerZone, "0");
  DoMkfs(std::move(bc), argv, false, &bc);
}

TEST(FormatFilesystemTest, MkfsOptionsExtensions) {
  auto device = std::make_unique<FakeBlockDevice>(kMkfsBlockCount, kMkfsBlockSize);
  std::unique_ptr<Bcache> bc;
  bool readonly_device = false;
  ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

  // Check default
  std::vector<const char *> argv = {"mkfs"};
  DoMkfs(std::move(bc), argv, true, &bc);
  Superblock sb = {};
  ReadSuperblock(bc.get(), &sb);
  VerifyExtensionList(sb, "");

  // Try with max extension counts
  std::string extensions("");
  for (uint32_t i = sizeof(kMediaExtList) / sizeof(const char *); i < kMaxExtension; ++i) {
    if (i > sizeof(kMediaExtList) / sizeof(const char *))
      extensions.append(",");
    extensions.append(std::to_string(i));
  }
  char ext_arg_buf[512];
  strcpy(ext_arg_buf, extensions.c_str());

  argv.clear();
  argv.push_back("mkfs");
  AddArg(argv, ArgType::Extension, ext_arg_buf);
  DoMkfs(std::move(bc), argv, true, &bc);
  ReadSuperblock(bc.get(), &sb);
  VerifyExtensionList(sb, extensions.c_str());

  // If exeeding max extension counts, only extensions within max count are valid
  extensions.append(",foo");
  strcpy(ext_arg_buf, extensions.c_str());

  argv.clear();
  argv.push_back("mkfs");
  AddArg(argv, ArgType::Extension, ext_arg_buf);
  DoMkfs(std::move(bc), argv, true, &bc);
  ReadSuperblock(bc.get(), &sb);
  VerifyExtensionList(sb, extensions.c_str());
}

TEST(FormatFilesystemTest, MkfsOptionsHeapBasedAlloc) {
  auto device = std::make_unique<FakeBlockDevice>(kMkfsBlockCount, kMkfsBlockSize);
  std::unique_ptr<Bcache> bc;
  bool readonly_device = false;
  ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

  // Check default
  std::vector<const char *> argv = {"mkfs"};
  DoMkfs(std::move(bc), argv, true, &bc);
  Superblock sb = {};
  ReadSuperblock(bc.get(), &sb);
  Checkpoint ckp = {};
  ReadCheckpoint(bc.get(), sb, &ckp);
  VerifyHeapBasedAllocation(sb, ckp, default_option.heap_based_allocation);

  // If arg set to 0, not using heap-based allocation
  argv.clear();
  argv.push_back("mkfs");
  std::string tmp("0");
  AddArg(argv, ArgType::Heap, tmp.data());
  DoMkfs(std::move(bc), argv, true, &bc);
  ReadSuperblock(bc.get(), &sb);
  ReadCheckpoint(bc.get(), sb, &ckp);
  VerifyHeapBasedAllocation(sb, ckp, false);

  // If arg set to 1, using heap-based allocation
  argv.clear();
  argv.push_back("mkfs");
  tmp = "1";
  AddArg(argv, ArgType::Heap, tmp.data());
  DoMkfs(std::move(bc), argv, true, &bc);
  ReadSuperblock(bc.get(), &sb);
  ReadCheckpoint(bc.get(), sb, &ckp);
  VerifyHeapBasedAllocation(sb, ckp, true);
}

TEST(FormatFilesystemTest, MkfsOptionsOverprovision) {
  auto device = std::make_unique<FakeBlockDevice>(kMkfsBlockCount, kMkfsBlockSize);
  std::unique_ptr<Bcache> bc;
  bool readonly_device = false;
  ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

  // Check default
  std::vector<const char *> argv = {"mkfs"};
  DoMkfs(std::move(bc), argv, true, &bc);
  Superblock sb = {};
  ReadSuperblock(bc.get(), &sb);
  Checkpoint ckp = {};
  ReadCheckpoint(bc.get(), sb, &ckp);

  // Try with various values
  const uint32_t overprovision_ratio_list[] = {3, 5, 7};
  for (uint32_t overprovision_ratio : overprovision_ratio_list) {
    FX_LOGS(INFO) << "overprovision_ratio = " << overprovision_ratio;
    argv.clear();
    argv.push_back("mkfs");
    std::string tmp(std::to_string(overprovision_ratio).c_str());
    AddArg(argv, ArgType::OP, tmp.data());
    DoMkfs(std::move(bc), argv, true, &bc);
    ReadCheckpoint(bc.get(), sb, &ckp);
    VerifyOP(sb, ckp, overprovision_ratio);
  }

  // Check failure with zero
  argv.clear();
  argv.push_back("mkfs");
  std::string tmp("0");
  AddArg(argv, ArgType::OP, tmp.data());
  DoMkfs(std::move(bc), argv, false, &bc);
}
#if 0  //[TODO] fix errors with core.arm64-release
      // https://ci.chromium.org/ui/p/fuchsia/builders/try/core.arm64-release/b8837818659754240433/overview
TEST(FormatFilesystemTest, MkfsOptionsMixed) {
  auto device = std::make_unique<FakeBlockDevice>(kMkfsBlockCount, kMkfsBlockSize);
  std::unique_ptr<Bcache> bc;
  bool readonly_device = false;
  ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

  const char *label_list[] = {"aa", "bbbbb"};
  const uint32_t segs_per_sec_list[] = {2, 4};
  const uint32_t secs_per_zone_list[] = {2, 4};
  const char *ext_list[] = {"foo", "foo,bar"};
  const uint32_t heap_based_list[] = {0};
  const uint32_t overprovision_list[] = {7, 9};

  for (const char *label : label_list) {
    for (const uint32_t segs_per_sec : segs_per_sec_list) {
      for (const uint32_t secs_per_zone : secs_per_zone_list) {
        for (const char *extensions : ext_list) {
          for (const uint32_t heap_based : heap_based_list) {
            for (const uint32_t overprovision : overprovision_list) {
              std::vector<const char *> argv = {"mkfs"};
              char ext_arg_buf[512];

              AddArg(argv, ArgType::Label, label);
              AddArg(argv, ArgType::SegsPerSec, std::to_string(segs_per_sec).c_str());
              AddArg(argv, ArgType::SecsPerZone, std::to_string(secs_per_zone).c_str());

              // Mkfs will tokenize extension list using strtok().
              // Since strtok() needs to modify original string, use non-const buffer to deliver
              // argument
              strcpy(ext_arg_buf, extensions);
              AddArg(argv, ArgType::Extension, ext_arg_buf);

              AddArg(argv, ArgType::Heap, std::to_string(heap_based).c_str());
              AddArg(argv, ArgType::OP, std::to_string(overprovision).c_str());

              PrintArg(argv);
              DoMkfs(bc.get(), argv, true);

              Superblock sb = {};
              ReadSuperblock(bc.get(), &sb);

              Checkpoint ckp = {};
              ReadCheckpoint(bc.get(), sb, &ckp);

              VerifyLabel(sb, label);
              VerifySegsPerSec(sb, segs_per_sec);
              VerifySecsPerZone(sb, secs_per_zone);
              VerifyExtensionList(sb, extensions);
              VerifyHeapBasedAllocation(sb, ckp, (heap_based != 0));
              VerifyOP(sb, ckp, overprovision);
            }
          }
        }
      }
    }
  }
}
#endif

TEST(FormatFilesystemTest, BlockSize) {
  uint32_t block_size_array[] = {256, 512, 1024, 2048, 4096, 8192};
  uint32_t total_size = 104'857'600;

  for (uint32_t block_size : block_size_array) {
    uint64_t block_count = total_size / block_size;
    std::unique_ptr<Bcache> bc;
    MkfsOptions mkfs_options;
    auto device = std::make_unique<FakeBlockDevice>(FakeBlockDevice::Config{
        .block_count = block_count, .block_size = block_size, .supports_trim = true});
    bool readonly_device = false;

    if (block_size > (1 << kMaxLogSectorSize)) {
      ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_ERR_BAD_STATE);
    } else if (block_size < (1 << kMinLogSectorSize)) {
      ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

      MkfsWorker mkfs(std::move(bc), mkfs_options);
      auto ret = mkfs.DoMkfs();
      ASSERT_EQ(ret.is_error(), true);
      ASSERT_EQ(ret.error_value(), ZX_ERR_INVALID_ARGS);
      bc = mkfs.Destroy();
      bc.reset();
    } else {
      ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

      MkfsWorker mkfs(std::move(bc), mkfs_options);
      auto ret = mkfs.DoMkfs();
      ASSERT_EQ(ret.is_error(), false);
      bc = std::move(*ret);

      std::unique_ptr<F2fs> fs;
      MountOptions options{};
      async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
      FileTester::MountWithOptions(loop.dispatcher(), options, &bc, &fs);

      fbl::RefPtr<VnodeF2fs> root;
      FileTester::CreateRoot(fs.get(), &root);
      fbl::RefPtr<Dir> root_dir = fbl::RefPtr<Dir>::Downcast(std::move(root));

      Superblock &fsb = fs->RawSb();
      ASSERT_EQ(1 << fsb.log_sectorsize, static_cast<const int32_t>(block_size));
      ASSERT_EQ(1 << fs->GetSuperblockInfo().GetLogSectorsPerBlock(),
                static_cast<const int32_t>((1 << kMaxLogSectorSize) / block_size));

      ASSERT_EQ(root_dir->Close(), ZX_OK);
      root_dir.reset();

      FileTester::Unmount(std::move(fs), &bc);
      EXPECT_EQ(Fsck(std::move(bc), FsckOptions{.repair = false}, &bc), ZX_OK);
    }
  }
}

TEST(FormatFilesystemTest, MkfsSmallVolume) {
  uint32_t volume_size_array[] = {30, 40, 50, 60, 70, 80, 90, 100};
  uint32_t block_size = 4096;

  for (uint32_t volume_size : volume_size_array) {
    uint64_t block_count = volume_size * 1024 * 1024 / block_size;

    std::unique_ptr<Bcache> bc;
    auto device = std::make_unique<FakeBlockDevice>(FakeBlockDevice::Config{
        .block_count = block_count, .block_size = block_size, .supports_trim = true});
    bool readonly_device = false;
    ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

    MkfsOptions mkfs_options;
    MkfsWorker mkfs(std::move(bc), mkfs_options);
    auto ret = mkfs.DoMkfs();
    if (volume_size >= 40) {
      ASSERT_TRUE(ret.is_ok());
      bc = std::move(*ret);

      std::unique_ptr<F2fs> fs;
      MountOptions options{};
      async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
      FileTester::MountWithOptions(loop.dispatcher(), options, &bc, &fs);

      Superblock &fsb = fs->RawSb();
      ASSERT_EQ(fsb.segment_count_main, static_cast<uint32_t>(volume_size / 2 - 8));

      FileTester::Unmount(std::move(fs), &bc);
      EXPECT_EQ(Fsck(std::move(bc), FsckOptions{.repair = false}), ZX_OK);
    } else {
      ASSERT_TRUE(ret.is_error());
      ASSERT_EQ(ret.status_value(), ZX_ERR_NO_SPACE);
      bc = mkfs.Destroy();
    }
  }
}

TEST(FormatFilesystemTest, MkfsPrintOptions) {
  auto device = std::make_unique<FakeBlockDevice>(kMkfsBlockCount, kMkfsBlockSize);
  std::unique_ptr<Bcache> bc;
  bool readonly_device = false;
  ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

  // Check default value
  std::vector<const char *> argv = {"mkfs"};
  DoMkfs(std::move(bc), argv, true, &bc);

  // Print CurrentOption
  MkfsOptions mkfs_options;
  MkfsWorker mkfs(std::move(bc), mkfs_options);
  bc = *mkfs.DoMkfs();
  mkfs.PrintCurrentOption();

  // Print uasage
  argv.clear();
  argv.push_back("mkfs");
  argv.push_back("-h");
  DoMkfs(std::move(bc), argv, false, &bc);
}

TEST(FormatFilesystemTest, PrepareSuperblockExceptionCase) {
  std::unique_ptr<Bcache> bc;
  MkfsOptions mkfs_options;

  auto device = std::make_unique<FakeBlockDevice>(FakeBlockDevice::Config{
      .block_count = kMkfsBlockCount, .block_size = kDefaultSectorSize, .supports_trim = true});
  bool readonly_device = false;

  ASSERT_EQ(CreateBcache(std::move(device), &readonly_device, &bc), ZX_OK);

  MkfsWorker mkfs(std::move(bc), mkfs_options);

  // Check invalid sector_size value
  ASSERT_EQ(MkfsTester::InitAndGetDeviceInfo(mkfs), ZX_OK);
  GlobalParameters &params = MkfsTester::GetGlobalParameters(mkfs);
  params.sector_size = kMinLogSectorSize / 2;
  auto ret = MkfsTester::FormatDevice(mkfs);
  ASSERT_TRUE(ret.is_error());

  // Check invalid sectors_per_blk value
  ASSERT_EQ(MkfsTester::InitAndGetDeviceInfo(mkfs), ZX_OK);
  params = MkfsTester::GetGlobalParameters(mkfs);
  params.sectors_per_blk = kDefaultSectorsPerBlock * 2;
  ret = MkfsTester::FormatDevice(mkfs);
  ASSERT_EQ(ret.is_error(), true);

  // Check invalid blks_per_seg value
  ASSERT_EQ(MkfsTester::InitAndGetDeviceInfo(mkfs), ZX_OK);
  params = MkfsTester::GetGlobalParameters(mkfs);
  params.blks_per_seg = kDefaultBlocksPerSegment * 2;
  ret = MkfsTester::FormatDevice(mkfs);
  ASSERT_EQ(ret.is_error(), true);

  // Check unaligned start_sector
  ASSERT_EQ(MkfsTester::InitAndGetDeviceInfo(mkfs), ZX_OK);
  params = MkfsTester::GetGlobalParameters(mkfs);
  params.start_sector = 1;
  ret = MkfsTester::FormatDevice(mkfs);
  ASSERT_EQ(ret.is_error(), false);
}

}  // namespace
}  // namespace f2fs
