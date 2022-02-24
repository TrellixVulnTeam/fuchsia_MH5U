// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::update::{paver, BuildInfo},
    anyhow::{anyhow, Error},
    epoch::EpochFile,
    fidl_fuchsia_mem::Buffer,
    fidl_fuchsia_paver::{Asset, BootManagerProxy, DataSinkProxy},
    fuchsia_inspect as inspect,
    fuchsia_syslog::{fx_log_err, fx_log_info, fx_log_warn},
    mundane::hash::{Hasher as _, Sha256},
    serde::{Deserialize, Serialize},
    std::{convert::TryInto as _, str::FromStr},
    update_package::{ImageType, SystemVersion, UpdatePackage},
};

/// The version of the OS.
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Version {
    /// The hash of the update package.
    pub update_hash: String,
    /// The hash of the system image package.
    pub system_image_hash: String,
    /// The vbmeta and zbi hash are SHA256 hash of the image with trailing zeros removed, we can't
    /// use the exact image because when reading from paver we get the entire partition back.
    pub vbmeta_hash: String,
    pub zbi_hash: String,
    /// The version in build-info.
    pub build_version: SystemVersion,
    /// The epoch of the update package.
    pub epoch: String,
}

impl Default for Version {
    fn default() -> Self {
        Version {
            update_hash: Default::default(),
            system_image_hash: Default::default(),
            vbmeta_hash: Default::default(),
            zbi_hash: Default::default(),
            build_version: SystemVersion::Opaque("".to_string()),
            epoch: "1".to_string(),
        }
    }
}

impl Version {
    #[cfg(test)]
    pub fn for_hash(update_hash: impl Into<String>) -> Self {
        Self { update_hash: update_hash.into(), ..Self::default() }
    }

    #[cfg(test)]
    pub fn for_hash_and_empty_paver_hashes(update_hash: impl Into<String>) -> Self {
        const EMPTY_HASH: &str = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        Self {
            update_hash: update_hash.into(),
            vbmeta_hash: EMPTY_HASH.to_owned(),
            zbi_hash: EMPTY_HASH.to_owned(),
            ..Self::default()
        }
    }

    /// Returns the Version for the given update package.
    pub async fn for_update_package(update_package: &UpdatePackage) -> Self {
        let update_hash = match update_package.hash().await {
            Ok(hash) => hash.to_string(),
            Err(e) => {
                fx_log_err!("Failed to get update hash: {:#}", anyhow!(e));
                "".to_string()
            }
        };
        let system_image_hash =
            get_system_image_hash_from_update_package(update_package).await.unwrap_or_else(|e| {
                fx_log_err!("Failed to get system image hash: {:#}", anyhow!(e));
                "".to_string()
            });
        let vbmeta_hash =
            get_image_hash_from_update_package(update_package, ImageType::FuchsiaVbmeta)
                .await
                .unwrap_or_else(|e| {
                    fx_log_err!("Failed to read vbmeta hash: {:#}", anyhow!(e));
                    "".to_string()
                });
        let zbi_hash =
            match get_image_hash_from_update_package(update_package, ImageType::Zbi).await {
                Ok(v) => v,
                Err(_) => get_image_hash_from_update_package(update_package, ImageType::ZbiSigned)
                    .await
                    .unwrap_or_else(|e| {
                        fx_log_err!("Failed to read zbi hash: {:#}", anyhow!(e));
                        "".to_string()
                    }),
            };
        let build_version = update_package.version().await.unwrap_or_else(|e| {
            fx_log_err!("Failed to read build version: {:#}", anyhow!(e));
            SystemVersion::Opaque("".to_string())
        });
        let epoch = match update_package.epoch().await {
            Ok(Some(epoch)) => epoch.to_string(),
            Ok(None) => {
                fx_log_info!("epoch.json does not exist, defaulting to zero");
                "0".to_string()
            }
            Err(e) => {
                fx_log_err!("Failed to read epoch: {:#}", anyhow!(e));
                "".to_string()
            }
        };
        Self { update_hash, system_image_hash, vbmeta_hash, zbi_hash, build_version, epoch }
    }

