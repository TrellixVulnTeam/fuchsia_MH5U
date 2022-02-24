// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{anyhow, bail, Context, Error},
    async_trait::async_trait,
    epoch::EpochFile,
    fidl::endpoints::ProtocolMarker as _,
    fidl_fuchsia_io::DirectoryProxy,
    fidl_fuchsia_paver::DataSinkProxy,
    fidl_fuchsia_pkg::{
        PackageCacheProxy, PackageResolverProxy, RetainedPackagesMarker, RetainedPackagesProxy,
    },
    fidl_fuchsia_space::ManagerProxy as SpaceManagerProxy,
    fidl_fuchsia_update_installer_ext::{
        FetchFailureReason, Options, PrepareFailureReason, State, UpdateInfo,
    },
    fuchsia_async::{Task, TimeoutExt as _},
    fuchsia_syslog::{fx_log_err, fx_log_info},
    fuchsia_url::pkg_url::PkgUrl,
    futures::{prelude::*, stream::FusedStream},
    parking_lot::Mutex,
    std::{pin::Pin, sync::Arc, time::Duration},
    thiserror::Error,
    update_package::{Image, ImageType, UpdateMode, UpdatePackage},
};

mod config;
mod environment;
mod genutil;
mod history;
mod metrics;
mod paver;
mod reboot;
mod resolver;
mod state;

pub(super) use {
    config::Config,
    environment::{
        BuildInfo, CobaltConnector, Environment, EnvironmentConnector,
        NamespaceEnvironmentConnector,
    },
    genutil::GeneratorExt,
    history::{UpdateAttempt, UpdateHistory},
    reboot::{ControlRequest, RebootController},
    resolver::ResolveError,
};

#[cfg(test)]
pub(super) use {
    config::ConfigBuilder,
    environment::{NamespaceBuildInfo, NamespaceCobaltConnector},
};

const COBALT_FLUSH_TIMEOUT: Duration = Duration::from_secs(30);
const SOURCE_EPOCH_RAW: &str = &include_str!(env!("EPOCH_PATH"));

