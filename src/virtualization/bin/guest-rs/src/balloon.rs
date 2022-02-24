// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use {
    crate::services,
    anyhow::anyhow,
    anyhow::Context,
    anyhow::Error,
    fidl_fuchsia_virtualization::{BalloonControllerMarker, BalloonControllerProxy},
    fuchsia_zircon_status as zx_status,
};

const TAG_LENGTH: usize = 21; // How long should we make the stat strings

// Constants from zircon/system/ulib/virtio/include/virtio/balloon.h
const VIRTIO_BALLOON_S_SWAP_IN: u16 = 0;
const VIRTIO_BALLOON_S_SWAP_OUT: u16 = 1;
const VIRTIO_BALLOON_S_MAJFLT: u16 = 2;
const VIRTIO_BALLOON_S_MINFLT: u16 = 3;
const VIRTIO_BALLOON_S_MEMFREE: u16 = 4;
const VIRTIO_BALLOON_S_MEMTOT: u16 = 5;
const VIRTIO_BALLOON_S_AVAIL: u16 = 6; // Available memory as in /proc
const VIRTIO_BALLOON_S_CACHES: u16 = 7; // Disk caches
const VIRTIO_BALLOON_S_HTLB_PGALLOC: u16 = 8; // HugeTLB page allocations
const VIRTIO_BALLOON_S_HTLB_PGFAIL: u16 = 9; // HugeTLB page allocation failures

pub async fn connect_to_balloon_controller(
    env_id: u32,
    cid: u32,
) -> Result<BalloonControllerProxy, Error> {
    let realm = services::connect_to_env(env_id)?;

    let (balloon_controller, balloon_server_end) =
        fidl::endpoints::create_proxy::<BalloonControllerMarker>()
            .context("failed to make balloon controller")?;

    // Wire up the controller we made to the realm created (that has the memory we want)
    realm.connect_to_balloon(cid, balloon_server_end).context("Failed to connect to given cid")?;

    Ok(balloon_controller)
}

fn map_tag_name(tag: u16) -> String {
    format!(
        "{:TAG_LENGTH$}",
        match tag {
            VIRTIO_BALLOON_S_SWAP_IN => String::from("swap-in:"),
            VIRTIO_BALLOON_S_SWAP_OUT => String::from("swap-out:"),
            VIRTIO_BALLOON_S_MAJFLT => String::from("major-faults:"),
            VIRTIO_BALLOON_S_MINFLT => String::from("minor-faults:"),
            VIRTIO_BALLOON_S_MEMFREE => String::from("free-memory:"),
            VIRTIO_BALLOON_S_MEMTOT => String::from("total-memory:"),
            VIRTIO_BALLOON_S_AVAIL => String::from("available-memory:"),
            VIRTIO_BALLOON_S_CACHES => String::from("disk-caches:"),
            VIRTIO_BALLOON_S_HTLB_PGALLOC => String::from("hugetlb-allocations:"),
            VIRTIO_BALLOON_S_HTLB_PGFAIL => String::from("hugetlb-failures:"),
            _ => String::from("unknown:"),
        }
    )
}

pub async fn handle_balloon(
    balloon_controller: BalloonControllerProxy,
    num_pages: u32,
) -> Result<String, Error> {
    // Update the balloon size to the size specified
    balloon_controller
        .request_num_pages(num_pages)
        .context("Couldn't query the balloon controller")?;
    Ok(format!("Resizing memory balloon to {} pages!\n", num_pages))
}

pub async fn handle_balloon_stats(
    balloon_controller: BalloonControllerProxy,
) -> Result<String, Error> {
    // If this errors we couldn't find the specified env at all
    let (status, mem_stats) =
        balloon_controller.get_mem_stats().await.context("Failed to get memory statistics")?;
    // mem_stats is an Option<Vec<MemStat>>,
    // and it can succeed with a None (usually on ZX_ERR_SHOULD_WAIT)
    let stats = mem_stats.ok_or(anyhow!(zx_status::Status::from_raw(status)))?;
    Ok(stats
        .into_iter()
        .map(|stat| format!("{}{}", map_tag_name(stat.tag), stat.val))
        .collect::<Vec<String>>()
        .join("\n"))
}