    /// Returns the Version for the current running system.
    pub async fn current(
        last_target_version: Option<&Version>,
        data_sink: &DataSinkProxy,
        boot_manager: &BootManagerProxy,
        build_info: &impl BuildInfo,
        pkgfs_system: &Option<pkgfs::system::Client>,
        source_epoch_raw: &str,
    ) -> Self {
        let system_image_hash =
            get_system_image_hash_from_pkgfs_system(pkgfs_system).await.unwrap_or_else(|e| {
                fx_log_err!("Failed to read system image hash: {:#}", anyhow!(e));
                "".to_string()
            });
        let (vbmeta_hash, zbi_hash) =
            get_vbmeta_and_zbi_hash_from_environment(data_sink, boot_manager).await.unwrap_or_else(
                |e| {
                    fx_log_err!("Failed to read vbmeta and/or zbi hash: {:#}", anyhow!(e));
                    ("".to_string(), "".to_string())
                },
            );
        let build_version = match build_info.version().await {
            Ok(Some(version)) => version,
            Ok(None) => {
                fx_log_err!("Build version not found");
                "".to_string()
            }
            Err(e) => {
                fx_log_err!("Failed to read build version: {:#}", anyhow!(e));
                "".to_string()
            }
        };

        let build_version = SystemVersion::from_str(&build_version).unwrap();
        let update_hash = match last_target_version {
            Some(version) => {
                if vbmeta_hash == version.vbmeta_hash
                    && (system_image_hash == "" || system_image_hash == version.system_image_hash)
                    && (zbi_hash == "" || zbi_hash == version.zbi_hash)
                    && (build_version.is_empty() || build_version == version.build_version)
                {
                    version.update_hash.clone()
                } else {
                    "".to_string()
                }
            }
            None => "".to_string(),
        };
        let epoch = match serde_json::from_str(source_epoch_raw) {
            Ok(EpochFile::Version1 { epoch }) => epoch.to_string(),
            Err(e) => {
                fx_log_err!("Failed to parse source epoch: {:#}", anyhow!(e));
                "".to_string()
            }
        };

        Self { update_hash, system_image_hash, vbmeta_hash, zbi_hash, build_version, epoch }
    }

    pub fn write_to_inspect(&self, node: &inspect::Node) {
        // This destructure exists to use the compiler to guarantee we are copying all the
        // UpdateAttempt fields to inspect.
        let Version { update_hash, system_image_hash, vbmeta_hash, zbi_hash, build_version, epoch } =
            self;
        node.record_string("update_hash", update_hash);
        node.record_string("system_image_hash", system_image_hash);
        node.record_string("vbmeta_hash", vbmeta_hash);
        node.record_string("zbi_hash", zbi_hash);
        node.record_string("build_version", build_version.to_string());
        node.record_string("epoch", epoch);
    }
}

async fn get_system_image_hash_from_update_package(
    update_package: &UpdatePackage,
) -> Result<String, Error> {
    let packages = update_package.packages().await?;
    let system_image = packages
        .into_iter()
        .find(|url| url.path() == "/system_image/0")
        .ok_or_else(|| anyhow!("system image not found"))?;
    let hash =
        system_image.package_hash().ok_or_else(|| anyhow!("system image package has no hash"))?;
    Ok(hash.to_string())
}

async fn get_image_hash_from_update_package(
    update_package: &UpdatePackage,
    image: ImageType,
) -> Result<String, Error> {
    let buffer = update_package.open_image(&update_package::Image::new(image, None)).await?;
    Ok(sha256_hash_with_no_trailing_zeros(buffer)?)
}

async fn get_system_image_hash_from_pkgfs_system(
    pkgfs_system: &Option<pkgfs::system::Client>,
) -> Result<String, Error> {
    match pkgfs_system.as_ref() {
        Some(pkgfs_system) => Ok(pkgfs_system.hash().await?.to_string()),
        None => Err(anyhow!("pkgfs/system not available")),
    }
}

