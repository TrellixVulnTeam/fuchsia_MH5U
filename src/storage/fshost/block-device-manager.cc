// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/fshost/block-device-manager.h"

#include <fidl/fuchsia.device/cpp/wire.h>
#include <inttypes.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/device/block.h>
#include <zircon/hw/gpt.h>

#include <set>

#include "src/lib/storage/fs_management/cpp/format.h"
#include "src/storage/fshost/constants.h"

namespace fshost {
namespace {

// Setting for the maximum bytes to allow a partition to grow to.
struct PartitionLimit {
  // When unset, this limit will apply only to non-ramdisk devices. See
  // Config::kApplyLimitsToRamdisk.
  bool apply_to_ramdisk = false;

  // Partition max size in bytes, 0 means "no limit".
  uint64_t max_bytes = 0;
};

// Splits the path into a directory and the last component.
std::pair<std::string_view, std::string_view> SplitPath(std::string_view path) {
  size_t separator = path.rfind('/');
  if (separator != std::string::npos) {
    return std::make_pair(path.substr(0, separator), path.substr(separator + 1));
  } else {
    return std::make_pair(std::string_view(), path);
  }
}

bool IsRamdisk(const BlockDeviceInterface& device) {
  constexpr std::string_view kRamdiskPrefix = "/dev/sys/platform/00:00:2d/ramctl/";
  return device.topological_path().compare(0, kRamdiskPrefix.length(), kRamdiskPrefix) == 0;
}

// Matches all NAND devices.
class NandMatcher : public BlockDeviceManager::Matcher {
 public:
  fs_management::DiskFormat Match(const BlockDeviceInterface& device) override {
    if (device.IsNand()) {
      return fs_management::kDiskFormatNandBroker;
    }
    return fs_management::kDiskFormatUnknown;
  }

  zx_status_t Add(BlockDeviceInterface& device) override {
    zx_status_t status = device.Add();
    if (status != ZX_OK) {
      return status;
    }
    if (path_.empty()) {
      path_ = device.topological_path();
    }
    return ZX_OK;
  }

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

// Matches anything that appears to have the given content and keeps track of the first device it
// finds.
class ContentMatcher : public BlockDeviceManager::Matcher {
 public:
  // If |allow_multiple| is true, multiple devices will be matched.  Otherwise, only the first
  // device that appears will match.
  ContentMatcher(fs_management::DiskFormat format, bool allow_multiple)
      : format_(format), allow_multiple_(allow_multiple) {}

  fs_management::DiskFormat Match(const BlockDeviceInterface& device) override {
    if (!allow_multiple_ && !path_.empty()) {
      // Only match the first occurrence.
      return fs_management::kDiskFormatUnknown;
    }
    if (device.content_format() == format_) {
      return format_;
    } else {
      return fs_management::kDiskFormatUnknown;
    }
  }

  zx_status_t Add(BlockDeviceInterface& device) override {
    zx_status_t status = device.Add();
    if (status != ZX_OK) {
      return status;
    }
    if (path_.empty()) {
      path_ = device.topological_path();
    }
    return ZX_OK;
  }

  const std::string& path() const { return path_; }

 private:
  const fs_management::DiskFormat format_;
  const bool allow_multiple_;
  std::string path_;
};

// Matches devices that handle groups of partitions.
class PartitionMapMatcher : public ContentMatcher {
 public:
  // |suffix| is a device that is expected to appear when the driver is bound. For example, FVM,
  // will add a "/fvm" device before adding children whilst GPT won't add anything.  If
  // |ramdisk_required| is set, this matcher will only match against a ram-disk.
  PartitionMapMatcher(fs_management::DiskFormat format, bool allow_multiple,
                      std::string_view suffix, bool ramdisk_required)
      : ContentMatcher(format, allow_multiple),
        suffix_(suffix),
        ramdisk_required_(ramdisk_required) {}

  bool ramdisk_required() const { return ramdisk_required_; }

  fs_management::DiskFormat Match(const BlockDeviceInterface& device) override {
    if (ramdisk_required_ && !IsRamdisk(device)) {
      return fs_management::kDiskFormatUnknown;
    }
    return ContentMatcher::Match(device);
  }

