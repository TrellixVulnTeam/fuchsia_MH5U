// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{id::Id, proxies::player::Player, Result};
use anyhow::Context as _;
use fidl_fuchsia_media_sessions2::*;
use fuchsia_inspect as inspect;
use fuchsia_syslog::fx_log_warn;
use futures::{channel::mpsc, channel::oneshot, prelude::*};
use std::sync::Arc;

const LOG_TAG: &str = "publisher";

/// Implements `fuchsia.media.session2.Publisher`.
pub struct Publisher {
    player_list: Arc<inspect::Node>,
    player_sink: mpsc::Sender<Player>,
}

impl Publisher {
    pub fn new(player_sink: mpsc::Sender<Player>, player_list: Arc<inspect::Node>) -> Self {
        Self { player_sink, player_list }
    }

    pub async fn serve(mut self, mut request_stream: PublisherRequestStream) -> Result<()> {
        while let Some(request) = request_stream.try_next().await.context("Publisher requests")? {
            let (player, registration, responder) = match request {
                PublisherRequest::Publish { player, registration, responder } => {
                    (player, registration, Some(responder))
                }
            };

            let (player_published_sink, player_published_receiever) = oneshot::channel();
            let id = Id::new().context("Allocating new unique id")?;
            let id_str = format!("{}", id.get());
            let player_node = self.player_list.create_child(id_str);

            let player_result =
                Player::new(id, player, registration, player_node, player_published_sink);

            match player_result {
                Ok(player) => {
                    let session_id = player.id();
                    self.player_sink.send(player).await?;

                    // Wait till other tasks know about this ID
                    player_published_receiever.await?;

                    // Tell client about the new ID
                    if let Some(responder) = responder {
                        responder.send(session_id)?;
                    }
                }
                Err(e) => {
                    fx_log_warn!(
                        tag: LOG_TAG,
                        "A request to publish a player was invalid: {:?}",
                        e
                    );
                }
            }
        }

        Ok(())
    }
}
