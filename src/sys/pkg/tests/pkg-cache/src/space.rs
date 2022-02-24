// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{get_missing_blobs, write_blob, TestEnv},
    assert_matches::assert_matches,
    blobfs_ramdisk::{BlobfsRamdisk, Ramdisk},
    fidl_fuchsia_io::{DirectoryMarker, FileMarker},
    fidl_fuchsia_paver as paver,
    fidl_fuchsia_pkg::{BlobInfo, NeededBlobsMarker, PackageCacheProxy},
    fidl_fuchsia_pkg_ext::BlobId,
    fidl_fuchsia_space::ErrorCode,
    fuchsia_async::{self as fasync, OnSignals},
    fuchsia_pkg_testing::{Package, PackageBuilder, SystemImageBuilder},
    fuchsia_zircon::{self as zx, Status},
    futures::TryFutureExt,
    mock_paver::{hooks as mphooks, MockPaverServiceBuilder, PaverEvent},
    pkgfs_ramdisk::PkgfsRamdisk,
    rand::prelude::*,
    std::collections::{BTreeSet, HashMap},
    std::io::Read,
};

// TODO(fxbug.dev/76724): Deduplicate this function.
async fn do_fetch(package_cache: &PackageCacheProxy, pkg: &Package) {
    let mut meta_blob_info =
        BlobInfo { blob_id: BlobId::from(*pkg.meta_far_merkle_root()).into(), length: 0 };

    let (needed_blobs, needed_blobs_server_end) =
        fidl::endpoints::create_proxy::<NeededBlobsMarker>().unwrap();
    let (dir, dir_server_end) = fidl::endpoints::create_proxy::<DirectoryMarker>().unwrap();
    let get_fut = package_cache
        .get(&mut meta_blob_info, needed_blobs_server_end, Some(dir_server_end))
        .map_ok(|res| res.map_err(Status::from_raw));

    let (meta_far, contents) = pkg.contents();
    let mut contents = contents
        .into_iter()
        .map(|blob| (BlobId::from(blob.merkle), blob.contents))
        .collect::<HashMap<_, Vec<u8>>>();

    let (meta_blob, meta_blob_server_end) = fidl::endpoints::create_proxy::<FileMarker>().unwrap();
    let res = needed_blobs.open_meta_blob(meta_blob_server_end).await.unwrap().unwrap();
    assert!(res);
    let () = write_blob(&meta_far.contents, meta_blob).await.unwrap();

    let missing_blobs = get_missing_blobs(&needed_blobs).await;
    for mut blob in missing_blobs {
        let buf = contents.remove(&blob.blob_id.into()).unwrap();

        let (content_blob, content_blob_server_end) =
            fidl::endpoints::create_proxy::<FileMarker>().unwrap();
        assert!(needed_blobs
            .open_blob(&mut blob.blob_id, content_blob_server_end)
            .await
            .unwrap()
            .unwrap());

        let () = write_blob(&buf, content_blob).await.unwrap();
    }

    let () = get_fut.await.unwrap().unwrap();
    let () = pkg.verify_contents(&dir).await.unwrap();
}

#[fasync::run_singlethreaded(test)]
async fn gc_error_pending_commit() {
    let (throttle_hook, throttler) = mphooks::throttle();

    let blobfs = BlobfsRamdisk::start().unwrap();
    let system_image_package = SystemImageBuilder::new().build().await;
    system_image_package.write_to_blobfs_dir(&blobfs.root_dir().unwrap());
    let pkgfs = PkgfsRamdisk::builder()
        .blobfs(blobfs)
        .system_image_merkle(system_image_package.meta_far_merkle_root())
        .start()
        .unwrap();

    let env = TestEnv::builder()
        .pkgfs(pkgfs)
        .paver_service_builder(
            MockPaverServiceBuilder::new()
                .insert_hook(throttle_hook)
                .insert_hook(mphooks::config_status(|_| Ok(paver::ConfigurationStatus::Pending))),
        )
        .build()
        .await;

    // Allow the paver to emit enough events to unblock the CommitStatusProvider FIDL server, but
    // few enough to guarantee the commit is still pending.
    let () = throttler.emit_next_paver_events(&[
        PaverEvent::QueryCurrentConfiguration,
        PaverEvent::QueryConfigurationStatus { configuration: paver::Configuration::A },
    ]);
    assert_matches!(env.proxies.space_manager.gc().await, Ok(Err(ErrorCode::PendingCommit)));

    // When the commit completes, GC should unblock as well.
    let () = throttler.emit_next_paver_events(&[
        PaverEvent::SetConfigurationHealthy { configuration: paver::Configuration::A },
        PaverEvent::SetConfigurationUnbootable { configuration: paver::Configuration::B },
        PaverEvent::BootManagerFlush,
    ]);
    let event_pair =
        env.proxies.commit_status_provider.is_current_system_committed().await.unwrap();
    assert_eq!(OnSignals::new(&event_pair, zx::Signals::USER_0).await, Ok(zx::Signals::USER_0));
    assert_matches!(env.proxies.space_manager.gc().await, Ok(Ok(())));
}