#[cfg(test)]
mod test {
    use {
        super::*,
        assert_matches::assert_matches,
        fidl::endpoints::{create_proxy_and_stream, ControlHandle, RequestStream},
        fidl_fuchsia_virtualization::{BalloonControllerMarker, MemStat},
        fuchsia_async::{self as fasync, futures::StreamExt},
        fuchsia_zircon_status as zx_status,
        pretty_assertions::assert_eq,
    };

    #[fasync::run_until_stalled(test)]
    async fn balloon_valid_page_num_returns_ok() {
        let (proxy, mut stream) = create_proxy_and_stream::<BalloonControllerMarker>().unwrap();
        let expected_string = "Resizing memory balloon to 0 pages!\n";

        let res = handle_balloon(proxy, 0).await;
        let _ = stream
            .next()
            .await
            .expect("Failed to read from stream")
            .expect("Failed to parse request")
            .into_request_num_pages()
            .expect("Unexpected call to Balloon Controller");

        assert_eq!(res.unwrap(), expected_string);
    }

    #[fasync::run_until_stalled(test)]
    async fn balloon_stats_server_shut_down_returns_err() {
        let (proxy, mut stream) = create_proxy_and_stream::<BalloonControllerMarker>().unwrap();
        let _task = fasync::Task::spawn(async move {
            let _ = stream
                .next()
                .await
                .expect("Failed to read from stream")
                .expect("Failed to parse request")
                .into_get_mem_stats()
                .expect("Unexpected call to Balloon Controller");
            stream.control_handle().shutdown();
        });

        let res = handle_balloon_stats(proxy).await;
        assert_matches!(
            res.unwrap_err().downcast(),
            Ok(fidl::Error::ClientChannelClosed {
                status: zx_status::Status::PEER_CLOSED,
                protocol_name: "fuchsia.virtualization.BalloonController"
            })
        );
    }

    #[fasync::run_until_stalled(test)]
    async fn balloon_stats_empty_mem_stats_returns_empty_string() {
        let (proxy, mut stream) = create_proxy_and_stream::<BalloonControllerMarker>().unwrap();
        let mut test_stats: [MemStat; 0] = [];

        let _task = fasync::Task::spawn(async move {
            let responder = stream
                .next()
                .await
                .expect("Failed to read from stream")
                .expect("Failed to parse request")
                .into_get_mem_stats()
                .expect("Unexpected call to Balloon Controller");
            responder
                .send(0, Some(&mut test_stats.iter_mut()))
                .expect("Failed to send request to proxy");
        });

        let res = handle_balloon_stats(proxy).await;
        assert_eq!(res.unwrap(), "");
    }

    #[fasync::run_until_stalled(test)]
    async fn balloon_stats_empty_input_returns_err() {
        let (proxy, mut stream) = create_proxy_and_stream::<BalloonControllerMarker>().unwrap();

        let _task = fasync::Task::spawn(async move {
            let responder = stream
                .next()
                .await
                .expect("Failed to read from stream")
                .expect("Failed to parse request")
                .into_get_mem_stats()
                .expect("Unexpected call to Balloon Controller");
            responder
                .send(zx_status::Status::INTERNAL.into_raw(), None)
                .expect("Failed to send request to proxy");
        });

        let res = handle_balloon_stats(proxy).await;
        assert_matches!(res.unwrap_err().downcast(), Ok(zx_status::Status::INTERNAL));
    }

    #[fasync::run_until_stalled(test)]
    async fn balloon_stats_valid_input_returns_valid_string() {
        let mut test_stats: [MemStat; 2] = [
            MemStat { tag: VIRTIO_BALLOON_S_SWAP_IN, val: 2 },
            MemStat { tag: VIRTIO_BALLOON_S_SWAP_OUT, val: 3 },
        ];

        let (proxy, mut stream) = create_proxy_and_stream::<BalloonControllerMarker>().unwrap();
        let _task = fasync::Task::spawn(async move {
            let responder = stream
                .next()
                .await
                .expect("Failed to read from stream")
                .expect("Failed to parse request")
                .into_get_mem_stats()
                .expect("Unexpected call to Balloon Controller");
            responder
                .send(0, Some(&mut test_stats.iter_mut()))
                .expect("Failed to send request to proxy");
        });

        let res: String =
            handle_balloon_stats(proxy).await.expect("Unexpected error result from Balloon Stats");
        assert_eq!((&res).split_whitespace().collect::<String>().contains("swap-in:2"), true);
        assert_eq!((&res).split_whitespace().collect::<String>().contains("swap-out:3"), true);
    }
}
