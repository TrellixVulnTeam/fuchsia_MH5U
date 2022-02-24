// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::base_package::BasePackage;
use crate::config::{ProductConfig, ZbiConfig};
use crate::util::pkg_manifest_from_path;

use anyhow::{anyhow, Context, Result};
use assembly_images_config::{PostProcessingScript, Zbi, ZbiCompression};
use assembly_tool::Tool;
use assembly_util::PathToStringExt;
use fuchsia_pkg::PackageManifest;
use std::path::{Path, PathBuf};
use std::process::Command;
use zbi::ZbiBuilder;

pub fn convert_to_new_config(zbi_config: &ZbiConfig) -> Result<Zbi> {
    Ok(Zbi {
        name: zbi_config.name.clone(),
        compression: zbi_config.compression.parse()?,
        postprocessing_script: match &zbi_config.signing_script {
            None => None,
            Some(signing_script) => Some(PostProcessingScript {
                path: signing_script.tool.clone(),
                args: signing_script.extra_arguments.clone(),
            }),
        },
    })
}

pub fn construct_zbi(
    zbi_tool: Box<dyn Tool>,
    outdir: impl AsRef<Path>,
    gendir: impl AsRef<Path>,
    product: &ProductConfig,
    zbi_config: &Zbi,
    base_package: Option<&BasePackage>,
    fvm: Option<impl AsRef<Path>>,
) -> Result<PathBuf> {
    let mut zbi_builder = ZbiBuilder::new(zbi_tool);

    // Add the kernel image.
    zbi_builder.set_kernel(&product.kernel.path);

    // Add the additional boot args.
    for boot_arg in &product.boot_args {
        zbi_builder.add_boot_arg(boot_arg);
    }

    // If a base merkle is supplied, then add the boot arguments for starting up pkgfs with the
    // merkle of the Base Package.
    if let Some(base_package) = &base_package {
        // Indicate the clock UTC backstop.
        zbi_builder.add_boot_arg(&format!("clock.backstop={}", product.kernel.clock_backstop));

        // Instruct devmgr that a /system volume is required.
        zbi_builder.add_boot_arg("devmgr.require-system=true");

        // Specify how to launch pkgfs: bin/pkgsvr <base-merkle>
        zbi_builder
            .add_boot_arg(&format!("zircon.system.pkgfs.cmd=bin/pkgsvr+{}", &base_package.merkle));

        // Add the pkgfs blobs to the boot arguments, so that pkgfs can be bootstrapped out of blobfs,
        // before the blobfs service is available.
        let pkgfs_manifest: PackageManifest = product
            .base
            .iter()
            .find_map(|p| {
                if let Ok(m) = pkg_manifest_from_path(p) {
                    if m.name().as_ref() == "pkgfs" {
                        return Some(m);
                    }
                }
                return None;
            })
            .context("Failed to find pkgfs in the base packages")?;

        pkgfs_manifest.into_blobs().into_iter().filter(|b| b.path != "meta/").for_each(|b| {
            zbi_builder.add_boot_arg(&format!("zircon.system.pkgfs.file.{}={}", b.path, b.merkle));
        });
    }

    // Add the command line.
    for cmd in &product.kernel.args {
        zbi_builder.add_cmdline_arg(cmd);
    }

    // Add the BootFS files.
    for bootfs_entry in &product.bootfs_files {
        zbi_builder.add_bootfs_file(&bootfs_entry.source, &bootfs_entry.destination);
    }

    // Add the FVM as a ramdisk in the ZBI if necessary.
    if let Some(fvm) = &fvm {
        zbi_builder.add_ramdisk(&fvm);
    }

    // Set the zbi compression to use.
    zbi_builder.set_compression(match zbi_config.compression {
        ZbiCompression::ZStd => "zstd",
        ZbiCompression::ZStdMax => "zstd.max",
    });

    // Create an output manifest that describes the contents of the built ZBI.
    zbi_builder.set_output_manifest(&gendir.as_ref().join("zbi.json"));

    // Build and return the ZBI.
    let zbi_path = outdir.as_ref().join(format!("{}.zbi", zbi_config.name));
    zbi_builder.build(gendir, zbi_path.as_path())?;
    Ok(zbi_path)
}

