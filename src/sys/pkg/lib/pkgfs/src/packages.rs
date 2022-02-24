// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Typesafe wrappers around the /pkgfs/packages filesystem.

use {
    fidl_fuchsia_io::{DirectoryMarker, DirectoryProxy, DirectoryRequestStream},
    fuchsia_zircon::Status,
    thiserror::Error,
};

/// An error encountered while opening a package
#[derive(Debug, Error)]
#[allow(missing_docs)]
pub enum OpenError {
    #[error("the package does not exist")]
    NotFound,

    #[error("while opening the package")]
    Io(#[source] io_util::node::OpenError),
}

/// An open handle to /pkgfs/packages
#[derive(Debug, Clone)]
pub struct Client {
    proxy: DirectoryProxy,
}

impl Client {
    /// Returns an client connected to pkgfs from the current component's namespace
    pub fn open_from_namespace() -> Result<Self, io_util::node::OpenError> {
        let proxy = io_util::directory::open_in_namespace(
            "/pkgfs/packages",
            fidl_fuchsia_io::OPEN_RIGHT_READABLE,
        )?;
        Ok(Client { proxy })
    }

    /// Returns an client connected to pkgfs from the given pkgfs root dir.
    pub fn open_from_pkgfs_root(pkgfs: &DirectoryProxy) -> Result<Self, io_util::node::OpenError> {
        Ok(Client {
            proxy: io_util::directory::open_directory_no_describe(
                pkgfs,
                "packages",
                fidl_fuchsia_io::OPEN_RIGHT_READABLE,
            )?,
        })
    }

    /// Creates a new client backed by the returned request stream. This constructor should not be
    /// used outside of tests.
    ///
    /// # Panics
    ///
    /// Panics on error
    pub fn new_test() -> (Self, DirectoryRequestStream) {
        let (proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<DirectoryMarker>().unwrap();

        (Self { proxy }, stream)
    }

    /// Open the package given by `name` and `variant` (defaults to "0"). Verifies the OnOpen event
    /// before returning.
    pub async fn open_package(
        &self,
        name: &str,
        variant: Option<&str>,
    ) -> Result<fuchsia_pkg::PackageDirectory, OpenError> {
        // TODO(fxbug.dev/37858) allow opening as executable too
        let flags = fidl_fuchsia_io::OPEN_RIGHT_READABLE;
        let path = format!("{}/{}", name, variant.unwrap_or("0"));
        let dir = io_util::directory::open_directory(&self.proxy, &path, flags).await.map_err(
            |e| match e {
                io_util::node::OpenError::OpenError(Status::NOT_FOUND) => OpenError::NotFound,
                other => OpenError::Io(other),
            },
        )?;

        Ok(fuchsia_pkg::PackageDirectory::from_proxy(dir))
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*, crate::install::BlobKind, assert_matches::assert_matches,
        fuchsia_pkg_testing::PackageBuilder, pkgfs_ramdisk::PkgfsRamdisk,
    };

    #[fuchsia_async::run_singlethreaded(test)]
    async fn open_non_existant_package_fails() {
        let pkgfs = PkgfsRamdisk::start().unwrap();
        let root = pkgfs.root_dir_proxy().unwrap();
        let client = Client::open_from_pkgfs_root(&root).unwrap();

        assert_matches!(client.open_package("fake", None).await, Err(OpenError::NotFound));
        assert_matches!(
            client.open_package("fake", Some("package")).await,
            Err(OpenError::NotFound)
        );

        pkgfs.stop().await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn open_package_single_blob() {
        let pkgfs =
            PkgfsRamdisk::builder().enforce_packages_non_static_allowlist(false).start().unwrap();
        let root = pkgfs.root_dir_proxy().unwrap();
        let install = crate::install::Client::open_from_pkgfs_root(&root).unwrap();
        let client = Client::open_from_pkgfs_root(&root).unwrap();

        let pkg = PackageBuilder::new("uniblob").build().await.unwrap();
        install.write_meta_far(&pkg).await;

        assert_matches!(client.open_package("uniblob", None).await, Ok(_));
        assert_matches!(
            pkg.verify_contents(
                &client.open_package("uniblob", Some("0")).await.unwrap().into_proxy()
            )
            .await,
            Ok(())
        );

        pkgfs.stop().await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn open_package_multiple_blobs() {
        let pkgfs =
            PkgfsRamdisk::builder().enforce_packages_non_static_allowlist(false).start().unwrap();
        let root = pkgfs.root_dir_proxy().unwrap();
        let install = crate::install::Client::open_from_pkgfs_root(&root).unwrap();
        let client = Client::open_from_pkgfs_root(&root).unwrap();

        let pkg = PackageBuilder::new("multiblob")
            .add_resource_at("data/first", "contents of first blob".as_bytes())
            .add_resource_at("data/second", "contents of second blob".as_bytes())
            .build()
            .await
            .unwrap();
        let pkg_contents = pkg.meta_contents().unwrap().contents().to_owned();
        install.write_meta_far(&pkg).await;

        // Package is not complete yet, so opening fails.
        assert_matches!(client.open_package("multiblob", None).await, Err(OpenError::NotFound));

        install
            .write_blob(
                pkg_contents["data/first"],
                BlobKind::Data,
                "contents of first blob".as_bytes(),
            )
            .await;
        install
            .write_blob(
                pkg_contents["data/second"],
                BlobKind::Data,
                "contents of second blob".as_bytes(),
            )
            .await;

        assert_matches!(client.open_package("multiblob", None).await, Ok(_));
        assert_matches!(
            pkg.verify_contents(
                &client.open_package("multiblob", Some("0")).await.unwrap().into_proxy()
            )
            .await,
            Ok(())
        );

        pkgfs.stop().await.unwrap();
    }
}
