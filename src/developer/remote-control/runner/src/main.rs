// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Result},
    fidl_fuchsia_developer_remotecontrol::{RemoteControlMarker, RemoteControlProxy},
    fuchsia_async as fasync,
    fuchsia_component::client::connect_to_protocol,
    futures::future::try_join,
    futures::io::BufReader,
    futures::prelude::*,
    hoist::{hoist, OvernetInstance},
    std::os::unix::io::{AsRawFd, FromRawFd},
};

const BUFFER_SIZE: usize = 65536;

async fn buffered_copy<R, W>(mut from: R, mut to: W, buffer_size: usize) -> std::io::Result<u64>
where
    R: AsyncRead + std::marker::Unpin,
    W: AsyncWrite + std::marker::Unpin,
{
    let mut buf_from = BufReader::with_capacity(buffer_size, &mut from);
    futures::io::copy_buf(&mut buf_from, &mut to).await
}

fn zx_socket_from_fd(fd: i32) -> Result<fidl::AsyncSocket> {
    let handle = fdio::transfer_fd(unsafe { std::fs::File::from_raw_fd(fd) })?;
    fidl::AsyncSocket::from_socket(fidl::Socket::from(handle))
        .context("making fidl::AsyncSocket from fidl::Socket")
}

async fn send_request(proxy: &RemoteControlProxy, id: Option<u64>) -> Result<()> {
    // If the program was launched with a u64, that's our ffx daemon ID, so add it to RCS.
    // The daemon id is used to map the RCS instance back to an ip address or
    // nodename in the daemon, for target merging.
    if let Some(id) = id {
        proxy.add_id(id).await.with_context(|| format!("Failed to add id {} to RCS", id))
    } else {
        // We just need to make a request to the RCS - it doesn't really matter
        // what we choose here so long as there are no side effects.
        let _ = proxy.identify_host().await?;
        Ok(())
    }
}

fn get_id_argument<I>(mut args: I) -> Result<Option<u64>>
where
    I: Iterator<Item = String>,
{
    // Assume the first argument is the name of the binary.
    let _ = args.next();
    if let Some(arg) = args.next() {
        arg.parse::<u64>()
            .with_context(|| format!("Failed to parse {} as u64", arg))
            .map(|n| Some(n))
    } else {
        Ok(None)
    }
}

#[fasync::run_singlethreaded]
async fn main() -> Result<()> {
    let rcs_proxy = connect_to_protocol::<RemoteControlMarker>()?;
    send_request(&rcs_proxy, get_id_argument(std::env::args())?).await?;
    let (local_socket, remote_socket) = fidl::Socket::create(fidl::SocketOpts::STREAM)?;
    let local_socket = fidl::AsyncSocket::from_socket(local_socket)?;
    let (mut rx_socket, mut tx_socket) = futures::AsyncReadExt::split(local_socket);
    hoist().connect_as_mesh_controller()?.attach_socket_link(remote_socket)?;

    let mut stdin = zx_socket_from_fd(std::io::stdin().lock().as_raw_fd())?;
    let mut stdout = zx_socket_from_fd(std::io::stdout().lock().as_raw_fd())?;

    try_join(
        buffered_copy(&mut stdin, &mut tx_socket, BUFFER_SIZE),
        buffered_copy(&mut rx_socket, &mut stdout, BUFFER_SIZE),
    )
    .await
    .map(|_| ())
    .context("io copy")
}

#[cfg(test)]
mod test {

    use {
        super::*,
        anyhow::Error,
        fidl::endpoints::create_proxy_and_stream,
        fidl_fuchsia_developer_remotecontrol::{
            IdentifyHostResponse, RemoteControlMarker, RemoteControlProxy, RemoteControlRequest,
        },
        fuchsia_async as fasync,
        std::cell::RefCell,
        std::rc::Rc,
    };

    #[test]
    fn test_get_id_argument() {
        assert_eq!(
            get_id_argument(vec!["foo".to_string(), "1234".to_string()].into_iter()).unwrap(),
            Some(1234u64)
        );
        assert_eq!(
            get_id_argument(vec!["foo".to_string(), "4567".to_string()].into_iter()).unwrap(),
            Some(4567u64)
        );
        assert!(get_id_argument(vec!["foo".to_string(), "foo".to_string()].into_iter()).is_err());
    }

    fn setup_fake_rcs(handle_stream: bool) -> RemoteControlProxy {
        let (proxy, mut stream) = create_proxy_and_stream::<RemoteControlMarker>().unwrap();

        if !handle_stream {
            return proxy;
        }

        fasync::Task::local(async move {
            let last_id = Rc::new(RefCell::new(0));
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(RemoteControlRequest::IdentifyHost { responder }) => {
                        let _ = responder
                            .send(&mut Ok(IdentifyHostResponse {
                                nodename: Some("".to_string()),
                                addresses: Some(vec![]),
                                ids: Some(vec![last_id.borrow().clone()]),
                                ..IdentifyHostResponse::EMPTY
                            }))
                            .unwrap();
                    }
                    Some(RemoteControlRequest::AddId { id, responder }) => {
                        last_id.replace(id);
                        responder.send().unwrap();
                    }
                    _ => assert!(false),
                }
            }
        })
        .detach();

        proxy
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_handles_successful_response() -> Result<(), Error> {
        let rcs_proxy = setup_fake_rcs(true);
        assert!(send_request(&rcs_proxy, None).await.is_ok());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_handles_failed_response() -> Result<(), Error> {
        let rcs_proxy = setup_fake_rcs(false);
        assert!(send_request(&rcs_proxy, None).await.is_err());
        Ok(())
    }

    #[fasync::run_singlethreaded(test)]
    async fn test_sends_id_if_given() -> Result<(), Error> {
        let rcs_proxy = setup_fake_rcs(true);
        send_request(&rcs_proxy, Some(34u64)).await.unwrap();
        let ident = rcs_proxy.identify_host().await?.unwrap();
        assert_eq!(34u64, ident.ids.unwrap()[0]);
        Ok(())
    }
}
