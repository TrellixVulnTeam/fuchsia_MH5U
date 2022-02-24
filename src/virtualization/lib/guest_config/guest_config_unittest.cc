// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/lib/guest_config/guest_config.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

class GuestConfigParserTest : public ::testing::Test {
 protected:
  std::vector<std::string> paths_;
  fuchsia::virtualization::GuestConfig config_;

  zx_status_t ParseConfig(const std::string& config_str) {
    auto open_at = [this](const std::string& path, auto) {
      paths_.emplace_back(path);
      return ZX_OK;
    };
    auto st = guest_config::ParseConfig(config_str, std::move(open_at), &config_);
    if (st == ZX_OK) {
      guest_config::SetDefaults(&config_);
    }
    return st;
  }

  zx_status_t ParseArgs(std::vector<const char*> args) {
    args.insert(args.begin(), "exe_name");
    return guest_config::ParseArguments(static_cast<int>(args.size()), args.data(), &config_);
  }
};

TEST_F(GuestConfigParserTest, DefaultValues) {
  ASSERT_EQ(ZX_OK, ParseConfig("{}"));
  ASSERT_FALSE(config_.has_kernel_type());
  ASSERT_FALSE(config_.has_kernel());
  ASSERT_FALSE(config_.has_ramdisk());
  ASSERT_EQ(zx_system_get_num_cpus(), config_.cpus());
  ASSERT_FALSE(config_.has_block_devices());
  ASSERT_FALSE(config_.has_cmdline());
}

TEST_F(GuestConfigParserTest, ParseConfig) {
  ASSERT_EQ(ZX_OK, ParseConfig(
                       R"JSON({
          "zircon": "zircon_path",
          "ramdisk": "ramdisk_path",
          "cpus": "4",
          "block": "/pkg/data/block_path",
          "cmdline": "kernel cmdline"
        })JSON"));
  ASSERT_EQ(fuchsia::virtualization::KernelType::ZIRCON, config_.kernel_type());
  ASSERT_TRUE(config_.kernel());
  ASSERT_TRUE(config_.ramdisk());
  ASSERT_EQ(4u, config_.cpus());
  ASSERT_EQ(1ul, config_.block_devices().size());
  ASSERT_EQ("/pkg/data/block_path", config_.block_devices().front().id);
  ASSERT_EQ("kernel cmdline", config_.cmdline());
}

TEST_F(GuestConfigParserTest, ParseDisallowedArgs) {
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, ParseArgs({"--linux=linux_path"}));
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, ParseArgs({"--ramdisk=ramdisk_path"}));
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, ParseArgs({"--block=/pkg/data/block_path"}));
}

TEST_F(GuestConfigParserTest, ParseArgs) {
  ASSERT_EQ(ZX_OK, ParseArgs({"--cpus=8"}));
  ASSERT_EQ(8, config_.cpus());
}

TEST_F(GuestConfigParserTest, InvalidCpusArgs) {
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, ParseArgs({"--cpus=invalid"}));
}

TEST_F(GuestConfigParserTest, UnknownArgument) {
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, ParseArgs({"--invalid-arg"}));
}

TEST_F(GuestConfigParserTest, BooleanFlag) {
  ASSERT_EQ(ZX_OK, ParseArgs({"--virtio-balloon=false"}));
  ASSERT_FALSE(config_.virtio_balloon());

  config_.clear_virtio_balloon();
  ASSERT_EQ(ZX_OK, ParseArgs({"--virtio-balloon=true"}));
  ASSERT_TRUE(config_.virtio_balloon());
}

TEST_F(GuestConfigParserTest, CommandLineAppend) {
  ASSERT_EQ(ZX_OK, ParseArgs({"--cmdline-add=foo", "--cmdline-add=bar"}));
  EXPECT_THAT(config_.cmdline_add(), testing::ElementsAre("foo", "bar"));
}

TEST_F(GuestConfigParserTest, BlockSpecJson) {
  ASSERT_EQ(ZX_OK, ParseConfig(
                       R"JSON({
          "block": [
            "/pkg/data/foo,ro,file",
            "/dev/class/block/001,rw,file"
          ]
        })JSON"));
  ASSERT_EQ(2ul, config_.block_devices().size());

  const fuchsia::virtualization::BlockSpec& spec0 = config_.block_devices()[0];
  ASSERT_EQ("/pkg/data/foo", spec0.id);
  ASSERT_EQ(fuchsia::virtualization::BlockMode::READ_ONLY, spec0.mode);
  ASSERT_EQ(fuchsia::virtualization::BlockFormat::FILE, spec0.format);

  const fuchsia::virtualization::BlockSpec& spec1 = config_.block_devices()[1];
  ASSERT_EQ("/dev/class/block/001", spec1.id);
  ASSERT_EQ(fuchsia::virtualization::BlockMode::READ_WRITE, spec1.mode);
  ASSERT_EQ(fuchsia::virtualization::BlockFormat::FILE, spec1.format);

  EXPECT_THAT(paths_, testing::ElementsAre("/pkg/data/foo", "/dev/class/block/001"));
}

TEST_F(GuestConfigParserTest, InterruptSpecArg) {
  ASSERT_EQ(ZX_OK, ParseArgs({"--interrupt=32", "--interrupt=33"}));
  ASSERT_EQ(2ul, config_.interrupts().size());

  ASSERT_EQ(32u, config_.interrupts()[0]);
  ASSERT_EQ(33u, config_.interrupts()[1]);
}