async fn get_vbmeta_and_zbi_hash_from_environment(
    data_sink: &DataSinkProxy,
    boot_manager: &BootManagerProxy,
) -> Result<(String, String), Error> {
    let current_configuration = paver::query_current_configuration(boot_manager).await?;
    let configuration = current_configuration
        .to_configuration()
        .ok_or_else(|| anyhow!("device does not support ABR"))?;
    let vbmeta_buffer =
        paver::paver_read_asset(data_sink, configuration, Asset::VerifiedBootMetadata).await?;
    let vbmeta_hash = sha256_hash_with_no_trailing_zeros(vbmeta_buffer)?;
    let zbi_buffer = paver::paver_read_asset(data_sink, configuration, Asset::Kernel).await?;
    let zbi_hash = sha256_hash_with_no_trailing_zeros(zbi_buffer)?;
    Ok((vbmeta_hash, zbi_hash))
}

// Compute the SHA256 hash of the buffer with trailing zeros stripped.
fn sha256_hash_with_no_trailing_zeros(buffer: Buffer) -> Result<String, Error> {
    let mut size = buffer.size.try_into()?;
    let mut data = vec![0u8; size];
    buffer.vmo.read(&mut data, 0)?;
    while size > 0 && data[size - 1] == 0 {
        size -= 1;
    }
    if size == 0 {
        fx_log_warn!("entire buffer is 0, size: {}", buffer.size);
    }
    Ok(Sha256::hash(&data[..size]).to_string())
}