/// Sets up the test environment and writes the packages out to base.
async fn setup_test_env(
    blobfs: Option<BlobfsRamdisk>,
    static_packages: &[&Package],
) -> (TestEnv, Package) {
    let blobfs = match blobfs {
        Some(fs) => fs,
        None => BlobfsRamdisk::start().unwrap(),
    };
    let system_image_package = SystemImageBuilder::new().static_packages(static_packages);
    let system_image_package = system_image_package.build().await;
    system_image_package.write_to_blobfs_dir(&blobfs.root_dir().unwrap());
    for pkg in static_packages {
        pkg.write_to_blobfs_dir(&blobfs.root_dir().unwrap());
    }
    let pkgfs = PkgfsRamdisk::builder()
        .blobfs(blobfs)
        .system_image_merkle(system_image_package.meta_far_merkle_root())
        .start()
        .unwrap();

    let env = TestEnv::builder().pkgfs(pkgfs).build().await;
    env.block_until_started().await;
    (env, system_image_package)
}

/// Assert that performing a GC does nothing on a blobfs that only includes the system image and
/// static packages.
#[fasync::run_singlethreaded(test)]
async fn gc_noop_system_image() {
    let static_package = PackageBuilder::new("static-package")
        .add_resource_at("resource", &[][..])
        .build()
        .await
        .unwrap();
    let (env, _) = setup_test_env(None, &[&static_package]).await;
    let original_blobs = env.blobfs().list_blobs().expect("to get an initial list of blobs");

    assert_matches!(env.proxies.space_manager.gc().await, Ok(Ok(())));
    assert_eq!(env.blobfs().list_blobs().expect("to get new blobs"), original_blobs);
}