TEST_F(GuestConfigParserTest, InterruptSpecJson) {
  ASSERT_EQ(ZX_OK, ParseConfig(
                       R"JSON({
          "interrupt": [
            "32",
            "33"
          ]
        })JSON"));
  ASSERT_EQ(2ul, config_.interrupts().size());

  const uint32_t& spec0 = config_.interrupts()[0];
  ASSERT_EQ(32u, spec0);

  const uint32_t& spec1 = config_.interrupts()[1];
  ASSERT_EQ(33u, spec1);
}

TEST_F(GuestConfigParserTest, Memory_1024k) {
  ASSERT_EQ(ZX_OK, ParseArgs({"--memory=1024k"}));
  const auto& memory = config_.memory();
  EXPECT_EQ(1ul, memory.size());
  EXPECT_EQ(0ul, memory[0].base);
  EXPECT_EQ(1ul << 20, memory[0].size);
  EXPECT_EQ(fuchsia::virtualization::MemoryPolicy::GUEST_CACHED, memory[0].policy);
}

TEST_F(GuestConfigParserTest, Memory_2M) {
  ASSERT_EQ(ZX_OK, ParseArgs({"--memory=2M"}));
  const auto& memory = config_.memory();
  EXPECT_EQ(1ul, memory.size());
  EXPECT_EQ(0ul, memory[0].base);
  EXPECT_EQ(2ul << 20, memory[0].size);
  EXPECT_EQ(fuchsia::virtualization::MemoryPolicy::GUEST_CACHED, memory[0].policy);
}

TEST_F(GuestConfigParserTest, Memory_4G) {
  ASSERT_EQ(ZX_OK, ParseArgs({"--memory=4G"}));
  const auto& memory = config_.memory();
  EXPECT_EQ(1ul, memory.size());
  EXPECT_EQ(0ul, memory[0].base);
  EXPECT_EQ(4ul << 30, memory[0].size);
  EXPECT_EQ(fuchsia::virtualization::MemoryPolicy::GUEST_CACHED, memory[0].policy);
}

TEST_F(GuestConfigParserTest, Memory_AddressAndSize) {
  ASSERT_EQ(ZX_OK, ParseArgs({"--memory=ffff,4G"}));
  const auto& memory = config_.memory();
  EXPECT_EQ(1ul, memory.size());
  EXPECT_EQ(0xfffful, memory[0].base);
  EXPECT_EQ(4ul << 30, memory[0].size);
  EXPECT_EQ(fuchsia::virtualization::MemoryPolicy::GUEST_CACHED, memory[0].policy);
}

TEST_F(GuestConfigParserTest, Memory_HostCached) {
  ASSERT_EQ(ZX_OK, ParseArgs({"--memory=eeee,2G,cached"}));
  const auto& memory = config_.memory();
  EXPECT_EQ(1ul, memory.size());
  EXPECT_EQ(0xeeeeul, memory[0].base);
  EXPECT_EQ(2ul << 30, memory[0].size);
  EXPECT_EQ(fuchsia::virtualization::MemoryPolicy::HOST_CACHED, memory[0].policy);
}

TEST_F(GuestConfigParserTest, Memory_HostDevice) {
  ASSERT_EQ(ZX_OK, ParseArgs({"--memory=dddd,1G,device"}));
  const auto& memory = config_.memory();
  EXPECT_EQ(1ul, memory.size());
  EXPECT_EQ(0xddddul, memory[0].base);
  EXPECT_EQ(1ul << 30, memory[0].size);
  EXPECT_EQ(fuchsia::virtualization::MemoryPolicy::HOST_DEVICE, memory[0].policy);
}

TEST_F(GuestConfigParserTest, Memory_MultipleEntries) {
  ASSERT_EQ(ZX_OK, ParseArgs({"--memory=f0000000,1M", "--memory=ffffffff,2M"}));
  const auto& memory = config_.memory();
  EXPECT_EQ(2ul, memory.size());
  EXPECT_EQ(0xf0000000ul, memory[0].base);
  EXPECT_EQ(1ul << 20, memory[0].size);
  EXPECT_EQ(fuchsia::virtualization::MemoryPolicy::GUEST_CACHED, memory[0].policy);
  EXPECT_EQ(0xfffffffful, memory[1].base);
  EXPECT_EQ(2ul << 20, memory[1].size);
  EXPECT_EQ(fuchsia::virtualization::MemoryPolicy::GUEST_CACHED, memory[1].policy);
}

TEST_F(GuestConfigParserTest, Memory_IllegalModifier) {
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, ParseArgs({"--memory=5l"}));
}

TEST_F(GuestConfigParserTest, Memory_NonNumber) {
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, ParseArgs({"--memory=abc"}));
}

TEST_F(GuestConfigParserTest, VirtioGpu) {
  fuchsia::virtualization::GuestConfig config;

  ASSERT_EQ(ZX_OK, ParseArgs({"--virtio-gpu=true"}));
  ASSERT_TRUE(config_.virtio_gpu());

  config_.clear_virtio_gpu();
  ASSERT_EQ(ZX_OK, ParseArgs({"--virtio-gpu=false"}));
  ASSERT_FALSE(config_.virtio_gpu());
}
