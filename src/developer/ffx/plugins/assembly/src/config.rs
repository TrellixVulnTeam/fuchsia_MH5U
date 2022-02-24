// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::util;
use anyhow::{anyhow, Result};
use assembly_fvm::FilesystemAttributes;
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};
use std::path::PathBuf;

pub mod product_config;

/// The set of information that defines a fuchsia product.  All fields are
/// optional to allow for specifying incomplete configurations.
#[derive(Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PartialProductConfig {
    /// The packages whose files get added to the base package. The
    /// packages themselves are not added, but their individual files are
    /// extracted and added to the base package. These files are needed
    /// to bootstrap pkgfs.
    #[serde(default)]
    pub system: Vec<PathBuf>,

    /// The packages that are in the base package list, which is added
    /// to the base package (data/static_packages). These packages get
    /// updated by flashing and OTAing, and cannot be garbage collected.
    #[serde(default)]
    pub base: Vec<PathBuf>,

    /// The packages that are in the cache package list, which is added
    /// to the base package (data/cache_packages). These packages get
    /// updated by flashing and OTAing, but can be garbage collected.
    #[serde(default)]
    pub cache: Vec<PathBuf>,

    /// The parameters that specify which kernel to put into the ZBI.
    pub kernel: Option<PartialKernelConfig>,

    /// The list of additional boot args to add.
    #[serde(default)]
    pub boot_args: Vec<String>,

    /// The set of files to be placed in BOOTFS in the ZBI.
    #[serde(default)]
    pub bootfs_files: Vec<FileEntry>,
}

impl PartialProductConfig {
    /// Create a new PartialProductConfig by merging N PartialProductConfigs.
    /// At most one can specify a kernel path, or a clock backstop.
    ///
    /// Packages in the base and cache sets are deduplicated, as are any boot
    /// arguments, kernel args, or packages used to provide files for the system
    /// itself.
    ///
    /// bootfs entries are merged, and any entries with duplicate destination
    /// paths will cause an error.
    pub fn try_from_partials<I: IntoIterator<Item = PartialProductConfig>>(
        configs: I,
    ) -> Result<Self> {
        let mut system = BTreeSet::new();
        let mut base = BTreeSet::new();
        let mut cache = BTreeSet::new();
        let mut boot_args = BTreeSet::new();
        let mut bootfs_files = BTreeMap::new();

        let mut kernel_path = None;
        let mut kernel_args = Vec::new();
        let mut kernel_clock_backstop = None;

        for config in configs.into_iter() {
            system.extend(config.system);
            base.extend(config.base);
            cache.extend(config.cache);
            boot_args.extend(config.boot_args);

            for entry in config.bootfs_files {
                add_bootfs_file(&mut bootfs_files, entry)?;
            }

            if let Some(PartialKernelConfig { path, mut args, clock_backstop }) = config.kernel {
                util::set_option_once_or(
                    &mut kernel_path,
                    path,
                    anyhow!("Only one product configuration can specify a kernel path"),
                )?;
                kernel_args.append(&mut args);

                util::set_option_once_or(
                    &mut kernel_clock_backstop,
                    clock_backstop,
                    anyhow!("Only one product configuration can specify a backstop time"),
                )?;
            }
        }

        Ok(Self {
            system: system.into_iter().collect(),
            base: base.into_iter().collect(),
            cache: cache.into_iter().collect(),
            kernel: Some(PartialKernelConfig {
                path: kernel_path,
                args: kernel_args,
                clock_backstop: kernel_clock_backstop,
            }),
            boot_args: boot_args.into_iter().collect(),
            bootfs_files: bootfs_files.into_values().collect(),
        })
    }
}