/// Assert that any blobs protected by the dynamic index are ineligible for garbage collection.
/// Furthermore, ensure that an incomplete package does not lose blobs, and that the previous
/// packages' blobs survive until the new package is entirely written.
#[fasync::run_singlethreaded(test)]
async fn gc_dynamic_index_protected() {
    let (env, sysimg_pkg) = setup_test_env(None, &[]).await;

    let pkg = PackageBuilder::new("gc_dynamic_index_protected_pkg_cache")
        .add_resource_at("bin/x", "bin-x-version-1".as_bytes())
        .add_resource_at("data/unchanging", "unchanging-content".as_bytes())
        .build()
        .await
        .unwrap();
    do_fetch(&env.proxies.package_cache, &pkg).await;

    // Ensure that the just-fetched blobs are not reaped by a GC cycle.
    let mut test_blobs = env.blobfs().list_blobs().expect("to get an initial list of blobs");

    assert_matches!(env.proxies.space_manager.gc().await, Ok(Ok(())));
    assert_eq!(env.blobfs().list_blobs().expect("to get new blobs"), test_blobs);

    // Fetch an updated package, skipping both its content blobs to guarantee that there are
    // missing blobs. This helps us ensure that the meta.far is not lost.
    let pkgprime = PackageBuilder::new("gc_dynamic_index_protected_pkg_cache")
        .add_resource_at("bin/x", "bin-x-version-2".as_bytes())
        .add_resource_at("bin/y", "bin-y-version-1".as_bytes())
        .add_resource_at("data/unchanging", "unchanging-content".as_bytes())
        .build()
        .await
        .unwrap();

    // We can't call do_fetch here because pkg-cache differs from pkgfs in that the NeededBlobs
    // protocol can be "canceled". This means that if the channel is closed before the protocol is
    // completed, the blobs mentioned in the meta far are no longer protected by the dynamic index.
    // That's WAI, but complicating that interface further isn't worth it.
    //
    // Here, we persist the meta.far
    let mut meta_blob_info =
        BlobInfo { blob_id: BlobId::from(*pkgprime.meta_far_merkle_root()).into(), length: 0 };
    let package_cache = &env.proxies.package_cache;

    let (needed_blobs, needed_blobs_server_end) =
        fidl::endpoints::create_proxy::<NeededBlobsMarker>().unwrap();
    let (dir, dir_server_end) = fidl::endpoints::create_proxy::<DirectoryMarker>().unwrap();
    let get_fut = package_cache
        .get(&mut meta_blob_info, needed_blobs_server_end, Some(dir_server_end))
        .map_ok(|res| res.map_err(Status::from_raw));

    let (meta_far, contents) = pkgprime.contents();
    let mut contents = contents
        .into_iter()
        .map(|blob| (BlobId::from(blob.merkle), blob.contents))
        .collect::<HashMap<_, Vec<u8>>>();

    let (meta_blob, meta_blob_server_end) = fidl::endpoints::create_proxy::<FileMarker>().unwrap();
    let res = needed_blobs.open_meta_blob(meta_blob_server_end).await.unwrap().unwrap();
    assert!(res);
    let () = write_blob(&meta_far.contents, meta_blob).await.unwrap();

    // Ensure that the new meta.far is persisted despite having missing blobs, and the "old" blobs
    // are not removed.
    assert_matches!(env.proxies.space_manager.gc().await, Ok(Ok(())));
    test_blobs.insert(*pkgprime.meta_far_merkle_root());
    assert_eq!(env.blobfs().list_blobs().expect("to get new blobs"), test_blobs);

    // Fully fetch pkgprime, and ensure that blobs from the old package are not persisted past GC.
    let missing_blobs = get_missing_blobs(&needed_blobs).await;
    for mut blob in missing_blobs {
        let buf = contents.remove(&blob.blob_id.into()).unwrap();

        let (content_blob, content_blob_server_end) =
            fidl::endpoints::create_proxy::<FileMarker>().unwrap();
        assert!(needed_blobs
            .open_blob(&mut blob.blob_id, content_blob_server_end)
            .await
            .unwrap()
            .unwrap());

        let () = write_blob(&buf, content_blob).await.unwrap();

        // Run a GC to try to reap blobs protected by meta far.
        assert_matches!(env.proxies.space_manager.gc().await, Ok(Ok(())));
    }

    let () = get_fut.await.unwrap().unwrap();
    let () = pkgprime.verify_contents(&dir).await.unwrap();
    assert_matches!(env.proxies.space_manager.gc().await, Ok(Ok(())));

    // At this point, we expect blobfs to only contain the blobs from the system image package and
    // from pkgprime.
    let expected_blobs = sysimg_pkg
        .list_blobs()
        .unwrap()
        .union(&pkgprime.list_blobs().unwrap())
        .cloned()
        .collect::<BTreeSet<_>>();

    assert_eq!(env.blobfs().list_blobs().expect("all blobs"), expected_blobs);
}

/// Test that a blobfs with blobs not belonging to a known package will lose those blobs on GC.
#[fasync::run_singlethreaded(test)]
async fn gc_random_blobs() {
    let static_package = PackageBuilder::new("static-package")
        .add_resource_at("resource", &[][..])
        .build()
        .await
        .unwrap();
    let blobfs = BlobfsRamdisk::builder()
        .with_blob(b"blobby mcblobberson".to_vec())
        .start()
        .expect("blobfs creation to succeed with stray blob");
    let gced_blob = blobfs
        .list_blobs()
        .expect("to find initial blob")
        .into_iter()
        .next()
        .expect("to get initial blob");
    let (env, _) = setup_test_env(Some(blobfs), &[&static_package]).await;
    let mut original_blobs = env.blobfs().list_blobs().expect("to get an initial list of blobs");

    assert_matches!(env.proxies.space_manager.gc().await, Ok(Ok(())));
    assert!(original_blobs.remove(&gced_blob));
    assert_eq!(env.blobfs().list_blobs().expect("to read current blobfs state"), original_blobs);
}

