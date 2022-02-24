// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Framing and deframing datagrams onto QUIC streams

use super::{PeerConnRef, ReadExact};
use crate::coding;
use crate::labels::NodeId;
use crate::stat_counter::StatCounter;
use anyhow::{format_err, Error};
use futures::{prelude::*, ready};
use quic::{AsyncQuicStreamReader, AsyncQuicStreamWriter, StreamProperties};
use std::{
    convert::TryInto,
    pin::Pin,
    task::{Context, Poll},
};

/// The type of frame that can be received on a QUIC stream
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum FrameType {
    Hello,
    Data(coding::Context),
    Control(coding::Context),
    Signal(coding::Context),
}

/// Header for one frame of data on a QUIC stream
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct FrameHeader {
    /// Type of the frame
    frame_type: FrameType,
    /// Length of the frame (usize here to avoid casts in client code; this is checked to fit in a
    /// u32 before serialization)
    length: usize,
}

/// Length of the header for a frame.
const FRAME_HEADER_LENGTH: usize = 8;

impl FrameHeader {
    fn to_bytes(&self) -> Result<[u8; FRAME_HEADER_LENGTH], Error> {
        let length = self.length;
        if length > std::u32::MAX as usize {
            return Err(anyhow::format_err!("Message too long: {}", length));
        }
        let length = length as u32;
        let hdr: u64 = (length as u64)
            | (match self.frame_type {
                FrameType::Hello => 0,
                FrameType::Data(coding::Context { use_persistent_header: false }) => 1,
                FrameType::Control(coding::Context { use_persistent_header: false }) => 2,
                FrameType::Signal(coding::Context { use_persistent_header: false }) => 3,
                FrameType::Data(coding::Context { use_persistent_header: true }) => 4,
                FrameType::Control(coding::Context { use_persistent_header: true }) => 5,
                FrameType::Signal(coding::Context { use_persistent_header: true }) => 6,
            } << 32);
        Ok(hdr.to_le_bytes())
    }

    fn from_bytes(bytes: &[u8]) -> Result<Self, Error> {
        let hdr: &[u8; FRAME_HEADER_LENGTH] = bytes[0..FRAME_HEADER_LENGTH].try_into()?;
        let hdr = u64::from_le_bytes(*hdr);
        let length = (hdr & 0xffff_ffff) as usize;
        let frame_type = match hdr >> 32 {
            0 => FrameType::Hello,
            1 => FrameType::Data(coding::Context { use_persistent_header: false }),
            2 => FrameType::Control(coding::Context { use_persistent_header: false }),
            3 => FrameType::Signal(coding::Context { use_persistent_header: false }),
            4 => FrameType::Data(coding::Context { use_persistent_header: true }),
            5 => FrameType::Control(coding::Context { use_persistent_header: true }),
            6 => FrameType::Signal(coding::Context { use_persistent_header: true }),
            _ => return Err(anyhow::format_err!("Unknown frame type {}", hdr >> 32)),
        };
        Ok(FrameHeader { frame_type, length })
    }
}

#[derive(Default, Debug)]
pub struct MessageStats {
    sent_bytes: StatCounter,
    sent_messages: StatCounter,
}

impl MessageStats {
    fn sent_message(&self, bytes: u64) {
        self.sent_messages.inc();
        self.sent_bytes.add(bytes);
    }

    pub fn sent_bytes(&self) -> u64 {
        self.sent_bytes.fetch()
    }

    pub fn sent_messages(&self) -> u64 {
        self.sent_messages.fetch()
    }
}

#[derive(Debug)]
pub(crate) struct FramedStreamWriter {
    /// The underlying QUIC stream
    quic: AsyncQuicStreamWriter,
    /// The peer node id
    peer_node_id: NodeId,
}

impl FramedStreamWriter {
    pub fn from_quic(quic: AsyncQuicStreamWriter, peer_node_id: NodeId) -> Self {
        Self { quic, peer_node_id }
    }

    pub async fn abandon(&mut self) {
        self.quic.abandon().await
    }

    pub fn conn(&self) -> PeerConnRef<'_> {
        PeerConnRef::from_quic(self.quic.conn(), self.peer_node_id)
    }

    pub fn id(&self) -> u64 {
        self.quic.id()
    }

    pub async fn send(
        &mut self,
        frame_type: FrameType,
        bytes: &[u8],
        fin: bool,
        message_stats: &MessageStats,
    ) -> Result<(), Error> {
        let r = self.send_inner(frame_type, bytes, fin, message_stats).await;
        if r.is_err() {
            self.abandon().await;
        }
        r
    }

    async fn send_inner(
        &mut self,
        frame_type: FrameType,
        bytes: &[u8],
        fin: bool,
        message_stats: &MessageStats,
    ) -> Result<(), Error> {
        let frame_len = bytes.len();
        assert!(frame_len <= 0xffff_ffff);
        let mut header = FrameHeader { frame_type, length: frame_len }.to_bytes()?;
        log::trace!("header: {:?}", header);
        self.quic.send(&mut header, false).await?;
        if bytes.len() > 0 {
            self.quic.send(bytes, fin).await?;
        }
        message_stats.sent_message(header.len() as u64 + bytes.len() as u64);
        Ok(())
    }
}