/// The set of information that defines a fuchsia product.  This is capable of
/// being a complete configuration (it at least has a kernel).
#[derive(Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ProductConfig {
    /// The packages whose files get added to the base package. The
    /// packages themselves are not added, but their individual files are
    /// extracted and added to the base package. These files are needed
    /// to bootstrap pkgfs.
    #[serde(default)]
    pub system: Vec<PathBuf>,

    /// The packages that are in the base package list, which is added
    /// to the base package (data/static_packages). These packages get
    /// updated by flashing and OTAing, and cannot be garbage collected.
    #[serde(default)]
    pub base: Vec<PathBuf>,

    /// The packages that are in the cache package list, which is added
    /// to the base package (data/cache_packages). These packages get
    /// updated by flashing and OTAing, but can be garbage collected.
    #[serde(default)]
    pub cache: Vec<PathBuf>,

    /// The parameters that specify which kernel to put into the ZBI.
    pub kernel: KernelConfig,

    /// The list of additional boot args to add.
    #[serde(default)]
    pub boot_args: Vec<String>,

    /// The set of files to be placed in BOOTFS in the ZBI.
    #[serde(default)]
    pub bootfs_files: Vec<FileEntry>,
}

impl ProductConfig {
    /// Create a new ProductConfig by merging N PartialProductConfigs, only one
    /// of which must specify the kernel path and the clock_backstop.
    ///
    /// Packages in the base and cache sets are deduplicated, as are any boot
    /// arguments, kernel args, or packages used to provide files for the system
    /// itself.
    ///
    /// bootfs entries are merged, and any entries with duplicate destination
    /// paths will cause an error.
    pub fn try_from_partials<I: IntoIterator<Item = PartialProductConfig>>(
        configs: I,
    ) -> Result<Self> {
        let PartialProductConfig { system, base, cache, kernel, boot_args, bootfs_files } =
            PartialProductConfig::try_from_partials(configs.into_iter())?;

        let PartialKernelConfig { path: kernel_path, args: cmdline_args, clock_backstop } =
            kernel.ok_or(anyhow!("A kernel configuration must be specified"))?;

        let kernel_path =
            kernel_path.ok_or(anyhow!("No product configurations specify a kernel"))?;
        let clock_backstop = clock_backstop
            .ok_or(anyhow!("No product configurations specify a clock backstop time"))?;

        Ok(Self {
            system,
            base,
            cache,
            kernel: KernelConfig { path: kernel_path, args: cmdline_args, clock_backstop },
            boot_args,
            bootfs_files,
        })
    }
}

/// Attempt to add the given entry to the map of bootfs entries.
/// Returns an error if it duplicates an existing entry.
fn add_bootfs_file(
    bootfs_entries: &mut BTreeMap<String, FileEntry>,
    entry: FileEntry,
) -> Result<()> {
    if let Some(existing_entry) = bootfs_entries.get(&entry.destination) {
        if existing_entry.source != entry.source {
            return Err(anyhow!(format!(
                "Found a duplicate bootfs entry for destination: {}, with sources:\n{}\n{}",
                entry.destination,
                entry.source.display(),
                existing_entry.source.display()
            )));
        }
    } else {
        bootfs_entries.insert(entry.destination.clone(), entry);
    }
    Ok(())
}

/// The information required to specify a kernel and its arguments, all optional
/// to allow for the partial specification
#[derive(Debug, Deserialize, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PartialKernelConfig {
    /// The path to the prebuilt kernel.
    pub path: Option<PathBuf>,

    /// The list of command line arguments to pass to the kernel on startup.
    #[serde(default)]
    pub args: Vec<String>,

    /// The backstop UTC time for the clock.
    pub clock_backstop: Option<u64>,
}

/// The information required to specify a kernel and its arguments.
#[derive(Debug, Deserialize, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct KernelConfig {
    /// The path to the prebuilt kernel.
    pub path: PathBuf,

    /// The list of command line arguments to pass to the kernel on startup.
    #[serde(default)]
    pub args: Vec<String>,

    /// The backstop UTC time for the clock.
    /// This is kept separate from the `args` to make it clear that this is a required argument.
    pub clock_backstop: u64,
}

/// The set of information that defines a fuchsia board.
#[derive(Debug, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct BoardConfig {
    /// The name of the base package.
    #[serde(default = "default_base_package_name")]
    pub base_package_name: String,

    /// The information required to construct and flash the verified boot
    /// metadata (VBMeta).
    pub vbmeta: Option<VBMetaConfig>,

    /// The information required to construct and flash the ZBI.
    #[serde(default)]
    pub zbi: ZbiConfig,

    /// The information required to construct the BlobFS.
    #[serde(default)]
    pub blobfs: BlobFSConfig,

    /// The information required to construct and flash the FVM.
    pub fvm: Option<FvmConfig>,
}

