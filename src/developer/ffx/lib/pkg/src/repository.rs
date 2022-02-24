// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{anyhow, Context, Result},
    async_lock::Mutex as AsyncMutex,
    bytes::Bytes,
    camino::Utf8PathBuf,
    fidl_fuchsia_developer_bridge::{ListFields, PackageEntry, RepositoryPackage},
    fidl_fuchsia_developer_bridge_ext::{RepositorySpec, RepositoryStorageType},
    fidl_fuchsia_pkg as pkg,
    fuchsia_archive::AsyncReader,
    fuchsia_pkg::MetaContents,
    futures::{
        future::{join_all, ready, try_join_all},
        io::Cursor,
        stream::{once, BoxStream},
        AsyncReadExt, StreamExt as _,
    },
    io_util::file::Adapter,
    parking_lot::Mutex as SyncMutex,
    serde::{Deserialize, Serialize},
    std::{
        convert::TryFrom,
        io,
        sync::{
            atomic::{AtomicUsize, Ordering},
            Arc,
        },
        time::SystemTime,
    },
    tuf::{
        client::{Client, Config},
        crypto::KeyType,
        interchange::Json,
        metadata::{
            Metadata as _, MetadataPath, MetadataVersion, RawSignedMetadata, Role,
            TargetDescription, TargetPath, TargetsMetadata,
        },
        repository::{EphemeralRepository, RepositoryProvider},
        verify::Verified,
    },
    url::ParseError,
};

mod file_system;
mod manager;
mod pm;
mod server;

pub mod http_repository;

pub use file_system::FileSystemRepository;
pub use http_repository::{package_download, HttpRepository};
pub use manager::RepositoryManager;
pub use pm::PmRepository;
pub use server::{ConnectionStream, RepositoryServer, RepositoryServerBuilder};

/// A unique ID which is given to every repository.
#[derive(Copy, Clone, Debug, PartialEq, Eq, Hash)]
pub struct RepositoryId(usize);

impl RepositoryId {
    fn new() -> Self {
        static NEXT_ID: AtomicUsize = AtomicUsize::new(0);
        RepositoryId(NEXT_ID.fetch_add(1, Ordering::Relaxed))
    }
}

/// The below types exist to provide definitions with Serialize.
/// TODO(fxbug.dev/76041) They should be removed in favor of the
/// corresponding fidl-fuchsia-pkg-ext types.
#[derive(Debug, Deserialize, PartialEq, Serialize)]
pub struct RepositoryConfig {
    pub repo_url: Option<String>,
    pub root_keys: Option<Vec<RepositoryKeyConfig>>,
    pub root_version: Option<u32>,
    pub mirrors: Option<Vec<MirrorConfig>>,
    pub storage_type: Option<RepositoryStorageType>,
}

impl From<RepositoryConfig> for pkg::RepositoryConfig {
    fn from(repo_config: RepositoryConfig) -> Self {
        pkg::RepositoryConfig {
            repo_url: repo_config.repo_url,
            root_keys: repo_config.root_keys.map(|v| v.into_iter().map(|r| r.into()).collect()),
            root_version: repo_config.root_version,
            mirrors: repo_config.mirrors.map(|v| v.into_iter().map(|m| m.into()).collect()),
            storage_type: repo_config.storage_type.map(|v| match v {
                RepositoryStorageType::Ephemeral => pkg::RepositoryStorageType::Ephemeral,
                RepositoryStorageType::Persistent => pkg::RepositoryStorageType::Persistent,
            }),
            ..pkg::RepositoryConfig::EMPTY
        }
    }
}

#[derive(Debug, Clone, Deserialize, PartialEq, Serialize)]
pub enum RepositoryKeyConfig {
    /// The raw ed25519 public key as binary data.
    Ed25519Key(Vec<u8>),
}

impl Into<pkg::RepositoryKeyConfig> for RepositoryKeyConfig {
    fn into(self) -> pkg::RepositoryKeyConfig {
        match self {
            Self::Ed25519Key(keys) => pkg::RepositoryKeyConfig::Ed25519Key(keys),
        }
    }
}

#[derive(Debug, Deserialize, PartialEq, Serialize)]
pub struct MirrorConfig {
    /// The base URL of the TUF metadata on this mirror. Required.
    pub mirror_url: Option<String>,
    /// Whether or not to automatically monitor the mirror for updates. Required.
    pub subscribe: Option<bool>,
}