#[derive(Debug)]
pub(crate) struct FramedStreamReader {
    /// The underlying QUIC stream
    quic: AsyncQuicStreamReader,
    /// Peer node id
    peer_node_id: NodeId,
    /// Current read state
    read_state: ReadState,
    /// Scratch space for reading the frame header
    hdr: [u8; FRAME_HEADER_LENGTH],
    /// Scratch space for reading the frame payload
    payload: Vec<u8>,
}

impl FramedStreamReader {
    pub fn from_quic(quic: AsyncQuicStreamReader, peer_node_id: NodeId) -> Self {
        Self {
            quic,
            peer_node_id,
            read_state: ReadState::Initial,
            hdr: [0u8; FRAME_HEADER_LENGTH],
            payload: Vec::new(),
        }
    }

    pub(crate) async fn abandon(&mut self) {
        self.quic.abandon().await
    }

    pub fn conn(&self) -> PeerConnRef<'_> {
        PeerConnRef::from_quic(self.quic.conn(), self.peer_node_id)
    }

    pub fn is_initiator(&self) -> bool {
        self.quic.is_initiator()
    }

    pub(crate) fn next<'b>(&'b mut self) -> ReadNextFrame<'b> {
        let (read, payload) = match self.read_state {
            ReadState::Initial => {
                self.payload.clear();
                (self.quic.read_exact(&mut self.hdr), Some(&mut self.payload))
            }
            ReadState::GotHeader(hdr) => {
                assert_eq!(self.payload.len(), hdr.length);
                (self.quic.read_exact(&mut self.payload), None)
            }
        };
        ReadNextFrame {
            read,
            payload,
            read_state: &mut self.read_state,
            peer_node_id: self.peer_node_id,
        }
    }
}

#[derive(Debug)]
enum ReadState {
    Initial,
    GotHeader(FrameHeader),
}

#[derive(Debug)]
pub(crate) struct ReadNextFrame<'b> {
    read: ReadExact<'b>,
    read_state: &'b mut ReadState,
    payload: Option<&'b mut Vec<u8>>,
    peer_node_id: NodeId,
}

impl<'b> ReadNextFrame<'b> {
    #[allow(dead_code)]
    pub(crate) fn debug_id(&self) -> impl std::fmt::Debug {
        self.read.debug_id()
    }

    pub(crate) fn conn(&self) -> PeerConnRef<'_> {
        PeerConnRef::from_quic(self.read.conn(), self.peer_node_id)
    }

    fn poll_inner(
        &mut self,
        ctx: &mut Context<'_>,
    ) -> Poll<Result<(FrameType, Vec<u8>, bool), Error>> {
        loop {
            let fin = ready!(self.read.poll_unpin(ctx))?;
            match self.read_state {
                ReadState::Initial => {
                    let hdr = FrameHeader::from_bytes(self.read.read_buf_mut().as_mut_slice())?;
                    if hdr.length == 0 {
                        return Poll::Ready(Ok((hdr.frame_type, Vec::new(), fin)));
                    }
                    if fin {
                        return Poll::Ready(Err(format_err!("Unexpected end of stream")));
                    }
                    let payload = self.payload.take().unwrap();
                    *payload = vec![0u8; hdr.length];
                    self.read.rearm(payload);
                    *self.read_state = ReadState::GotHeader(hdr);
                }
                ReadState::GotHeader(hdr) => {
                    let out =
                        Poll::Ready(Ok((hdr.frame_type, self.read.read_buf_mut().take_vec(), fin)));
                    *self.read_state = ReadState::Initial;
                    return out;
                }
            }
        }
    }
}

impl<'b> Future for ReadNextFrame<'b> {
    type Output = Result<(FrameType, Vec<u8>, bool), Error>;
    fn poll(self: Pin<&mut Self>, ctx: &mut Context<'_>) -> Poll<Self::Output> {
        Pin::into_inner(self).poll_inner(ctx)
    }
}

#[cfg(test)]
mod test {
    use super::*;

    fn roundtrip(h: FrameHeader) {
        assert_eq!(h, FrameHeader::from_bytes(&h.to_bytes().unwrap()).unwrap());
    }

    #[fuchsia::test]
    fn roundtrips() {
        roundtrip(FrameHeader {
            frame_type: FrameType::Data(coding::Context { use_persistent_header: false }),
            length: 0,
        });
        roundtrip(FrameHeader {
            frame_type: FrameType::Data(coding::Context { use_persistent_header: false }),
            length: std::u32::MAX as usize,
        });
        roundtrip(FrameHeader {
            frame_type: FrameType::Data(coding::Context { use_persistent_header: true }),
            length: 0,
        });
        roundtrip(FrameHeader {
            frame_type: FrameType::Data(coding::Context { use_persistent_header: true }),
            length: std::u32::MAX as usize,
        });
    }

    #[fuchsia::test]
    fn too_long() {
        FrameHeader {
            frame_type: FrameType::Data(coding::Context { use_persistent_header: false }),
            length: (std::u32::MAX as usize) + 1,
        }
        .to_bytes()
        .expect_err("Should fail");
    }

    #[fuchsia::test]
    fn bad_frame_type() {
        assert!(format!(
            "{}",
            FrameHeader::from_bytes(&[0, 0, 0, 0, 11, 0, 0, 0]).expect_err("should fail")
        )
        .contains("Unknown frame type 11"));
    }
}