fn default_base_package_name() -> String {
    "system_image".to_string()
}

/// A mapping between a file source and destination.
#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(deny_unknown_fields)]
pub struct FileEntry {
    /// The path of the source file.
    pub source: PathBuf,

    /// The destination path to put the file.
    pub destination: String,
}

/// The information required to sign a VBMeta image.
#[derive(Debug, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct VBMetaConfig {
    /// The partition name of the kernel included as a VBMeta descriptor.
    pub kernel_partition: String,

    /// The path on the host to the VBMeta key.
    pub key: PathBuf,

    /// The metadata used to verify the VBMeta key.
    pub key_metadata: PathBuf,

    /// Paths to descriptors to add to the VBMeta.
    #[serde(default)]
    pub additional_descriptor_files: Vec<PathBuf>,
}

/// A bootloader to add to the update package and flash-files.
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct BootloaderEntry {
    /// The partition name to flash the bootloader.
    pub partition: Option<String>,

    /// The name of the bootloader entry to add to the update manifest.
    pub name: String,

    /// The type of the bootloader.
    #[serde(rename = "type")]
    pub bootloader_type: String,

    /// The path on the host to the bootloader.
    pub source: PathBuf,
}

/// The information required to construct a ZBI.
#[derive(Debug, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ZbiConfig {
    /// The name to give the output ZBI file.
    /// (e.g. "fuchsia" results in "fuchsia.zbi")
    #[serde(default = "default_zbi_name")]
    pub name: String,

    /// The maximum size of the ZBI in bytes.
    #[serde(default)]
    pub max_size: u64,

    /// Whether the FVM should be added to the ZBI as a RAMDISK.
    #[serde(default = "default_false")]
    pub embed_fvm_in_zbi: bool,

    /// The compression format for the ZBI.
    #[serde(default = "default_zbi_compression")]
    pub compression: String,

    /// An optional "signing script" to sign/repackage the zbi correctly for
    /// use with the device bootloader.
    pub signing_script: Option<ZbiSigningScript>,
}

/// A Default impl which matches how serde should deserialize an all-defaults
/// struct.
impl Default for ZbiConfig {
    fn default() -> Self {
        Self {
            name: default_zbi_name(),
            max_size: Default::default(),
            embed_fvm_in_zbi: default_false(),
            compression: default_zbi_compression(),
            signing_script: Default::default(),
        }
    }
}

/// The information needed to custom-package a ZBI for use on a board with
/// a non-standard (for Fuchsia) bootloader
///
/// The tool specified here _must_ take the following arguments:
///  -z <path to ZBI>
///  -o <output path to write to>
///  -B <build dir, relative to tool's pwd>
#[derive(Debug, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ZbiSigningScript {
    /// The path to the tool to use
    pub tool: PathBuf,

    /// Extra arguments to pass to the tool.  These are passed to the tool after
    /// the above-documented, required, arguments, are passed to the tool.
    #[serde(default)]
    pub extra_arguments: Vec<String>,
}

fn default_false() -> bool {
    false
}

fn default_zbi_name() -> String {
    "fuchsia".to_string()
}

fn default_zbi_compression() -> String {
    "zstd".to_string()
}

/// The information required to construct a BlobFS.
#[derive(Debug, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct BlobFSConfig {
    /// The layout format of the blobs.
    /// Typically "padded" or "compact"
    #[serde(default = "default_blob_layout")]
    pub layout: String,

    #[serde(default = "default_true")]
    pub compress: bool,
}

/// A Default impl which matches how serde should deserialize an all-defaults
/// struct.
impl Default for BlobFSConfig {
    fn default() -> Self {
        Self { layout: default_blob_layout(), compress: default_true() }
    }
}

fn default_blob_layout() -> String {
    "compact".to_string()
}

fn default_true() -> bool {
    true
}