  // Returns true if |device| is a child of the device matched by this matcher.
  bool IsChild(const BlockDeviceInterface& device) const {
    if (path().empty()) {
      return false;
    }
    // Child partitions should have topological paths of the form:
    //   .../<suffix>/<partition-name>/block
    auto [dir1, base1] = SplitPath(device.topological_path());
    if (base1 != "block") {
      return false;
    }
    auto [dir2, base2] = SplitPath(dir1);
    // base should be something like <partition-name>-p-1, but we ignore that.
    return path() + suffix_ == dir2;
  }

 private:
  const std::string suffix_;
  const bool ramdisk_required_;
};

// Extracts the path that the FVM driver responds to FIDL requests at given the PartitionMapMatcher
// for the path.
std::string GetFvmPathForPartitionMap(const PartitionMapMatcher& matcher) {
  return matcher.path() + "/fvm";
}

// Matches a partition with a given name and expected type GUID.
class SimpleMatcher : public BlockDeviceManager::Matcher {
 public:
  SimpleMatcher(PartitionMapMatcher& map, std::string partition_name,
                const fuchsia_hardware_block_partition::wire::Guid& type_guid,
                fs_management::DiskFormat format, PartitionLimit limit)
      : map_(map),
        partition_name_(partition_name),
        type_guid_(type_guid),
        format_(format),
        limit_(limit) {}

  fs_management::DiskFormat Match(const BlockDeviceInterface& device) override {
    if (map_.IsChild(device) && device.partition_name() == partition_name_ &&
        !memcmp(&device.GetTypeGuid(), &type_guid_, sizeof(type_guid_))) {
      return format_;
    } else {
      return fs_management::kDiskFormatUnknown;
    }
  }

  zx_status_t Add(BlockDeviceInterface& device) override {
    if (limit_.max_bytes) {
      if (limit_.apply_to_ramdisk || !IsRamdisk(device)) {
        // Set the max size for this partition in FVM. Ignore failures since the max size is
        // mostly a guard rail against bad behavior and we can still function.
        [[maybe_unused]] auto status =
            device.SetPartitionMaxSize(GetFvmPathForPartitionMap(map_), limit_.max_bytes);
      }
    }
    return device.Add();
  }

 private:
  const PartitionMapMatcher& map_;
  const std::string partition_name_;
  const fuchsia_hardware_block_partition::wire::Guid type_guid_;
  const fs_management::DiskFormat format_;
  const PartitionLimit limit_;
};

// Matches a data partition, which is a Minfs partition backed by zxcrypt.
class MinfsMatcher : public BlockDeviceManager::Matcher {
 public:
  using PartitionNames = std::set<std::string, std::less<>>;
  enum class ZxcryptVariant {
    // A regular minfs partition backed by zxcrypt.
    kNormal,
    // A minfs partition not backed by zxcrypt.
    kNoZxcrypt,
    // Only attach and unseal the zxcrypt partition; doesn't mount minfs.
    kZxcryptOnly
  };

  struct Variant {
    ZxcryptVariant zxcrypt = ZxcryptVariant::kNormal;
    bool format_minfs_on_corruption = true;
  };

  static constexpr std::string_view kZxcryptSuffix = "/zxcrypt/unsealed/block";

  MinfsMatcher(const PartitionMapMatcher& map, PartitionNames partition_names,
               std::string_view preferred_name,
               const fuchsia_hardware_block_partition::wire::Guid& type_guid, Variant variant,
               PartitionLimit limit)
      : map_(map),
        partition_names_(std::move(partition_names)),
        preferred_name_(preferred_name),
        type_guid_(type_guid),
        variant_(variant),
        limit_(limit) {}

  static Variant GetVariantFromConfig(const Config& config) {
    Variant variant;
    if (config.is_set(Config::kNoZxcrypt)) {
      variant.zxcrypt = ZxcryptVariant::kNoZxcrypt;
    } else {
      variant.zxcrypt = ZxcryptVariant::kNormal;
    }

    variant.format_minfs_on_corruption = config.is_set(Config::kFormatMinfsOnCorruption);
    return variant;
  }