impl Into<pkg::MirrorConfig> for MirrorConfig {
    fn into(self) -> pkg::MirrorConfig {
        pkg::MirrorConfig {
            mirror_url: self.mirror_url,
            subscribe: self.subscribe,
            ..pkg::MirrorConfig::EMPTY
        }
    }
}
#[derive(thiserror::Error, Debug)]
pub enum Error {
    #[error("not found")]
    NotFound,
    #[error("invalid path '{0}'")]
    InvalidPath(Utf8PathBuf),
    #[error("I/O error")]
    Io(#[source] io::Error),
    #[error("URL Parsing Error")]
    URLParseError(#[source] ParseError),
    #[error(transparent)]
    Tuf(#[from] tuf::Error),
    #[error(transparent)]
    Far(#[from] fuchsia_archive::Error),
    #[error(transparent)]
    Meta(#[from] fuchsia_pkg::MetaContentsError),
    #[error(transparent)]
    ParseInt(#[from] std::num::ParseIntError),
    #[error(transparent)]
    ToStr(#[from] hyper::header::ToStrError),
    #[error(transparent)]
    Other(#[from] anyhow::Error),
    #[error("range not satisfiable")]
    RangeNotSatisfiable,
}

impl From<std::io::Error> for Error {
    fn from(err: std::io::Error) -> Self {
        if err.kind() == std::io::ErrorKind::NotFound {
            Error::NotFound
        } else {
            Error::Io(err)
        }
    }
}

impl From<ParseError> for Error {
    fn from(err: ParseError) -> Self {
        Error::URLParseError(err)
    }
}

/// [Resource] represents some resource as a stream of [Bytes] as provided from
/// a repository server.
pub struct Resource {
    /// The length of the file range in bytes.
    pub content_len: u64,

    /// The length of the total file in bytes.
    pub total_len: u64,

    /// A stream of bytes representing the resource.
    pub stream: BoxStream<'static, io::Result<Bytes>>,
}

impl Resource {
    async fn read_to_end(&mut self, buf: &mut Vec<u8>) -> Result<(), Error> {
        buf.reserve(self.content_len as usize);
        while let Some(chunk) = self.stream.next().await.transpose()? {
            buf.extend_from_slice(&chunk);
        }
        Ok(())
    }
}

impl std::fmt::Debug for Resource {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("resource")
            .field("content_len", &self.content_len)
            .field("total_len", &self.total_len)
            .field("stream", &"..")
            .finish()
    }
}

impl TryFrom<RepositoryConfig> for Resource {
    type Error = Error;
    fn try_from(config: RepositoryConfig) -> Result<Resource, Error> {
        let json = Bytes::from(serde_json::to_vec(&config).map_err(|e| anyhow::anyhow!(e))?);
        let len = json.len() as u64;
        Ok(Resource { content_len: len, total_len: len, stream: once(ready(Ok(json))).boxed() })
    }
}

fn is_component_manifest(s: &str) -> bool {
    return s.ends_with(".cm") || s.ends_with(".cmx");
}

pub struct Repository {
    /// The name of the repository.
    name: String,

    /// A unique ID for the repository, scoped to this instance of the daemon.
    id: RepositoryId,

    /// Backend for this repository
    backend: Box<dyn RepositoryBackend + Send + Sync>,

    /// Call these functions upon drop. This is synchronous since it's used in the Drop impl.
    drop_handlers: SyncMutex<Vec<Box<dyn FnOnce() + Send + Sync>>>,

    /// The TUF client for this repository
    client:
        Arc<AsyncMutex<Client<Json, EphemeralRepository<Json>, Box<dyn RepositoryProvider<Json>>>>>,
}

impl Repository {
    pub async fn new(
        name: &str,
        backend: Box<dyn RepositoryBackend + Send + Sync>,
    ) -> Result<Self, Error> {
        let tuf_repo = backend.get_tuf_repo()?;
        let tuf_client = Self::get_tuf_client(tuf_repo).await?;

        Ok(Self {
            name: name.to_string(),
            id: RepositoryId::new(),
            backend,
            client: Arc::new(AsyncMutex::new(tuf_client)),
            drop_handlers: SyncMutex::new(Vec::new()),
        })
    }

    pub fn id(&self) -> RepositoryId {
        self.id
    }

    pub fn repo_url(&self) -> String {
        format!("fuchsia-pkg://{}", self.name)
    }

    /// Stores the given function to be run when the repository is dropped.
    pub fn on_drop<F: FnOnce() + Send + Sync + 'static>(&self, f: F) {
        self.drop_handlers.lock().push(Box::new(f));
    }

    pub fn name(&self) -> &str {
        &self.name
    }

    /// Get a [RepositorySpec] for this [Repository]
    pub fn spec(&self) -> RepositorySpec {
        self.backend.spec()
    }

    /// Returns if the repository supports watching for timestamp changes.
    pub fn supports_watch(&self) -> bool {
        return self.backend.supports_watch();
    }

    /// Return a stream that yields whenever the repository's timestamp changes.
    pub fn watch(&self) -> anyhow::Result<BoxStream<'static, ()>> {
        self.backend.watch()
    }

    /// Return a stream of bytes for the metadata resource.
    pub async fn fetch_metadata(&self, path: &str) -> Result<Resource, Error> {
        self.fetch_metadata_range(path, ResourceRange::RangeFull).await
    }

    /// Return a stream of bytes for the metadata resource in given range.
    pub async fn fetch_metadata_range(
        &self,
        path: &str,
        range: ResourceRange,
    ) -> Result<Resource, Error> {
        self.backend.fetch_metadata(path, range).await
    }

    /// Return a stream of bytes for the blob resource.
    pub async fn fetch_blob(&self, path: &str) -> Result<Resource, Error> {
        self.fetch_blob_range(path, ResourceRange::RangeFull).await
    }

    /// Return a stream of bytes for the blob resource in given range.
    pub async fn fetch_blob_range(
        &self,
        path: &str,
        range: ResourceRange,
    ) -> Result<Resource, Error> {
        self.backend.fetch_blob(path, range).await
    }

    /// Return the target description for a TUF target path.
    pub async fn get_target_description(
        &self,
        path: &str,
    ) -> Result<Option<TargetDescription>, Error> {
        let mut client = Self::get_tuf_client(self.backend.get_tuf_repo()?).await?;
        let _ = client.update().await?;

        match client.database().trusted_targets() {
            Some(trusted_targets) => Ok(trusted_targets
                .targets()
                .get(&TargetPath::new(path).map_err(|e| anyhow::anyhow!(e))?)
                .map(|t| t.clone())),
            None => Ok(None),
        }
    }

    pub async fn get_config(
        &self,
        mirror_url: &str,
        storage_type: Option<RepositoryStorageType>,
    ) -> Result<RepositoryConfig, Error> {
        let trusted_root = {
            let mut client = self.client.lock().await;

            // Update the root metadata to the latest version. We don't care if the other metadata has
            // expired.
            //
            // FIXME: This can be replaced with `client.update_root()` once
            // https://github.com/heartsucker/rust-tuf/pull/318 lands.
            match client.update().await {
                Ok(_) => {}
                Err(err @ tuf::Error::ExpiredMetadata(Role::Root)) => {
                    return Err(err.into());
                }
                Err(tuf::Error::ExpiredMetadata(_)) => {}
                Err(err) => {
                    return Err(err.into());
                }
            }

            client.database().trusted_root().clone()
        };

        let root_keys = trusted_root
            .root_keys()
            .filter(|k| *k.typ() == KeyType::Ed25519)
            .map(|key| RepositoryKeyConfig::Ed25519Key(key.as_bytes().to_vec()))
            .collect::<Vec<_>>();

        let root_version = trusted_root.version();

        Ok(RepositoryConfig {
            repo_url: Some(self.repo_url()),
            root_keys: Some(root_keys),
            root_version: Some(root_version),
            mirrors: Some(vec![MirrorConfig {
                mirror_url: Some(format!("http://{}", mirror_url)),
                subscribe: Some(self.backend.supports_watch()),
            }]),
            storage_type: storage_type,
        })
    }

    async fn get_tuf_client(
        tuf_repo: Box<dyn RepositoryProvider<Json>>,
    ) -> Result<Client<Json, EphemeralRepository<Json>, Box<dyn RepositoryProvider<Json>>>, Error>
    {
        let metadata_repo = EphemeralRepository::<Json>::new();

        let raw_signed_meta = {
            // FIXME(http://fxbug.dev/92126) we really should be initializing trust, rather than just
            // trusting 1.root.json.
            let mut md = tuf_repo
                .fetch_metadata(&MetadataPath::from_role(&Role::Root), MetadataVersion::Number(1))
                .await?;

            let mut buf = Vec::new();
            md.read_to_end(&mut buf).await.context("reading metadata")?;

            RawSignedMetadata::<Json, _>::new(buf)
        };

        let client =
            Client::with_trusted_root(Config::default(), &raw_signed_meta, metadata_repo, tuf_repo)
                .await?;

        Ok(client)
    }

    async fn get_components_for_package(
        &self,
        trusted_targets: &Verified<TargetsMetadata>,
        package: &RepositoryPackage,
    ) -> Result<Option<Vec<PackageEntry>>> {
        let package_entries = self
            .show_target_package(trusted_targets, package.name.as_ref().unwrap().to_string())
            .await?;
        if package_entries.is_none() {
            return Ok(None);
        }
        let components = package_entries
            .unwrap()
            .into_iter()
            .filter(|e| is_component_manifest(&e.path.as_ref().unwrap()))
            .collect();
        Ok(Some(components))
    }

    // TODO(fxbug.dev/79915) add tests for this method.
    pub async fn list_packages(
        &self,
        include_fields: ListFields,
    ) -> Result<Vec<RepositoryPackage>, Error> {
        let trusted_targets = {
            let mut client = self.client.lock().await;

            // Get the latest TUF metadata.
            client.update().await?;

            client.database().trusted_targets().context("missing target information")?.clone()
        };

        let packages: Result<Vec<RepositoryPackage>, Error> =
            join_all(trusted_targets.targets().into_iter().filter_map(|(k, v)| {
                let custom = v.custom();
                let size = custom.get("size")?.as_u64()?;
                let hash = custom.get("merkle")?.as_str().unwrap_or("").to_string();
                let path = format!("blobs/{}", hash);
                Some(async move {
                    let mut bytes = vec![];
                    self.fetch_blob(&hash).await?.read_to_end(&mut bytes).await?;
                    let mut archive = AsyncReader::new(Adapter::new(Cursor::new(bytes))).await?;
                    let contents = archive.read_file("meta/contents").await?;
                    let contents = MetaContents::deserialize(contents.as_slice())?;

                    let size = size
                        + try_join_all(contents.contents().into_iter().map(
                            |(_, hash)| async move {
                                self.fetch_blob(&hash.to_string()).await.map(|x| x.content_len)
                            },
                        ))
                        .await?
                        .into_iter()
                        .sum::<u64>();

                    let modified = self
                        .backend
                        .blob_modification_time(&path)
                        .await?
                        .map(|x| -> anyhow::Result<u64> {
                            Ok(x.duration_since(SystemTime::UNIX_EPOCH)?.as_secs())
                        })
                        .transpose()?;
                    Ok(RepositoryPackage {
                        name: Some(k.to_string()),
                        size: Some(size),
                        hash: Some(hash),
                        modified,
                        ..RepositoryPackage::EMPTY
                    })
                })
            }))
            .await
            .into_iter()
            .collect();

        let mut packages = packages?;

        if include_fields.intersects(ListFields::COMPONENTS) {
            for package in packages.iter_mut() {
                match self.get_components_for_package(&trusted_targets, &package).await {
                    Ok(components) => package.entries = components,
                    Err(e) => {
                        log::error!(
                            "failed to get components for package '{}': {}",
                            package.name.as_ref().unwrap_or(&String::from("<unknown>")),
                            e
                        )
                    }
                };
            }
        }

        Ok(packages)
    }

    pub async fn show_package(&self, package_name: String) -> Result<Option<Vec<PackageEntry>>> {
        let trusted_targets = {
            let mut client = self.client.lock().await;

            // Get the latest TUF metadata.
            client.update().await?;

            client.database().trusted_targets().context("expected targets information")?.clone()
        };

        self.show_target_package(&trusted_targets, package_name).await
    }

    async fn show_target_package(
        &self,
        trusted_targets: &Verified<TargetsMetadata>,
        package_name: String,
    ) -> Result<Option<Vec<PackageEntry>>> {
        let target_path = TargetPath::new(&package_name)?;
        let target = if let Some(target) = trusted_targets.targets().get(&target_path) {
            target
        } else {
            return Ok(None);
        };

        let custom = target.custom();

        let hash = custom
            .get("merkle")
            .ok_or_else(|| anyhow!("package {:?} is missing the `merkle` field", package_name))?;

        let hash = hash
            .as_str()
            .ok_or_else(|| {
                anyhow!("package {:?} hash should be a string, not {:?}", package_name, hash)
            })?
            .to_string();

        let size = custom
            .get("size")
            .ok_or_else(|| anyhow!("package {:?} is missing the `size` field", package_name))?;

        let size = size
            .as_u64()
            .ok_or_else(|| anyhow!("package {:?} should be a u64, not {:?}", package_name, size))?;

        // Read the meta.far.
        let mut meta_far_bytes = vec![];
        self.fetch_blob(&hash).await?.read_to_end(&mut meta_far_bytes).await?;
        let mut archive = AsyncReader::new(Adapter::new(Cursor::new(meta_far_bytes))).await?;

        let modified = self
            .backend
            .blob_modification_time(&hash)
            .await?
            .map(|x| -> anyhow::Result<u64> {
                Ok(x.duration_since(SystemTime::UNIX_EPOCH)?.as_secs())
            })
            .transpose()?;

        // Add entry for meta.far
        let mut entries = vec![PackageEntry {
            path: Some("meta.far".to_string()),
            hash: Some(hash),
            size: Some(size),
            modified,
            ..PackageEntry::EMPTY
        }];

        entries.extend(archive.list().map(|item| PackageEntry {
            path: Some(item.path().to_string()),
            size: Some(item.length()),
            modified,
            ..PackageEntry::EMPTY
        }));

        match archive.read_file("meta/contents").await {
            Ok(c) => {
                let contents = MetaContents::deserialize(c.as_slice())?;
                for (name, hash) in contents.contents() {
                    let hash_string = hash.to_string();
                    let size = self.fetch_blob(&hash_string).await?.content_len;
                    let modified = self
                        .backend
                        .blob_modification_time(&hash_string)
                        .await?
                        .map(|x| -> anyhow::Result<u64> {
                            Ok(x.duration_since(SystemTime::UNIX_EPOCH)?.as_secs())
                        })
                        .transpose()?;

                    entries.push(PackageEntry {
                        path: Some(name.to_owned()),
                        hash: Some(hash_string),
                        size: Some(size),
                        modified,
                        ..PackageEntry::EMPTY
                    });
                }
            }
            Err(e) => {
                log::warn!("failed to read meta/contents for package {}: {}", package_name, e);
            }
        }

        Ok(Some(entries))
    }
}

impl Drop for Repository {
    fn drop(&mut self) {
        for handler in std::mem::take(&mut *self.drop_handlers.lock()) {
            (handler)()
        }
    }
}

#[derive(Debug, Clone)]
pub enum ResourceRange {
    RangeFull,
    Range { start: u64, end: u64 },
    RangeFrom { start: u64 },
    RangeTo { end: u64 },
}

#[async_trait::async_trait]
pub trait RepositoryBackend: std::fmt::Debug {
    /// Get a [RepositorySpec] for this [Repository]
    fn spec(&self) -> RepositorySpec;

    /// Fetch a metadata [Resource] from this repository.
    async fn fetch_metadata(&self, path: &str, range: ResourceRange) -> Result<Resource, Error>;

    /// Fetch a blob [Resource] from this repository.
    async fn fetch_blob(&self, path: &str, range: ResourceRange) -> Result<Resource, Error>;

    /// Whether or not the backend supports watching for file changes.
    fn supports_watch(&self) -> bool {
        false
    }

    /// Returns a stream which sends a unit value every time the given path is modified.
    fn watch(&self) -> anyhow::Result<BoxStream<'static, ()>> {
        Err(anyhow::anyhow!("Watching not supported for this repo type"))
    }

    /// Get the modification time of a blob in this repository if available.
    async fn blob_modification_time(&self, path: &str) -> anyhow::Result<Option<SystemTime>>;

    /// Produces the backing TUF [RepositoryProvider] for this repository.
    fn get_tuf_repo(&self) -> Result<Box<dyn RepositoryProvider<Json>>, Error>;
}

#[cfg(test)]
mod test {
    use {
        super::*,
        crate::test_utils::{make_readonly_empty_repository, repo_key},
    };
    const ROOT_VERSION: u32 = 1;
    const REPO_NAME: &str = "fake-repo";

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_get_config() {
        let repo = make_readonly_empty_repository(REPO_NAME).await.unwrap();

        let server_url = "some-url:1234";

        assert_eq!(
            repo.get_config(server_url, None).await.unwrap(),
            RepositoryConfig {
                repo_url: Some(format!("fuchsia-pkg://{}", REPO_NAME)),
                root_keys: Some(vec![repo_key()]),
                root_version: Some(ROOT_VERSION),
                storage_type: None,
                mirrors: Some(vec![MirrorConfig {
                    mirror_url: Some(format!("http://{}", server_url)),
                    subscribe: Some(true),
                }]),
            },
        );

        assert_eq!(
            repo.get_config(server_url, Some(RepositoryStorageType::Persistent)).await.unwrap(),
            RepositoryConfig {
                repo_url: Some(format!("fuchsia-pkg://{}", REPO_NAME)),
                root_keys: Some(vec![repo_key()]),
                root_version: Some(ROOT_VERSION),
                storage_type: Some(RepositoryStorageType::Persistent),
                mirrors: Some(vec![MirrorConfig {
                    mirror_url: Some(format!("http://{}", server_url)),
                    subscribe: Some(true),
                }]),
            },
        );
    }
}