/// The information required to construct a FVM.
#[derive(Debug, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct FvmConfig {
    /// The size in bytes of each slice.
    #[serde(default = "default_fvm_slice_size")]
    pub slice_size: u64,

    /// The number of slices reserved in the FVM for internal usage.
    #[serde(default)]
    pub reserved_slices: u64,

    /// If provided, the build will fail if the sparse FVM exceeds this size.
    #[serde(default)]
    pub max_disk_size: Option<u64>,

    /// If provided, the build will specify this length (that is, image size)
    /// when generating the complete (non-sparse) image. See documentation of
    /// the `--length` parameter of the `fvm` binary host tool for details.
    pub truncate_to_length: Option<u64>,

    /// If provided, a fastboot-supported sparse FVM will be generated.
    #[serde(default)]
    pub fastboot: Option<FastbootConfig>,

    /// The list of filesystems to add to the FVM.
    #[serde(default)]
    pub filesystems: Vec<FvmFilesystemEntry>,
}

fn default_fvm_slice_size() -> u64 {
    8388608
}

/// A Default impl which matches how serde should deserialize an all-defaults
/// struct.
impl Default for FvmConfig {
    fn default() -> Self {
        FvmConfig {
            slice_size: default_fvm_slice_size(),
            reserved_slices: Default::default(),
            max_disk_size: Default::default(),
            truncate_to_length: Default::default(),
            fastboot: Default::default(),
            filesystems: Default::default(),
        }
    }
}

#[derive(Debug, PartialEq, Deserialize, Serialize)]
pub enum FastbootConfig {
    /// Generate an EMMC-supported FVM for flashing.
    Emmc {
        /// The compression algorithm to use.
        #[serde(default = "default_fvm_emmc_compression")]
        compression: String,

        /// The length of the FVM to generate.
        length: u64,
    },

    /// Generate an NAND-supported FVM for flashing.
    Nand {
        /// The compression algorithm to use.
        compression: Option<String>,

        /// The nand page size.
        page_size: u64,

        /// The out of bound size.
        oob_size: u64,

        /// The pages per block.
        pages_per_block: u64,

        /// The number of blocks.
        block_count: u64,
    },
}

fn default_fvm_emmc_compression() -> String {
    "lz4".to_string()
}

/// A filesystem to add to the FVM.
#[derive(Debug, PartialEq, Deserialize, Serialize)]
pub enum FvmFilesystemEntry {
    /// A BlobFS filesystem.
    BlobFS {
        /// The filesystem attributes of blobfs.
        #[serde(flatten)]
        attributes: FilesystemAttributes,
    },

    /// A MinFS filesystem.
    MinFS {
        /// The filesystem attributes of minfs.
        #[serde(flatten)]
        attributes: FilesystemAttributes,
    },
}