/// If the board requires the zbi to be post-processed to make it bootable by
/// the bootloaders, then perform that task here.
pub fn vendor_sign_zbi(
    outdir: impl AsRef<Path>,
    zbi_config: &Zbi,
    zbi: impl AsRef<Path>,
) -> Result<PathBuf> {
    let script = match &zbi_config.postprocessing_script {
        Some(script) => script,
        _ => return Err(anyhow!("Missing postprocessing_script")),
    };

    // The resultant file path
    let signed_path = outdir.as_ref().join(format!("{}.zbi.signed", zbi_config.name));

    // The parameters of the script that are required:
    let mut args = Vec::new();
    args.push("-z".to_string());
    args.push(zbi.as_ref().path_to_string()?);
    args.push("-o".to_string());
    args.push(signed_path.path_to_string()?);

    // If the script config defines extra arguments, add them:
    args.extend_from_slice(&script.args[..]);

    let output = Command::new(&script.path)
        .args(&args)
        .output()
        .context(format!("Failed to run the vendor tool: {}", script.path.display()))?;

    // The tool ran, but may have returned an error.
    if output.status.success() {
        Ok(signed_path)
    } else {
        Err(anyhow!(
            "Vendor tool returned an error: {}\n{}",
            output.status,
            String::from_utf8_lossy(output.stderr.as_slice())
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::{construct_zbi, convert_to_new_config, vendor_sign_zbi};

    use crate::base_package::BasePackage;
    use crate::config::{ProductConfig, ZbiConfig, ZbiSigningScript};
    use assembly_images_config::{PostProcessingScript, Zbi, ZbiCompression};
    use assembly_test_util::generate_fake_tool;
    use assembly_tool::testing::FakeToolProvider;
    use assembly_tool::ToolProvider;
    use assembly_util::PathToStringExt;
    use fuchsia_hash::Hash;
    use serde_json::json;
    use serial_test::serial;
    use std::collections::BTreeMap;
    use std::fs::File;
    use std::io::Write;
    use std::path::{Path, PathBuf};
    use std::str::FromStr;
    use tempfile::tempdir;

    #[test]
    fn old_config() {
        let old_config = ZbiConfig {
            name: "fuchsia".into(),
            max_size: 0,
            embed_fvm_in_zbi: false,
            compression: "zstd".into(),
            signing_script: Some(ZbiSigningScript {
                tool: "tool".into(),
                extra_arguments: vec!["one".to_string(), "two".to_string()],
            }),
        };
        let new_config = convert_to_new_config(&old_config).unwrap();
        assert_eq!(new_config.name, "fuchsia");
        assert_eq!(new_config.compression, ZbiCompression::ZStd);
        let script = new_config.postprocessing_script.unwrap();
        assert_eq!(script.path, PathBuf::from("tool"));
        assert_eq!(script.args, vec!["one".to_string(), "two".to_string()]);

        let old_config = ZbiConfig {
            name: "fuchsia".into(),
            max_size: 0,
            embed_fvm_in_zbi: false,
            compression: "zstd.max".into(),
            signing_script: None,
        };
        let new_config = convert_to_new_config(&old_config).unwrap();
        assert_eq!(new_config.name, "fuchsia");
        assert_eq!(new_config.compression, ZbiCompression::ZStdMax);
        assert!(new_config.postprocessing_script.is_none());
    }

    // These tests must be ran serially, because otherwise they will affect each
    // other through process spawming. If a test spawns a process while the
    // other test has an open file, then the spawned process will get a copy of
    // the open file descriptor, preventing the other test from executing it.
    #[test]
    #[serial]
    fn construct() {
        let dir = tempdir().unwrap();

        // Create fake product/board definitions.
        let kernel_path = dir.path().join("kernel");
        let mut product_config = ProductConfig::new(&kernel_path, 0);
        let zbi_config = Zbi {
            name: "fuchsia".into(),
            compression: ZbiCompression::ZStd,
            postprocessing_script: None,
        };

        // Create a kernel which is equivalent to: zbi --ouput <zbi-name>
        let kernel_bytes = vec![
            0x42, 0x4f, 0x4f, 0x54, 0x00, 0x00, 0x00, 0x00, 0xe6, 0xf7, 0x8c, 0x86, 0x00, 0x00,
            0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29, 0x17, 0x78, 0xb5,
            0xd6, 0xe8, 0x87, 0x4a,
        ];
        std::fs::write(&kernel_path, kernel_bytes).unwrap();

        // Create a fake pkgfs.
        let pkgfs_manifest_path = generate_test_manifest_file(dir.path(), "pkgfs");
        product_config.base.push(pkgfs_manifest_path);

        // Create a fake base package.
        let base_path = dir.path().join("base.far");
        std::fs::write(&base_path, "fake base").unwrap();
        let base = BasePackage {
            merkle: Hash::from_str(
                "0000000000000000000000000000000000000000000000000000000000000000",
            )
            .unwrap(),
            contents: BTreeMap::default(),
            path: base_path,
        };

        // Create a fake zbi tool.
        let tools = FakeToolProvider::default();
        let zbi_tool = tools.get_tool("zbi").unwrap();

        construct_zbi(
            zbi_tool,
            dir.path(),
            dir.path(),
            &product_config,
            &zbi_config,
            Some(&base),
            None::<PathBuf>,
        )
        .unwrap();
    }

    #[test]
    #[serial]
    fn vendor_sign() {
        let dir = tempdir().unwrap();
        let expected_output = dir.path().join("fuchsia.zbi.signed");

        // Create a fake zbi.
        let zbi_path = dir.path().join("fuchsia.zbi");
        std::fs::write(&zbi_path, "fake zbi").unwrap();

        // Create the signing tool that ensures that we pass the correct arguments.
        let tool_path = dir.path().join("tool.sh");
        generate_fake_tool(
            &tool_path,
            format!(
                r#"#!/bin/bash
            if [[ "$1" != "-z" || "$3" != "-o" ]]; then
              exit 1
            fi
            if [[ "$2" != {} ]]; then
              exit 1
            fi
            if [[ "$4" != {} ]]; then
              exit 1
            fi
            if [[ "$5" != {} || "$6" != {} ]]; then
              exit 1
            fi
            exit 0
        "#,
                zbi_path.path_to_string().unwrap(),
                expected_output.path_to_string().unwrap(),
                "arg1",
                "arg2",
            ),
        );

        // Create the signing config.
        let extra_arguments = vec!["arg1".into(), "arg2".into()];

        // Create fake zbi config.
        let zbi = Zbi {
            name: "fuchsia".into(),
            compression: ZbiCompression::ZStd,
            postprocessing_script: Some(PostProcessingScript {
                path: tool_path,
                args: extra_arguments,
            }),
        };

        // Sign the zbi.
        let signed_zbi_path = vendor_sign_zbi(dir.path(), &zbi, &zbi_path).unwrap();
        assert_eq!(signed_zbi_path, expected_output);
    }

    // Generates a package manifest to be used for testing. The file is written
    // into `dir`, and the location is returned. The `name` is used in the blob
    // file names to make each manifest somewhat unique.
    // TODO(fxbug.dev/76993): See if we can share this with BasePackage.
    pub fn generate_test_manifest_file(dir: impl AsRef<Path>, name: impl AsRef<str>) -> PathBuf {
        // Create a data file for the package.
        let data_file_name = format!("{}_data.txt", name.as_ref());
        let data_path = dir.as_ref().join(&data_file_name);
        let data_file = File::create(&data_path).unwrap();
        write!(&data_file, "bleh").unwrap();

        // Create the manifest.
        let manifest_path = dir.as_ref().join(format!("{}.json", name.as_ref()));
        let manifest_file = File::create(&manifest_path).unwrap();
        serde_json::to_writer(
            &manifest_file,
            &json!({
                    "version": "1",
                    "repository": "testrepository.com",
                    "package": {
                        "name": name.as_ref(),
                        "version": "1",
                    },
                    "blobs": [
                        {
                            "source_path": format!("path/to/{}/meta.far", name.as_ref()),
                            "path": "meta/",
                            "merkle":
                                "0000000000000000000000000000000000000000000000000000000000000000",
                            "size": 1
                        },
                        {
                            "source_path": &data_path,
                            "path": &data_file_name,
                            "merkle":
                                "1111111111111111111111111111111111111111111111111111111111111111",
                            "size": 1
                        },
                    ]
                }
            ),
        )
        .unwrap();
        manifest_path
    }
}