  fs_management::DiskFormat Match(const BlockDeviceInterface& device) override {
    if (expected_inner_path_.empty()) {
      if (map_.IsChild(device) &&
          partition_names_.find(device.partition_name()) != partition_names_.end() &&
          !memcmp(&device.GetTypeGuid(), &type_guid_, sizeof(type_guid_))) {
        switch (variant_.zxcrypt) {
          case ZxcryptVariant::kNormal:
            return map_.ramdisk_required() ? fs_management::kDiskFormatMinfs
                                           : fs_management::kDiskFormatZxcrypt;
          case ZxcryptVariant::kNoZxcrypt:
            return fs_management::kDiskFormatMinfs;
          case ZxcryptVariant::kZxcryptOnly:
            return fs_management::kDiskFormatZxcrypt;
        }
      }
    } else if (variant_.zxcrypt == ZxcryptVariant::kNormal &&
               device.topological_path() == expected_inner_path_ &&
               !memcmp(&device.GetTypeGuid(), &type_guid_, sizeof(type_guid_))) {
      return fs_management::kDiskFormatMinfs;
    }
    return fs_management::kDiskFormatUnknown;
  }

  zx_status_t Add(BlockDeviceInterface& device) override {
    if (limit_.max_bytes) {
      if (limit_.apply_to_ramdisk || !IsRamdisk(device)) {
        // Set the max size for this partition in FVM. This is not persisted so we need to set it
        // every time on mount. Ignore failures since the max size is mostly a guard rail against
        // bad behavior and we can still function.
        [[maybe_unused]] auto status =
            device.SetPartitionMaxSize(GetFvmPathForPartitionMap(map_), limit_.max_bytes);
      }
    }

    if (expected_inner_path_.empty() && !preferred_name_.empty() &&
        device.partition_name() != preferred_name_) {
      if (zx_status_t status =
              device.SetPartitionName(GetFvmPathForPartitionMap(map_), preferred_name_);
          status != ZX_OK) {
        FX_LOGS(ERROR) << "Failed to change data partition name to '" << preferred_name_
                       << "': " << zx_status_get_string(status);
        // Continue since not fatal...
      } else {
        FX_LOGS(INFO) << "Changed data partition name to '" << preferred_name_ << "'";
      }
    }

    // If the volume doesn't appear to be zxcrypt, assume that it's because it was never formatted
    // as such, or the keys have been shredded, so skip straight to reformatting.  Strictly
    // speaking, it's not necessary, because attempting to unseal should trigger the same
    // behaviour, but the log messages in that case are scary.
    if (device.GetFormat() == fs_management::kDiskFormatZxcrypt) {
      if (device.content_format() != fs_management::kDiskFormatZxcrypt) {
        FX_LOGS(INFO) << "Formatting as zxcrypt partition";
        zx_status_t status = device.FormatZxcrypt();
        if (status != ZX_OK) {
          return status;
        }
        // Set the reformat_ flag so that when the Minfs device appears we can skip straight to
        // reformatting it (and skip any fsck).  Again, this isn't strictly required because
        // mounting should fail and we'll reformat, but we can skip that when we know we need to
        // reformat.
        reformat_ = true;
      }
    } else if (reformat_) {
      // We formatted zxcrypt, so skip straight to formatting minfs.
      zx_status_t status = device.FormatFilesystem();
      if (status != ZX_OK) {
        return status;
      }
      reformat_ = false;
    }
    zx_status_t status = device.Add(variant_.format_minfs_on_corruption);
    if (status != ZX_OK) {
      return status;
    }
    if (device.GetFormat() == fs_management::kDiskFormatZxcrypt) {
      expected_inner_path_ = device.topological_path();
      expected_inner_path_.append(kZxcryptSuffix);
    }
    return ZX_OK;
  }

 private:
  const PartitionMapMatcher& map_;
  const PartitionNames partition_names_;
  const std::string preferred_name_;
  const fuchsia_hardware_block_partition::wire::Guid type_guid_;
  const Variant variant_;
  const PartitionLimit limit_;