/// The information required to update and flash recovery.
#[derive(Debug, PartialEq, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RecoveryConfig {
    /// The path on the host to the prebuilt recovery ZBI.
    pub zbi: PathBuf,

    /// The path on the host to the prebuilt recovery VBMeta.
    pub vbmeta: Option<PathBuf>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::from_reader;
    use assert_matches::assert_matches;
    use serde::de::DeserializeOwned;
    use serde_json::json;
    use std::path::Path;

    impl ProductConfig {
        /// Helper function for constructing a ProductConfig in tests in this
        /// and other modules within the crate.
        pub fn new(kernel_path: impl AsRef<Path>, clock_backstop: u64) -> Self {
            Self {
                system: Vec::default(),
                base: Vec::default(),
                cache: Vec::default(),
                boot_args: Vec::default(),
                bootfs_files: Vec::default(),
                kernel: KernelConfig {
                    path: kernel_path.as_ref().into(),
                    args: Vec::default(),
                    clock_backstop,
                },
            }
        }
    }

    impl Default for BoardConfig {
        fn default() -> Self {
            Self {
                base_package_name: default_base_package_name(),
                vbmeta: None,
                zbi: ZbiConfig::default(),
                blobfs: BlobFSConfig::default(),
                fvm: None,
            }
        }
    }

    fn from_json_str<T>(json: &str) -> Result<T>
    where
        T: serde::de::DeserializeOwned,
    {
        let mut cursor = std::io::Cursor::new(json);
        return from_reader(&mut cursor);
    }

    #[test]
    fn product_from_json_file() {
        let json = r#"
            {
              "system": ["package0"],
              "base": ["package1", "package2"],
              "cache": ["package3", "package4"],
              "kernel": {
                "path": "path/to/kernel",
                "args": ["arg1", "arg2"],
                "clock_backstop": 0
              },
              "bootfs_files": [
                {
                    "source": "path/to/source",
                    "destination": "path/to/destination"
                }
              ]
            }
        "#;

        let config: PartialProductConfig = from_json_str(json).unwrap();
        assert_matches!(
            config.kernel,
            Some(PartialKernelConfig { path: Some(_), args: _, clock_backstop: Some(0) })
        );
    }

    #[test]
    fn product_from_json5_file() {
        let json = r#"
            {
              // json5 files can have comments in them.
              system: ["package0"],
              base: ["package1", "package2"],
              cache: ["package3", "package4"],
              kernel: {
                path: "path/to/kernel",
                args: ["arg1", "arg2"],
                clock_backstop: 0,
              },
              // and lists can have trailing commas
              boot_args: ["arg1", "arg2", ],
              bootfs_files: [
                {
                    source: "path/to/source",
                    destination: "path/to/destination",
                }
              ],
            }
        "#;

        let config: PartialProductConfig = from_json_str(json).unwrap();
        assert_matches!(
            config.kernel,
            Some(PartialKernelConfig { path: Some(_), args: _, clock_backstop: Some(0) })
        );
    }

    #[test]
    fn product_from_minimal_json_file() {
        let json = r#"
            {
              "kernel": {
                "path": "path/to/kernel",
                "clock_backstop": 0
              }
            }
        "#;

        let config: PartialProductConfig = from_json_str(json).unwrap();
        assert_matches!(
            config.kernel,
            Some(PartialKernelConfig { path: Some(_), args: _, clock_backstop: Some(0) })
        );
    }

    #[test]
    fn board_from_json_file() {
        let json = r#"
            {
              "base_package_name": "system_image",
              "vbmeta": {
                "kernel_partition": "zircon",
                "key": "path/to/key",
                "key_metadata": "path/to/metadata"
              },
              "zbi": {
                "name": "fuchsia",
                "max_size": 100,
                "embed_fvm_in_zbi": false,
                "compression": "zstd.max",
              },
              "fvm": {
                "slice_size": 100,
                "reserved_slices": 100,
                "filesystems": [
                  {
                    "MinFS": {
                      "path": "path/to/data.blk",
                      "name": "data",
                      "minimum_inodes": 100,
                      "minimum_data_size": 100,
                      "maximum_bytes": 100
                    }
                  },
                  {
                    "BlobFS": {
                      "name": "blob",
                      "minimum_inodes": 100,
                      "minimum_data_size": 100,
                      "maximum_bytes": 100
                    }
                  }
                ]
              },
              "blobfs": {
                "layout": "padded",
                "compress": true
              },
            }
         "#;

        let config: BoardConfig = from_json_str(json).unwrap();
        assert_eq!(config.base_package_name, "system_image");
    }

    #[test]
    fn empty_board_is_defaults() {
        let empty = from_json_str::<BoardConfig>("{}").unwrap();
        let empty_members = from_json_str::<BoardConfig>("{ blobfs:{}, zbi:{} }").unwrap();
        assert_eq!(empty, empty_members)
    }

    /// Helper fn that asserts that deserializing an empty dict for a given
    /// type results in the default() impl for that type.
    fn assert_deserialized_empty_dict_is_default<T>()
    where
        T: DeserializeOwned + Default + std::fmt::Debug + PartialEq,
    {
        let dut = from_json_str::<T>("{}").unwrap();
        let default_value = T::default();
        assert_eq!(dut, default_value);
    }

    #[test]
    fn board_from_minimal_json_file() {
        let config = from_json_str::<BoardConfig>("{}").unwrap();
        assert_eq!(config.base_package_name, "system_image");
    }

    #[test]
    fn board_from_minimal_is_default() {
        assert_deserialized_empty_dict_is_default::<BoardConfig>();
    }

    #[test]
    fn zbi_from_minimal_is_default() {
        assert_deserialized_empty_dict_is_default::<ZbiConfig>();
    }

    #[test]
    fn blobfs_from_minimal_is_default() {
        assert_deserialized_empty_dict_is_default::<BlobFSConfig>()
    }

    #[test]
    fn fvm_from_minimal_is_default() {
        assert_deserialized_empty_dict_is_default::<FvmConfig>();
    }

    #[test]
    fn product_from_invalid_json_file() {
        let json = r#"
            {
                "invalid": "data"
            }
        "#;

        let config: Result<ProductConfig> = from_json_str(json);
        assert!(config.is_err());
    }

    #[test]
    fn board_from_invalid_json_file() {
        let json = r#"
            {
                "invalid": "data"
            }
        "#;

        let config: Result<BoardConfig> = from_json_str(json);
        assert!(config.is_err());
    }

    #[test]
    fn merge_product_config() {
        let config_a = serde_json::from_value::<PartialProductConfig>(json!({
            "system": ["package0a"],
            "base": ["package1a", "package2a"],
            "cache": ["package3a", "package4a"],
            "kernel": {
                "path": "path/to/kernel",
                "args": ["arg10", "arg20"],
                "clock_backstop": 0
            },
            "bootfs_files": [
                {
                    "source": "path/to/source/a",
                    "destination": "path/to/destination/a"
                }
            ],
            "boot_args": [ "arg1a", "arg2a" ]
        }))
        .unwrap();

        let config_b = serde_json::from_value::<PartialProductConfig>(json!({
          "system": ["package0b"],
          "base": ["package1a", "package2b"],
          "cache": ["package3b", "package4b"],
          "bootfs_files": [
              {
                  "source": "path/to/source/b",
                  "destination": "path/to/destination/b"
              }
          ],
          "boot_args": [ "arg1b", "arg2b" ]
        }))
        .unwrap();

        let config_c = serde_json::from_value::<PartialProductConfig>(json!({
          "system": ["package0c"],
          "base": ["package1a", "package2c"],
          "cache": ["package3c", "package4c"],
          "bootfs_files": [
              {
                  "source": "path/to/source/c",
                  "destination": "path/to/destination/c"
              }
          ],
          "boot_args": [ "arg1c", "arg2c" ]
        }))
        .unwrap();

        let result = ProductConfig::try_from_partials(vec![config_a, config_b, config_c]).unwrap();

        let expected = serde_json::from_value::<ProductConfig>(json!({
            "system": ["package0a", "package0b", "package0c"],
            "base": ["package1a", "package2a", "package2b", "package2c"],
            "cache": ["package3a", "package3b", "package3c", "package4a", "package4b", "package4c"],
            "kernel": {
                "path": "path/to/kernel",
                "args": ["arg10", "arg20"],
                "clock_backstop": 0
            },
            "bootfs_files": [
                {
                    "source": "path/to/source/a",
                    "destination": "path/to/destination/a"
                },
                {
                    "source": "path/to/source/b",
                    "destination": "path/to/destination/b"
                },
                {
                    "source": "path/to/source/c",
                    "destination": "path/to/destination/c"
                },
            ],
            "boot_args": [ "arg1a", "arg1b", "arg1c", "arg2a", "arg2b", "arg2c" ]
        }))
        .unwrap();

        assert_eq!(result, expected);
    }

    #[test]
    fn test_fail_merge_with_no_kernel() {
        let config_a = PartialProductConfig::default();
        let config_b = PartialProductConfig::default();

        let result = ProductConfig::try_from_partials(vec![config_a, config_b]);
        assert!(result.is_err());
    }

    #[test]
    fn test_fail_merge_with_more_than_one_kernel() {
        let config_a = PartialProductConfig {
            kernel: Some(PartialKernelConfig {
                path: Some("foo".into()),
                args: Vec::default(),
                clock_backstop: Some(0),
            }),
            ..PartialProductConfig::default()
        };
        let config_b = PartialProductConfig {
            kernel: Some(PartialKernelConfig {
                path: Some("bar".into()),
                args: Vec::default(),
                clock_backstop: Some(2),
            }),
            ..PartialProductConfig::default()
        };

        let result = ProductConfig::try_from_partials(vec![config_a, config_b]);
        assert!(result.is_err());
    }
}
