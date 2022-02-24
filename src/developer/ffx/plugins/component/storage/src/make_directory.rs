// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::RemotePath,
    anyhow::Result,
    component_hub::io::Directory,
    errors::{ffx_bail, ffx_error},
    ffx_component_storage_args::MakeDirectoryArgs,
    fidl::endpoints::create_proxy,
    fidl_fuchsia_io as fio,
    fidl_fuchsia_sys2::StorageAdminProxy,
};

pub async fn make_directory(
    storage_admin: StorageAdminProxy,
    args: MakeDirectoryArgs,
) -> Result<()> {
    make_directory_cmd(storage_admin, args.path).await
}

async fn make_directory_cmd(storage_admin: StorageAdminProxy, path: String) -> Result<()> {
    let remote_path = RemotePath::parse(&path)?;

    let (dir_proxy, server) = create_proxy::<fio::DirectoryMarker>()?;
    let server = server.into_channel();
    let storage_dir = Directory::from_proxy(dir_proxy);

    if remote_path.relative_path.as_os_str().is_empty() {
        ffx_bail!("Remote path cannot be the root");
    }

    // Open the storage
    storage_admin
        .open_component_storage_by_id(&remote_path.component_instance_id, server.into())
        .await?
        .map_err(|e| ffx_error!("Could not open component storage: {:?}", e))?;

    // Send a request to create the directory
    let dir = storage_dir
        .open_dir(remote_path.relative_path, fio::OPEN_FLAG_CREATE | fio::OPEN_RIGHT_READABLE)?;

    // Verify that we can actually read the contents of the directory created
    dir.entries().await?;

    Ok(())
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use {
        super::*,
        crate::test::setup_oneshot_fake_storage_admin,
        fidl::endpoints::{RequestStream, ServerEnd},
        fidl::handle::AsyncChannel,
        fidl_fuchsia_io::*,
        fidl_fuchsia_sys2::StorageAdminRequest,
        futures::TryStreamExt,
    };

    pub fn node_to_directory(object: ServerEnd<NodeMarker>) -> DirectoryRequestStream {
        DirectoryRequestStream::from_channel(
            AsyncChannel::from_channel(object.into_channel()).unwrap(),
        )
    }

    fn setup_fake_storage_admin(expected_id: &'static str) -> StorageAdminProxy {
        setup_oneshot_fake_storage_admin(move |req| match req {
            StorageAdminRequest::OpenComponentStorageById { id, object, responder, .. } => {
                assert_eq!(expected_id, id);
                setup_fake_directory(node_to_directory(object));
                responder.send(&mut Ok(())).unwrap();
            }
            _ => panic!("got unexpected {:?}", req),
        })
    }

    // TODO(xbhatnag): Replace this mock with something more robust like VFS.
    // Currently VFS is not cross-platform.
    fn setup_fake_directory(mut root_dir: DirectoryRequestStream) {
        fuchsia_async::Task::local(async move {
            // Serve the root directory
            // Root directory should get Open call with CREATE flag
            let request = root_dir.try_next().await;
            let object =
                if let Ok(Some(DirectoryRequest::Open { flags, mode, path, object, .. })) = request
                {
                    assert_eq!(path, "test");
                    assert!(flags & OPEN_FLAG_CREATE != 0);
                    assert!(flags & OPEN_FLAG_DIRECTORY != 0);
                    assert!(mode & MODE_TYPE_DIRECTORY != 0);
                    object
                } else {
                    panic!("did not get open request: {:?}", request);
                };

            // Serve the new test directory
            let mut test_dir = node_to_directory(object);

            // Rewind on new directory should succeed
            let request = test_dir.try_next().await;
            if let Ok(Some(DirectoryRequest::Rewind { responder, .. })) = request {
                responder.send(0).unwrap();
            } else {
                panic!("did not get rewind request: {:?}", request)
            }

            // ReadDirents should report no contents in the new directory
            let request = test_dir.try_next().await;
            if let Ok(Some(DirectoryRequest::ReadDirents { responder, .. })) = request {
                responder.send(0, &[]).unwrap();
            } else {
                panic!("did not get readdirents request: {:?}", request)
            }
        })
        .detach();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_make_directory() -> Result<()> {
        let storage_admin = setup_fake_storage_admin("123456");
        make_directory_cmd(storage_admin, "123456::test".to_string()).await
    }
}