  std::string expected_inner_path_;
  // If we reformat the zxcrypt device, this flag is set so that we know we should reformat the
  // minfs device when it appears.
  bool reformat_ = false;
};

// Matches the factory partition.
class FactoryfsMatcher : public BlockDeviceManager::Matcher {
 public:
  static constexpr std::string_view kVerityMutableSuffix = "/verity/mutable/block";
  static constexpr std::string_view kVerityVerifiedSuffix = "/verity/verified/block";

  FactoryfsMatcher(const PartitionMapMatcher& map) : map_(map) {}

  fs_management::DiskFormat Match(const BlockDeviceInterface& device) override {
    static constexpr fuchsia_hardware_block_partition::wire::Guid factory_type_guid =
        GPT_FACTORY_TYPE_GUID;
    if (base_path_.empty()) {
      if (map_.IsChild(device) &&
          !memcmp(&device.GetTypeGuid(), &factory_type_guid, sizeof(factory_type_guid)) &&
          device.partition_name() == "factory") {
        return fs_management::kDiskFormatBlockVerity;
      }
    } else if (!memcmp(&device.GetTypeGuid(), &factory_type_guid, sizeof(factory_type_guid)) &&
               (device.topological_path() == std::string(base_path_).append(kVerityMutableSuffix) ||
                device.topological_path() ==
                    std::string(base_path_).append(kVerityVerifiedSuffix))) {
      return fs_management::kDiskFormatFactoryfs;
    }
    return fs_management::kDiskFormatUnknown;
  }

  zx_status_t Add(BlockDeviceInterface& device) override {
    zx_status_t status = device.Add();
    if (status != ZX_OK) {
      return status;
    }
    base_path_ = device.topological_path();
    return ZX_OK;
  }