/// Error encountered in the Prepare state.
#[derive(Debug, Error)]
enum PrepareError {
    #[error("while determining source epoch: '{0:?}'")]
    ParseSourceEpochError(String, #[source] serde_json::Error),

    #[error("while determining target epoch")]
    ParseTargetEpochError(#[source] update_package::ParseEpochError),

    #[error("while determining packages to fetch")]
    ParsePackages(#[source] update_package::ParsePackageError),

    #[error("while determining update mode")]
    ParseUpdateMode(#[source] update_package::ParseUpdateModeError),

    #[error("while preparing partitions for update")]
    PreparePartitionMetdata(#[source] paver::PreparePartitionMetadataError),

    #[error("while resolving the update package")]
    ResolveUpdate(#[source] ResolveError),

    #[error(
        "downgrades from epoch {src} to {target} are not allowed. For more context, see RFC-0071: https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0071_ota_backstop."
    )]
    UnsupportedDowngrade { src: u64, target: u64 },

    #[error("while verifying board name")]
    VerifyBoard(#[source] anyhow::Error),

    #[error("while verifying update package name")]
    VerifyName(#[source] update_package::VerifyNameError),

    #[error("force-recovery mode is incompatible with skip-recovery option")]
    VerifyUpdateMode,
}

impl PrepareError {
    fn reason(&self) -> PrepareFailureReason {
        match self {
            Self::ResolveUpdate(ResolveError::Error(
                fidl_fuchsia_pkg_ext::ResolveError::NoSpace,
                _,
            )) => PrepareFailureReason::OutOfSpace,
            Self::UnsupportedDowngrade { .. } => PrepareFailureReason::UnsupportedDowngrade,
            _ => PrepareFailureReason::Internal,
        }
    }
}

/// Error encountered in the Fetch state.
#[derive(Debug, Error)]
enum FetchError {
    #[error("while resolving a package")]
    Resolve(#[source] ResolveError),

    #[error("while syncing pkg-cache")]
    Sync(#[source] anyhow::Error),
}

impl FetchError {
    fn reason(&self) -> FetchFailureReason {
        match self {
            Self::Resolve(ResolveError::Error(fidl_fuchsia_pkg_ext::ResolveError::NoSpace, _)) => {
                FetchFailureReason::OutOfSpace
            }
            _ => FetchFailureReason::Internal,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum CommitAction {
    /// A reboot is required to apply the update, which should be performed by the system updater.
    Reboot,

    /// A reboot is required to apply the update, but the initiator of the update requested to
    /// perform the reboot themselves.
    RebootDeferred,
}

/// A trait to update the system in the given `Environment` using the provided config options.
#[async_trait(?Send)]
pub trait Updater {
    type UpdateStream: FusedStream<Item = State>;

    async fn update(
        &mut self,
        config: Config,
        env: Environment,
        reboot_controller: RebootController,
    ) -> (String, Self::UpdateStream);
}

pub struct RealUpdater {
    history: Arc<Mutex<UpdateHistory>>,
}

impl RealUpdater {
    pub fn new(history: Arc<Mutex<UpdateHistory>>) -> Self {
        Self { history }
    }
}

#[async_trait(?Send)]
impl Updater for RealUpdater {
    type UpdateStream = Pin<Box<dyn FusedStream<Item = State>>>;

    async fn update(
        &mut self,
        config: Config,
        env: Environment,
        reboot_controller: RebootController,
    ) -> (String, Self::UpdateStream) {
        let (attempt_id, attempt) =
            update(config, env, Arc::clone(&self.history), reboot_controller).await;
        (attempt_id, Box::pin(attempt))
    }
}

/// Updates the system in the given `Environment` using the provided config options.
///
/// Reboot vs RebootDeferred behavior is determined in the following priority order:
/// * is mode ForceRecovery? If so, reboot.
/// * is there a reboot controller? If so, yield reboot to the controller.
/// * if none of the above are true, reboot depending on the value of `Config::should_reboot`.
///
/// If a reboot is deferred, the initiator of the update is responsible for triggering
/// the reboot.
async fn update(
    config: Config,
    env: Environment,
    history: Arc<Mutex<UpdateHistory>>,
    reboot_controller: RebootController,
) -> (String, impl FusedStream<Item = State>) {
    let attempt_fut = history.lock().start_update_attempt(
        Options {
            initiator: config.initiator.into(),
            allow_attach_to_existing_attempt: config.allow_attach_to_existing_attempt,
            should_write_recovery: config.should_write_recovery,
        },
        config.update_url.clone(),
        config.start_time,
        &env.data_sink,
        &env.boot_manager,
        &env.build_info,
        &env.pkgfs_system,
    );
    let attempt = attempt_fut.await;
    let source_version = attempt.source_version().clone();
    let power_state_control = env.power_state_control.clone();

    let history_clone = Arc::clone(&history);
    let attempt_id = attempt.attempt_id().to_string();
    let stream = async_generator::generate(move |mut co| async move {
        let history = history_clone;
        // The only operation allowed to fail in this function is update_attempt. The rest of the
        // functionality here sets up the update attempt and takes the appropriate actions based on
        // whether the update attempt succeeds or fails.

        let mut phase = metrics::Phase::Tufupdate;
        let (mut cobalt, cobalt_forwarder_task) = env.cobalt_connector.connect();
        let cobalt_forwarder_task = Task::spawn(cobalt_forwarder_task);

        fx_log_info!("starting system update with config: {:?}", config);
        cobalt.log_ota_start("", config.initiator, config.start_time);

        let mut target_version = history::Version::default();

        let attempt_res = Attempt { config: &config, env: &env }
            .run(&mut co, &mut phase, &mut target_version)
            .await;

        fx_log_info!("system update attempt completed, logging metrics");
        let status_code = metrics::result_to_status_code(attempt_res.as_ref().map(|_| ()));
        let target_build_version = target_version.build_version.to_string();
        cobalt.log_ota_result_attempt(
            &target_build_version,
            config.initiator,
            history.lock().attempts_for(&source_version, &target_version) + 1,
            phase,
            status_code,
        );
        cobalt.log_ota_result_duration(
            &target_build_version,
            config.initiator,
            phase,
            status_code,
            config.start_time_mono.elapsed(),
        );
        drop(cobalt);

        // wait for all cobalt events to be flushed to the service.
        fx_log_info!("flushing cobalt events");
        let () = flush_cobalt(cobalt_forwarder_task, COBALT_FLUSH_TIMEOUT).await;

        let (state, mode, _packages) = match attempt_res {
            Ok(ok) => ok,
            Err(e) => {
                fx_log_err!("system update failed: {:#}", anyhow!(e));
                return target_version;
            }
        };

        fx_log_info!("checking if reboot is required or should be deferred, mode: {:?}", mode);
        // Figure out if we should reboot.
        match mode {
            // First priority: Always reboot on ForceRecovery success, even if the caller
            // asked to defer the reboot.
            UpdateMode::ForceRecovery => {
                fx_log_info!("system update in ForceRecovery mode complete, rebooting...");
            }
            // Second priority: Use the attached reboot controller.
            UpdateMode::Normal => {
                fx_log_info!("system update complete, waiting for initiator to signal reboot.");
                match reboot_controller.wait_to_reboot().await {
                    CommitAction::Reboot => {
                        fx_log_info!("initiator ready to reboot, rebooting...");
                    }
                    CommitAction::RebootDeferred => {
                        fx_log_info!("initiator deferred reboot to caller.");
                        state.enter_defer_reboot(&mut co).await;
                        return target_version;
                    }
                }
            }
        }

        state.enter_reboot(&mut co).await;
        target_version
    })
    .when_done(move |last_state: Option<State>, target_version| async move {
        let last_state = last_state.unwrap_or(State::Prepare);

        let should_reboot = matches!(last_state, State::Reboot { .. });

        let attempt = attempt.finish(target_version, last_state);
        history.lock().record_update_attempt(attempt);
        let save_fut = history.lock().save();
        save_fut.await;

        if should_reboot {
            reboot::reboot(&power_state_control).await;
        }
    });
    (attempt_id, stream)
}

async fn flush_cobalt(cobalt_forwarder_task: impl Future<Output = ()>, flush_timeout: Duration) {
    cobalt_forwarder_task.on_timeout(flush_timeout, || {
        fx_log_err!(
            "Couldn't flush cobalt events within the timeout. Proceeding, but may have dropped metrics."
        );
    })
    .await;
}

struct Attempt<'a> {
    config: &'a Config,
    env: &'a Environment,
}

impl<'a> Attempt<'a> {
    async fn run(
        mut self,
        co: &mut async_generator::Yield<State>,
        phase: &mut metrics::Phase,
        target_version: &mut history::Version,
    ) -> Result<(state::WaitToReboot, UpdateMode, Vec<DirectoryProxy>), Error> {
        // Prepare
        let state = state::Prepare::enter(co).await;

        let (update_pkg, mode, packages_to_fetch, current_configuration) =
            match self.prepare(target_version).await {
                Ok((update_pkg, mode, packages_to_fetch, current_configuration)) => {
                    (update_pkg, mode, packages_to_fetch, current_configuration)
                }
                Err(e) => {
                    state.fail(co, e.reason()).await;
                    bail!(e);
                }
            };

        // Fetch packages
        let mut state = state
            .enter_fetch(
                co,
                UpdateInfo::builder().download_size(0).build(),
                packages_to_fetch.len() as u64 + 1,
            )
            .await;
        *phase = metrics::Phase::PackageDownload;

        let packages = match self.fetch_packages(co, &mut state, packages_to_fetch, mode).await {
            Ok(packages) => packages,
            Err(e) => {
                state.fail(co, e.reason()).await;
                bail!(e);
            }
        };

        // Write images
        let mut state = state.enter_stage(co).await;
        *phase = metrics::Phase::ImageWrite;

        let () =
            match self.stage_images(co, &mut state, &update_pkg, mode, current_configuration).await
            {
                Ok(()) => (),
                Err(e) => {
                    state.fail(co).await;
                    bail!(e);
                }
            };

        // Success!
        let state = state.enter_wait_to_reboot(co).await;
        *phase = metrics::Phase::SuccessPendingReboot;

        Ok((state, mode, packages))
    }

    /// Acquire the necessary data to perform the update.
    ///
    /// This includes fetching the update package, which contains the list of packages in the
    /// target OS and kernel images that need written.
    async fn prepare(
        &mut self,
        target_version: &mut history::Version,
    ) -> Result<(UpdatePackage, UpdateMode, Vec<PkgUrl>, paver::CurrentConfiguration), PrepareError>
    {
        // Ensure that the partition boot metadata is ready for the update to begin. Specifically:
        // - the current configuration must be Healthy and Active, and
        // - the non-current configuration must be Unbootable.
        //
        // If anything goes wrong, abort the update. See the comments in
        // `prepare_partition_metadata` for why this is justified.
        //
        // We do this here rather than just before we write images because this location allows us
        // to "unstage" a previously staged OS in the non-current configuration that would otherwise
        // become active on next reboot. If we moved this to just before writing images, we would be
        // susceptible to a bug of the form:
        // - A is active/current running system version 1.
        // - Stage an OTA of version 2 to B, B is now marked active. Defer reboot.
        // - Start to stage a new OTA (version 3). Fetch packages encounters an error after fetching
        //   half of the updated packages.
        // - Retry the attempt for the new OTA (version 3). This GC may delete packages from the
        //   not-yet-booted system (version 2).
        // - Interrupt the update attempt, reboot.
        // - System attempts to boot to B (version 2), but the packages are not all present anymore
        let current_config = paver::prepare_partition_metadata(&self.env.boot_manager)
            .await
            .map_err(PrepareError::PreparePartitionMetdata)?;

        let update_pkg = resolve_update_package(
            &self.env.pkg_resolver,
            &self.config.update_url,
            &self.env.space_manager,
            &self.env.retained_packages,
        )
        .await
        .map_err(PrepareError::ResolveUpdate)?;

        *target_version = history::Version::for_update_package(&update_pkg).await;
        let () = update_pkg.verify_name().await.map_err(PrepareError::VerifyName)?;

        let mode = update_mode(&update_pkg).await.map_err(PrepareError::ParseUpdateMode)?;
        match mode {
            UpdateMode::Normal => {}
            UpdateMode::ForceRecovery => {
                if !self.config.should_write_recovery {
                    return Err(PrepareError::VerifyUpdateMode);
                }
            }
        }

        verify_board(&self.env.build_info, &update_pkg).await.map_err(PrepareError::VerifyBoard)?;

        let packages_to_fetch = match mode {
            UpdateMode::Normal => {
                update_pkg.packages().await.map_err(PrepareError::ParsePackages)?
            }
            UpdateMode::ForceRecovery => vec![],
        };

        let () = validate_epoch(SOURCE_EPOCH_RAW, &update_pkg).await?;

        let () = replace_retained_packages(
            packages_to_fetch
                .iter()
                .filter_map(|url| url.package_hash().cloned())
                .chain(self.config.update_url.package_hash().cloned()),
            &self.env.retained_packages,
        )
        .await
        .unwrap_or_else(|e| {
            fx_log_err!(
                "unable to replace retained packages set before gc in preparation \
                 for fetching packages listed in update package: {:#}",
                anyhow!(e)
            )
        });

        if let Err(e) = gc(&self.env.space_manager).await {
            fx_log_err!("unable to gc packages: {:#}", anyhow!(e));
        }

        Ok((update_pkg, mode, packages_to_fetch, current_config))
    }

    /// Fetch all base packages needed by the target OS.
    async fn fetch_packages(
        &mut self,
        co: &mut async_generator::Yield<State>,
        state: &mut state::Fetch,
        packages_to_fetch: Vec<PkgUrl>,
        mode: UpdateMode,
    ) -> Result<Vec<DirectoryProxy>, FetchError> {
        let mut packages = Vec::with_capacity(packages_to_fetch.len());

        let package_dir_futs =
            resolver::resolve_packages(&self.env.pkg_resolver, packages_to_fetch.iter());
        futures::pin_mut!(package_dir_futs);

        while let Some(package_dir) =
            package_dir_futs.try_next().await.map_err(FetchError::Resolve)?
        {
            packages.push(package_dir);

            state.add_progress(co, 1).await;
        }

        match mode {
            UpdateMode::Normal => {
                sync_package_cache(&self.env.pkg_cache).await.map_err(FetchError::Sync)?
            }
            UpdateMode::ForceRecovery => {}
        }

        Ok(packages)
    }

    /// Pave the various raw images (zbi, bootloaders, vbmeta), and configure the non-current
    /// configuration as active for the next boot.
    async fn stage_images(
        &mut self,
        co: &mut async_generator::Yield<State>,
        state: &mut state::Stage,
        update_pkg: &UpdatePackage,
        mode: UpdateMode,
        current_configuration: paver::CurrentConfiguration,
    ) -> Result<(), Error> {
        let image_list = [
            ImageType::Bootloader,
            ImageType::Firmware,
            ImageType::Zbi,
            ImageType::ZbiSigned,
            ImageType::FuchsiaVbmeta,
            ImageType::Recovery,
            ImageType::RecoveryVbmeta,
        ];

        let images = update_pkg
            .resolve_images(&image_list[..])
            .await
            .context("while determining which images to write")?;

        let images = images
            .verify(mode)
            .context("while ensuring the target images are compatible with this update mode")?
            .filter(|image| {
                if self.config.should_write_recovery {
                    true
                } else {
                    if image.classify().targets_recovery() {
                        fx_log_info!("Skipping recovery image: {}", image.name());
                        false
                    } else {
                        true
                    }
                }
            });

        fx_log_info!("Images to write: {:?}", images);

        let desired_config = current_configuration.to_non_current_configuration();
        fx_log_info!("Targeting configuration: {:?}", desired_config);

        write_images(&self.env.data_sink, &update_pkg, desired_config, images.iter()).await?;
        match mode {
            UpdateMode::Normal => {
                let () =
                    paver::set_configuration_active(&self.env.boot_manager, desired_config).await?;
            }
            UpdateMode::ForceRecovery => {
                let () = paver::set_recovery_configuration_active(&self.env.boot_manager).await?;
            }
        }
        let () = paver::flush(&self.env.data_sink, &self.env.boot_manager, desired_config).await?;

        state.add_progress(co, 1).await;

        Ok(())
    }
}

async fn write_images<'a, I>(
    data_sink: &DataSinkProxy,
    update_pkg: &UpdatePackage,
    desired_config: paver::NonCurrentConfiguration,
    images: I,
) -> Result<(), Error>
where
    I: Iterator<Item = &'a Image>,
{
    for image in images {
        paver::write_image(data_sink, update_pkg, image, desired_config)
            .await
            .context("while writing images")?;
    }
    Ok(())
}

async fn sync_package_cache(pkg_cache: &PackageCacheProxy) -> Result<(), Error> {
    async move {
        pkg_cache
            .sync()
            .await
            .context("while performing sync call")?
            .map_err(fuchsia_zircon::Status::from_raw)
            .context("sync responded with")
    }
    .await
    .context("while flushing packages to persistent storage")
}

async fn gc(space_manager: &SpaceManagerProxy) -> Result<(), Error> {
    let () = space_manager
        .gc()
        .await
        .context("while performing gc call")?
        .map_err(|e| anyhow!("garbage collection responded with {:?}", e))?;
    Ok(())
}

/// Resolve the update package, incorporating an increasingly aggressive GC and retry strategy.
async fn resolve_update_package(
    pkg_resolver: &PackageResolverProxy,
    update_url: &PkgUrl,
    space_manager: &SpaceManagerProxy,
    retained_packages: &RetainedPackagesProxy,
) -> Result<UpdatePackage, ResolveError> {
    // First, attempt to resolve the update package.
    match resolver::resolve_update_package(pkg_resolver, update_url).await {
        Ok(update_pkg) => return Ok(update_pkg),
        Err(ResolveError::Error(fidl_fuchsia_pkg_ext::ResolveError::NoSpace, _)) => (),
        Err(e) => return Err(e),
    }

    // If the first attempt fails with NoSpace, perform a GC and retry.
    if let Err(e) = gc(space_manager).await {
        fx_log_err!("unable to gc packages before first resolve retry: {:#}", anyhow!(e));
    }
    match resolver::resolve_update_package(pkg_resolver, update_url).await {
        Ok(update_pkg) => return Ok(update_pkg),
        Err(ResolveError::Error(fidl_fuchsia_pkg_ext::ResolveError::NoSpace, _)) => (),
        Err(e) => return Err(e),
    }

    // If the second attempt fails with NoSpace, remove packages we aren't sure we need from the
    // retained packages set, perform a GC and retry. If the third attempt fails,
    // return the error regardless of type.
    let () = async {
        if let Some(hash) = update_url.package_hash() {
            let () = replace_retained_packages(std::iter::once(*hash), retained_packages)
                .await
                .context("serve_blob_id_iterator")?;
        } else {
            let () = retained_packages.clear().await.context("calling RetainedPackages.Clear")?;
        }
        Ok(())
    }
    .await
    .unwrap_or_else(|e: anyhow::Error| {
        fx_log_err!(
            "while resolving update package, unable to minimize retained packages set before \
             second gc attempt: {:#}",
            anyhow!(e)
        )
    });

    if let Err(e) = gc(space_manager).await {
        fx_log_err!("unable to gc packages before second resolve retry: {:#}", anyhow!(e));
    }
    resolver::resolve_update_package(pkg_resolver, update_url).await
}

async fn verify_board<B>(build_info: &B, pkg: &UpdatePackage) -> Result<(), Error>
where
    B: BuildInfo,
{
    let current_board = build_info.board().await.context("while determining current board")?;
    if let Some(current_board) = current_board {
        let () = pkg.verify_board(&current_board).await.context("while verifying target board")?;
    }
    Ok(())
}

async fn update_mode(
    pkg: &UpdatePackage,
) -> Result<UpdateMode, update_package::ParseUpdateModeError> {
    pkg.update_mode().await.map(|opt| {
        opt.unwrap_or_else(|| {
            let mode = UpdateMode::default();
            fx_log_info!("update-mode file not found, using default mode: {:?}", mode);
            mode
        })
    })
}

/// Verify that epoch is non-decreasing. For more context, see
/// [RFC-0071](https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0071_ota_backstop).
async fn validate_epoch(source_epoch_raw: &str, pkg: &UpdatePackage) -> Result<(), PrepareError> {
    let src = match serde_json::from_str(source_epoch_raw)
        .map_err(|e| PrepareError::ParseSourceEpochError(source_epoch_raw.to_string(), e))?
    {
        EpochFile::Version1 { epoch } => epoch,
    };
    let target =
        pkg.epoch().await.map_err(PrepareError::ParseTargetEpochError)?.unwrap_or_else(|| {
            fx_log_info!("no epoch in update package, assuming it's 0");
            0
        });
    if target < src {
        return Err(PrepareError::UnsupportedDowngrade { src, target });
    }
    Ok(())
}

async fn replace_retained_packages(
    hashes: impl IntoIterator<Item = fuchsia_hash::Hash>,
    retained_packages: &RetainedPackagesProxy,
) -> Result<(), anyhow::Error> {
    let (client_end, stream) =
        fidl::endpoints::create_request_stream().context("creating request stream")?;
    let replace_resp = retained_packages.replace(client_end);
    let () = fidl_fuchsia_pkg_ext::serve_fidl_iterator(
        hashes
            .into_iter()
            .map(|hash| fidl_fuchsia_pkg_ext::BlobId::from(hash).into())
            .collect::<Vec<_>>(),
        stream,
    )
    .await
    .unwrap_or_else(|e| {
        fx_log_err!(
            "error serving {} protocol: {:#}",
            RetainedPackagesMarker::DEBUG_NAME,
            anyhow!(e)
        )
    });
    replace_resp.await.context("calling RetainedPackages.Replace")
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        assert_matches::assert_matches,
        fuchsia_async as fasync,
        fuchsia_pkg_testing::{make_epoch_json, TestUpdatePackage},
    };

    // Simulate the cobalt test hanging indefinitely, and ensure we time out correctly.
    // This test deliberately logs an error.
    #[fasync::run_singlethreaded(test)]
    async fn flush_cobalt_succeeds_when_cobalt_hangs() {
        let hung_task = futures::future::pending();
        flush_cobalt(hung_task, Duration::from_secs(2)).await;
    }

    #[fasync::run_singlethreaded(test)]
    async fn validate_epoch_success() {
        let source = make_epoch_json(1);
        let target = make_epoch_json(2);
        let p = TestUpdatePackage::new().add_file("epoch.json", target).await;

        let res = validate_epoch(&source, &p).await;

        assert_matches!(res, Ok(()));
    }

    #[fasync::run_singlethreaded(test)]
    async fn validate_epoch_fail_unsupported_downgrade() {
        let source = make_epoch_json(2);
        let target = make_epoch_json(1);
        let p = TestUpdatePackage::new().add_file("epoch.json", target).await;

        let res = validate_epoch(&source, &p).await;

        assert_matches!(res, Err(PrepareError::UnsupportedDowngrade { src: 2, target: 1 }));
    }

    #[fasync::run_singlethreaded(test)]
    async fn validate_epoch_fail_parse_source() {
        let p = TestUpdatePackage::new().add_file("epoch.json", make_epoch_json(1)).await;

        let res = validate_epoch("invalid source epoch.json", &p).await;

        assert_matches!(
            res,
            Err(PrepareError::ParseSourceEpochError(s, _)) if s == "invalid source epoch.json"
        );
    }

    #[fasync::run_singlethreaded(test)]
    async fn validate_epoch_fail_parse_target() {
        let p = TestUpdatePackage::new()
            .add_file("epoch.json", "invalid target epoch.json".to_string())
            .await;

        let res = validate_epoch(&make_epoch_json(1), &p).await;

        assert_matches!(res, Err(PrepareError::ParseTargetEpochError(_)));
    }

    #[fasync::run_singlethreaded(test)]
    async fn validate_epoch_target_defaults_to_zero() {
        let p = TestUpdatePackage::new();

        assert_matches!(
            validate_epoch(&make_epoch_json(1), &p).await,
            Err(PrepareError::UnsupportedDowngrade { src: 1, target: 0 })
        );
    }
}