/// Effectively the same as gc_dynamic_index_protected, except that the updated package also
/// existed as a static package as well.
#[fasync::run_singlethreaded(test)]
async fn gc_updated_static_package() {
    let static_package = PackageBuilder::new("gc_updated_static_package_pkg_cache")
        .add_resource_at("bin/x", "bin-x-version-0".as_bytes())
        .add_resource_at("data/unchanging", "unchanging-content".as_bytes())
        .build()
        .await
        .unwrap();

    let (env, _) = setup_test_env(None, &[&static_package]).await;
    let initial_blobs = env.blobfs().list_blobs().expect("to get initial blob list");

    let pkg = PackageBuilder::new("gc_updated_static_package_pkg_cache")
        .add_resource_at("bin/x", "bin-x-version-1".as_bytes())
        .add_resource_at("data/unchanging", "unchanging-content".as_bytes())
        .build()
        .await
        .unwrap();
    do_fetch(&env.proxies.package_cache, &pkg).await;

    // Ensure that the just-fetched blobs are not reaped by a GC cycle.
    let mut test_blobs = env.blobfs().list_blobs().expect("to get an initial list of blobs");

    assert_matches!(env.proxies.space_manager.gc().await, Ok(Ok(())));
    assert_eq!(env.blobfs().list_blobs().expect("to get new blobs"), test_blobs);

    let pkgprime = PackageBuilder::new("gc_updated_static_package_pkg_cache")
        .add_resource_at("bin/x", "bin-x-version-2".as_bytes())
        .add_resource_at("bin/y", "bin-y-version-1".as_bytes())
        .add_resource_at("data/unchanging", "unchanging-content".as_bytes())
        .build()
        .await
        .unwrap();
    // We can't call do_fetch here because pkg-cache differs from pkgfs in that the NeededBlobs
    // protocol can be "canceled". This means that if the channel is closed before the protocol is
    // completed, the blobs mentioned in the meta far are no longer protected by the dynamic index.
    // That's WAI, but complicating that interface further isn't worth it.
    //
    // Here, we persist the meta.far
    let mut meta_blob_info =
        BlobInfo { blob_id: BlobId::from(*pkgprime.meta_far_merkle_root()).into(), length: 0 };
    let package_cache = &env.proxies.package_cache;

    let (needed_blobs, needed_blobs_server_end) =
        fidl::endpoints::create_proxy::<NeededBlobsMarker>().unwrap();
    let (dir, dir_server_end) = fidl::endpoints::create_proxy::<DirectoryMarker>().unwrap();
    let get_fut = package_cache
        .get(&mut meta_blob_info, needed_blobs_server_end, Some(dir_server_end))
        .map_ok(|res| res.map_err(Status::from_raw));

    let (meta_far, contents) = pkgprime.contents();
    let mut contents = contents
        .into_iter()
        .map(|blob| (BlobId::from(blob.merkle), blob.contents))
        .collect::<HashMap<_, Vec<u8>>>();

    let (meta_blob, meta_blob_server_end) = fidl::endpoints::create_proxy::<FileMarker>().unwrap();
    let res = needed_blobs.open_meta_blob(meta_blob_server_end).await.unwrap().unwrap();
    assert!(res);
    let () = write_blob(&meta_far.contents, meta_blob).await.unwrap();

    // Ensure that the new meta.far is persisted despite having missing blobs, and the "old" blobs
    // are not removed.
    assert_matches!(env.proxies.space_manager.gc().await, Ok(Ok(())));
    test_blobs.insert(*pkgprime.meta_far_merkle_root());
    assert_eq!(env.blobfs().list_blobs().expect("to get new blobs"), test_blobs);

    // Fully fetch pkgprime, and ensure that blobs from the old package are not persisted past GC.
    let missing_blobs = get_missing_blobs(&needed_blobs).await;
    for mut blob in missing_blobs {
        let buf = contents.remove(&blob.blob_id.into()).unwrap();

        let (content_blob, content_blob_server_end) =
            fidl::endpoints::create_proxy::<FileMarker>().unwrap();
        assert!(needed_blobs
            .open_blob(&mut blob.blob_id, content_blob_server_end)
            .await
            .unwrap()
            .unwrap());

        let () = write_blob(&buf, content_blob).await.unwrap();

        // Run a GC to try to reap blobs protected by meta far.
        assert_matches!(env.proxies.space_manager.gc().await, Ok(Ok(())));
    }

    let () = get_fut.await.unwrap().unwrap();
    let () = pkgprime.verify_contents(&dir).await.unwrap();
    assert_matches!(env.proxies.space_manager.gc().await, Ok(Ok(())));

    // At this point, we expect blobfs to only contain the blobs from the system image package and
    // from pkgprime.
    let expected_blobs =
        initial_blobs.union(&pkgprime.list_blobs().unwrap()).cloned().collect::<BTreeSet<_>>();

    assert_eq!(env.blobfs().list_blobs().expect("all blobs"), expected_blobs);
}