 private:
  const PartitionMapMatcher& map_;
  std::string base_path_;
};

// Matches devices that report flags with BLOCK_FLAG_BOOTPART set.
class BootpartMatcher : public BlockDeviceManager::Matcher {
 public:
  fs_management::DiskFormat Match(const BlockDeviceInterface& device) override {
    fuchsia_hardware_block_BlockInfo info;
    zx_status_t status = device.GetInfo(&info);
    if (status != ZX_OK) {
      return fs_management::kDiskFormatUnknown;
    }
    return info.flags & BLOCK_FLAG_BOOTPART ? fs_management::kDiskFormatBootpart
                                            : fs_management::kDiskFormatUnknown;
  }
};

MinfsMatcher::PartitionNames GetMinfsPartitionNames(bool include_legacy) {
  if (include_legacy) {
    return {std::string(kDataPartitionLabel), "minfs", "fuchsia-data"};
  } else {
    return {std::string(kDataPartitionLabel)};
  }
}

}  // namespace

BlockDeviceManager::BlockDeviceManager(const Config* config) : config_(*config) {
  static constexpr fuchsia_hardware_block_partition::wire::Guid minfs_type_guid = GUID_DATA_VALUE;

  if (config_.is_set(Config::kBootpart)) {
    matchers_.push_back(std::make_unique<BootpartMatcher>());
  }
  if (config_.is_set(Config::kNand)) {
    matchers_.push_back(std::make_unique<NandMatcher>());
  }

  auto gpt = std::make_unique<PartitionMapMatcher>(fs_management::kDiskFormatGpt,
                                                   config_.is_set(Config::kGptAll), "",
                                                   /*ramdisk_required=*/false);
  auto fvm =
      std::make_unique<PartitionMapMatcher>(fs_management::kDiskFormatFvm, /*allow_multiple=*/false,
                                            "/fvm", config_.is_set(Config::kFvmRamdisk));

  bool gpt_required = config_.is_set(Config::kGpt) || config_.is_set(Config::kGptAll);
  bool fvm_required = config_.is_set(Config::kFvm);

  // Maximum partition limits. The limits only apply to physical devices (not ramdisks) unless
  // apply_limits_to_ramdisk is set.
  PartitionLimit blobfs_limit{
      .apply_to_ramdisk = config_.is_set(Config::kApplyLimitsToRamdisk),
      .max_bytes = config_.ReadUint64OptionValue(Config::kBlobfsMaxBytes, 0)};
  PartitionLimit minfs_limit{.apply_to_ramdisk = config_.is_set(Config::kApplyLimitsToRamdisk),
                             .max_bytes = config_.ReadUint64OptionValue(Config::kMinfsMaxBytes, 0)};

  if (!config_.is_set(Config::kNetboot)) {
    // GPT partitions:
    if (config_.is_set(Config::kDurable)) {
      static constexpr fuchsia_hardware_block_partition::wire::Guid durable_type_guid =
          GPT_DURABLE_TYPE_GUID;
      matchers_.push_back(std::make_unique<MinfsMatcher>(
          *gpt, MinfsMatcher::PartitionNames{GPT_DURABLE_NAME}, std::string_view(),
          durable_type_guid, MinfsMatcher::GetVariantFromConfig(config_), PartitionLimit()));
      gpt_required = true;
    }
    if (config_.is_set(Config::kFactory)) {
      matchers_.push_back(std::make_unique<FactoryfsMatcher>(*gpt));
      gpt_required = true;
    }

    // FVM partitions:
    if (config_.is_set(Config::kBlobfs)) {
      static constexpr fuchsia_hardware_block_partition::wire::Guid blobfs_type_guid =
          GUID_BLOB_VALUE;
      matchers_.push_back(std::make_unique<SimpleMatcher>(
          *fvm, std::string(kBlobfsPartitionLabel), blobfs_type_guid,
          fs_management::kDiskFormatBlobfs, blobfs_limit));
      fvm_required = true;
    }
    if (config_.is_set(Config::kMinfs)) {
      matchers_.push_back(std::make_unique<MinfsMatcher>(
          *fvm, GetMinfsPartitionNames(config_.is_set(Config::kAllowLegacyDataPartitionNames)),
          kDataPartitionLabel, minfs_type_guid, MinfsMatcher::GetVariantFromConfig(config_),
          minfs_limit));
      fvm_required = true;
    }
  }

  // The partition map matchers go last because they match on content.
  if (fvm_required) {
    std::unique_ptr<PartitionMapMatcher> non_ramdisk_fvm;
    if (config_.is_set(Config::kFvmRamdisk)) {
      // Add another matcher for the non-ramdisk version of FVM.
      non_ramdisk_fvm = std::make_unique<PartitionMapMatcher>(fs_management::kDiskFormatFvm,
                                                              /*allow_multiple=*/false, "/fvm",
                                                              /*ramdisk_required=*/false);

      if (config_.is_set(Config::kAttachZxcryptToNonRamdisk)) {
        matchers_.push_back(std::make_unique<MinfsMatcher>(
            *non_ramdisk_fvm,
            GetMinfsPartitionNames(config_.is_set(Config::kAllowLegacyDataPartitionNames)),
            kDataPartitionLabel, minfs_type_guid,
            MinfsMatcher::Variant{.zxcrypt = MinfsMatcher::ZxcryptVariant::kZxcryptOnly},
            minfs_limit));
      }
    }
    matchers_.push_back(std::move(fvm));
    if (non_ramdisk_fvm) {
      matchers_.push_back(std::move(non_ramdisk_fvm));
    }
  }
  if (gpt_required) {
    matchers_.push_back(std::move(gpt));
  }
  if (config_.is_set(Config::kMbr)) {
    // Default to allowing multiple devices because mbr support is disabled by default and if it's
    // enabled, it's likely required for removable devices and so supporting multiple devices is
    // probably appropriate.
    matchers_.push_back(std::make_unique<PartitionMapMatcher>(
        fs_management::kDiskFormatMbr, /*allow_multiple=*/true, "", /*ramdisk_required=*/false));
  }
}

zx_status_t BlockDeviceManager::AddDevice(BlockDeviceInterface& device) {
  if (device.topological_path().empty()) {
    return ZX_ERR_NOT_SUPPORTED;
  }

  for (auto& matcher : matchers_) {
    fs_management::DiskFormat format = matcher->Match(device);
    if (format != fs_management::kDiskFormatUnknown) {
      device.SetFormat(format);
      return matcher->Add(device);
    }
  }
  return ZX_ERR_NOT_SUPPORTED;
}

}  // namespace fshost