#[cfg(test)]
pub(super) async fn mock_pkgfs_system(
    system_image_hash: impl AsRef<[u8]>,
) -> (pkgfs::system::Client, tempfile::TempDir) {
    let pkgfs_dir = tempfile::tempdir().expect("/tmp to exist");
    std::fs::create_dir(pkgfs_dir.path().join("system")).expect("create dir");
    io_util::file::write_in_namespace(
        pkgfs_dir.path().join("system/meta").to_str().unwrap(),
        system_image_hash,
    )
    .await
    .expect("write to temp dir");
    let pkgfs_proxy = io_util::directory::open_in_namespace(
        pkgfs_dir.path().to_str().unwrap(),
        io_util::OPEN_RIGHT_READABLE,
    )
    .expect("temp dir to open");
    (pkgfs::system::Client::open_from_pkgfs_root(&pkgfs_proxy).unwrap(), pkgfs_dir)
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::update::environment::NamespaceBuildInfo,
        ::version::Version as SemanticVersion,
        fidl_fuchsia_paver::Configuration,
        fuchsia_pkg_testing::{make_epoch_json, TestUpdatePackage},
        fuchsia_zircon::Vmo,
        mock_paver::{hooks as mphooks, MockPaverServiceBuilder},
        pretty_assertions::assert_eq,
        std::sync::Arc,
    };

    #[fuchsia_async::run_singlethreaded(test)]
    async fn version_for_invalid_update_package() {
        let update_pkg = TestUpdatePackage::new();
        assert_eq!(
            Version::for_update_package(&update_pkg).await,
            Version { epoch: "0".to_string(), ..Version::default() }
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn version_for_valid_update_package() {
        let update_pkg = TestUpdatePackage::new()
            .hash("2937013f2181810606b2a799b05bda2849f3e369a20982a4138f0e0a55984ce4")
            .await
            .add_package("fuchsia-pkg://fuchsia.com/system_image/0?hash=838b5199d12c8ff4ef92bfd9771d2f8781b7b8fd739dd59bcf63f353a1a93f67")
            .await
            .add_file("fuchsia.vbmeta", "vbmeta")
            .await
            .add_file("zbi", "zbi")
            .await
            .add_file("version", "1.2.3.4")
            .await
            .add_file("epoch.json", make_epoch_json(1)).await;
        assert_eq!(
            Version::for_update_package(&update_pkg).await,
            Version {
                update_hash: "2937013f2181810606b2a799b05bda2849f3e369a20982a4138f0e0a55984ce4"
                    .to_string(),
                system_image_hash:
                    "838b5199d12c8ff4ef92bfd9771d2f8781b7b8fd739dd59bcf63f353a1a93f67".to_string(),
                vbmeta_hash: "a0c6f07a4b3a17fb9348db981de3c5602e2685d626599be1bd909195c694a57b"
                    .to_string(),
                // Should be "a7124150e065aa234710ab3875230f17deb36a9249938e11f2f3656954412ab8"
                // See comment in sha256_hash_removed_trailing_zeros test.
                zbi_hash: "a7124150e065aa234710ab387523f17deb36a9249938e11f2f3656954412ab8"
                    .to_string(),
                build_version: SystemVersion::Semantic(SemanticVersion::from([1, 2, 3, 4])),
                epoch: "1".to_string()
            }
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn version_current_copy_update_hash() {
        let paver = Arc::new(
            MockPaverServiceBuilder::new()
                .active_config(Configuration::A)
                .insert_hook(mphooks::read_asset(|configuration, asset| {
                    assert_eq!(configuration, Configuration::A);
                    match asset {
                        Asset::Kernel => Ok(vec![0x7a, 0x62, 0x69]),
                        Asset::VerifiedBootMetadata => Ok(vec![0x76, 0x62, 0x6d, 0x65, 0x74, 0x61]),
                    }
                }))
                .build(),
        );
        let data_sink = paver.spawn_data_sink_service();
        let boot_manager = paver.spawn_boot_manager_service();
        let (pkgfs_system, _pkgfs_dir) =
            mock_pkgfs_system("838b5199d12c8ff4ef92bfd9771d2f8781b7b8fd739dd59bcf63f353a1a93f67")
                .await;
        let last_target_version = Version {
            update_hash: "2937013f2181810606b2a799b05bda2849f3e369a20982a4138f0e0a55984ce4"
                .to_string(),
            system_image_hash: "838b5199d12c8ff4ef92bfd9771d2f8781b7b8fd739dd59bcf63f353a1a93f67"
                .to_string(),
            vbmeta_hash: "a0c6f07a4b3a17fb9348db981de3c5602e2685d626599be1bd909195c694a57b"
                .to_string(),
            // Should be "a7124150e065aa234710ab3875230f17deb36a9249938e11f2f3656954412ab8"
            // See comment in sha256_hash_removed_trailing_zeros test.
            zbi_hash: "a7124150e065aa234710ab387523f17deb36a9249938e11f2f3656954412ab8".to_string(),
            build_version: SystemVersion::Opaque("".to_string()),
            epoch: "1".to_string(),
        };
        assert_eq!(
            Version::current(
                Some(&last_target_version),
                &data_sink,
                &boot_manager,
                &NamespaceBuildInfo,
                &Some(pkgfs_system),
                &make_epoch_json(1)
            )
            .await,
            last_target_version
        );
    }

    #[test]
    fn sha256_hash_empty_buffer() {
        let buffer = Buffer { vmo: Vmo::create(0).unwrap(), size: 0 };
        assert_eq!(
            sha256_hash_with_no_trailing_zeros(buffer).unwrap(),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
    }

    #[test]
    fn sha256_hash_all_zero_buffer() {
        let vmo = Vmo::create(100).unwrap();
        vmo.write(&[0; 100], 0).unwrap();
        let buffer = Buffer { vmo, size: 100 };
        assert_eq!(
            sha256_hash_with_no_trailing_zeros(buffer).unwrap(),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
    }

    #[test]
    fn sha256_hash_removed_trailing_zeros() {
        let vmo = Vmo::create(100).unwrap();
        vmo.write(&[0; 100], 0).unwrap();
        vmo.write(&[1; 1], 0).unwrap();
        let buffer = Buffer { vmo, size: 100 };
        assert_eq!(
            sha256_hash_with_no_trailing_zeros(buffer).unwrap(),
            // This hash string is only 63 characters, but it should be 64:
            // 4bf5122f344554c53bde2ebb8cd2b7e3d1600ad631c385a5d7cce23c7785459a
            // note the "00" becomes "0", this is a bug in mundane, see fxr/410079.
            "4bf5122f344554c53bde2ebb8cd2b7e3d160ad631c385a5d7cce23c7785459a"
        );
    }
}