#[fuchsia_async::run_singlethreaded(test)]
async fn blob_write_fails_when_out_of_space() {
    let system_image_package = SystemImageBuilder::new().build().await;

    // Create a 2MB blobfs (4096 blocks * 512 bytes / block), which is about the minimum size blobfs
    // will fit in and still be able to write our small package.
    // See https://fuchsia.dev/fuchsia-src/concepts/filesystems/blobfs for information on the
    // blobfs format and metadata overhead.
    let very_small_blobfs = Ramdisk::builder()
        .block_count(4096)
        .into_blobfs_builder()
        .expect("made blobfs builder")
        .start()
        .expect("started blobfs");
    system_image_package
        .write_to_blobfs_dir(&very_small_blobfs.root_dir().expect("wrote system image to blobfs"));

    let pkgfs = PkgfsRamdisk::builder()
        .blobfs(very_small_blobfs)
        .system_image_merkle(system_image_package.meta_far_merkle_root())
        .start()
        .expect("started pkgfs");

    // A very large version of the same package, to put in the repo.
    // Critically, this package contains an incompressible 4MB asset in the meta.far,
    // which is larger than our blobfs, and attempting to resolve this package will result in
    // blobfs returning out of space.
    const LARGE_ASSET_FILE_SIZE: u64 = 2 * 1024 * 1024;
    let mut rng = StdRng::from_seed([0u8; 32]);
    let rng = &mut rng as &mut dyn RngCore;
    let pkg = PackageBuilder::new("pkg-a")
        .add_resource_at("meta/asset", rng.take(LARGE_ASSET_FILE_SIZE))
        .build()
        .await
        .expect("build large package");

    // The size of the meta far should be the size of our asset, plus two 4k-aligned files:
    // meta/package
    // Content chunks in FARs are 4KiB-aligned, so the most empty FAR we can get is 8KiB:
    // meta/package at one alignment boundary, and meta/contents is empty.
    // This FAR should be 8KiB + the size of our asset file.
    assert_eq!(
        pkg.meta_far().unwrap().metadata().unwrap().len(),
        LARGE_ASSET_FILE_SIZE + 4096 + 4096
    );

    let env = TestEnv::builder().pkgfs(pkgfs).build().await;

    let mut meta_blob_info =
        BlobInfo { blob_id: BlobId::from(*pkg.meta_far_merkle_root()).into(), length: 0 };

    let (needed_blobs, needed_blobs_server_end) =
        fidl::endpoints::create_proxy::<NeededBlobsMarker>().unwrap();
    let (_dir, dir_server_end) = fidl::endpoints::create_proxy::<DirectoryMarker>().unwrap();
    let _get_fut = env
        .proxies
        .package_cache
        .get(&mut meta_blob_info, needed_blobs_server_end, Some(dir_server_end))
        .map_ok(|res| res.map_err(Status::from_raw));

    let (meta_blob, meta_blob_server_end) = fidl::endpoints::create_proxy::<FileMarker>().unwrap();
    let res = needed_blobs.open_meta_blob(meta_blob_server_end).await.unwrap().unwrap();
    assert!(res);

    let (meta_far, _contents) = pkg.contents();
    assert_eq!(write_blob(&meta_far.contents, meta_blob).await, Err(Status::NO_SPACE));
}
