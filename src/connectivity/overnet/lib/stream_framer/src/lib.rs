// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Handles framing/deframing of stream links

use anyhow::{format_err, Error};
use async_utils::mutex_ticket::MutexTicket;
use fuchsia_async::{Task, Timer};
use futures::future::poll_fn;
use futures::lock::Mutex;
use futures::prelude::*;
use futures::ready;
use std::sync::Arc;
use std::task::{Context, Poll, Waker};
use std::time::Duration;

/// Describes a framing format.
pub trait Format: Send + Sync + 'static {
    /// Write a frame of `frame_type` with payload `bytes` into `outgoing`.
    fn frame(&self, bytes: &[u8], outgoing: &mut Vec<u8>) -> Result<(), Error>;

    /// Parse `bytes`.
    /// If the bytes could never lead to a successfully parsed frame ever again, return Err(_).
    /// Otherwise, return Ok(_).
    fn deframe(&self, bytes: &[u8]) -> Result<Deframed, Error>;

    /// Return a time at which the first byte of the buffered bytes should be dropped (as it's
    /// unlikely they'll form a correct parse).
    fn deframe_timeout(&self, have_pending_bytes: bool) -> Option<Duration>;
}

// We occasionally want to make a dynamic selection of framing type, and in those cases it's
// often convenient to deal with a Box<dyn Format> instead of a custom thing. Provide this impl
// so that a Box<dyn Format> is a Format too.
impl Format for Box<dyn Format> {
    fn frame(&self, bytes: &[u8], outgoing: &mut Vec<u8>) -> Result<(), Error> {
        self.as_ref().frame(bytes, outgoing)
    }

    fn deframe(&self, bytes: &[u8]) -> Result<Deframed, Error> {
        self.as_ref().deframe(bytes)
    }

    fn deframe_timeout(&self, have_pending_bytes: bool) -> Option<Duration> {
        self.as_ref().deframe_timeout(have_pending_bytes)
    }
}

/// Success result of [`Format::deframe`].
pub struct Deframed {
    /// The new beginning of the parsing buffer, as an offset from the beginning of the buffer.
    pub new_start_pos: usize,
    /// How many bytes (measured from the start of the buffer) should be reported as unframed
    /// data. It's required that `unframed_bytes` <= `new_start_pos`.
    pub unframed_bytes: usize,
    /// Optional parsed frame from the buffer.
    pub frame: Option<Vec<u8>>,
}

/// Manages framing of messages into a byte stream.
struct Framer<Fmt: Format> {
    fmt: Fmt,
    max_queued: usize,
    outgoing: Mutex<Outgoing>,
}

struct BVec(Vec<u8>);

impl std::fmt::Debug for BVec {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.0.len() <= 4 {
            self.0.fmt(f)
        } else {
            write!(f, "{:?}+{}b", &self.0[..4], self.0.len() - 4)
        }
    }
}

#[derive(Debug)]
enum Outgoing {
    Closed,
    Open { buffer: BVec, waiting_read: Option<Waker>, waiting_write: Option<Waker> },
}

impl Outgoing {
    fn wake(self) {
        match self {
            Outgoing::Closed => (),
            Outgoing::Open { waiting_read, waiting_write, .. } => {
                waiting_read.map(|w| w.wake());
                waiting_write.map(|w| w.wake());
            }
        }
    }
}

impl Default for Outgoing {
    fn default() -> Self {
        Outgoing::Closed
    }
}

/// Writes frames into the framer.
pub struct FramerWriter<Fmt: Format> {
    framer: Arc<Framer<Fmt>>,
}

/// Reads framed bytes out of the framer.
pub struct FramerReader<Fmt: Format> {
    framer: Arc<Framer<Fmt>>,
}

/// Construct a new framer for some format.
pub fn new_framer<Fmt: Format>(
    fmt: Fmt,
    max_queued: usize,
) -> (FramerWriter<Fmt>, FramerReader<Fmt>) {
    let framer = Arc::new(Framer {
        fmt,
        max_queued,
        outgoing: Mutex::new(Outgoing::Open {
            buffer: BVec(Vec::new()),
            waiting_read: None,
            waiting_write: None,
        }),
    });
    (FramerWriter { framer: framer.clone() }, FramerReader { framer })
}

impl<Fmt: Format> FramerWriter<Fmt> {
    fn poll_write(
        &self,
        ctx: &mut Context<'_>,
        bytes: &[u8],
        lock: &mut MutexTicket<'_, Outgoing>,
    ) -> Poll<Result<(), Error>> {
        let mut outgoing = ready!(lock.poll(ctx));
        match std::mem::replace(&mut *outgoing, Outgoing::Closed) {
            Outgoing::Closed => Poll::Ready(Err(format_err!("Closed Framer"))),
            Outgoing::Open { buffer: BVec(mut buffer), mut waiting_read, waiting_write: _ } => {
                if buffer.len() >= self.framer.max_queued {
                    *outgoing = Outgoing::Open {
                        buffer: BVec(buffer),
                        waiting_read,
                        waiting_write: Some(ctx.waker().clone()),
                    };
                    Poll::Pending
                } else {
                    self.framer.fmt.frame(bytes, &mut buffer)?;
                    waiting_read.take().map(|w| w.wake());
                    *outgoing =
                        Outgoing::Open { buffer: BVec(buffer), waiting_read, waiting_write: None };
                    Poll::Ready(Ok(()))
                }
            }
        }
    }

    /// Write a frame into the framer.
    pub async fn write(&mut self, bytes: &[u8]) -> Result<(), Error> {
        let mut lock = MutexTicket::new(&self.framer.outgoing);
        poll_fn(|ctx| self.poll_write(ctx, bytes, &mut lock)).await
    }
}

impl<Fmt: Format> Drop for FramerWriter<Fmt> {
    fn drop(&mut self) {
        let framer = self.framer.clone();
        // TODO: don't detach
        Task::spawn(async move {
            std::mem::replace(&mut *framer.outgoing.lock().await, Outgoing::Closed).wake();
        })
        .detach()
    }
}

impl<Fmt: Format> FramerReader<Fmt> {
    fn poll_read(
        &self,
        ctx: &mut Context<'_>,
        lock: &mut MutexTicket<'_, Outgoing>,
    ) -> Poll<Result<Vec<u8>, Error>> {
        let mut outgoing = ready!(lock.poll(ctx));
        match std::mem::replace(&mut *outgoing, Outgoing::Closed) {
            Outgoing::Closed => Poll::Ready(Err(format_err!("Closed Framer"))),
            Outgoing::Open { buffer: BVec(buffer), mut waiting_write, waiting_read: _ } => {
                if buffer.is_empty() {
                    *outgoing = Outgoing::Open {
                        buffer: BVec(buffer),
                        waiting_write,
                        waiting_read: Some(ctx.waker().clone()),
                    };
                    Poll::Pending
                } else {
                    waiting_write.take().map(|w| w.wake());
                    *outgoing = Outgoing::Open {
                        buffer: BVec(Vec::new()),
                        waiting_write,
                        waiting_read: None,
                    };
                    Poll::Ready(Ok(buffer))
                }
            }
        }
    }

    /// Read framed bytes out of the framer.
    pub async fn read(&mut self) -> Result<Vec<u8>, Error> {
        let mut lock = MutexTicket::new(&self.framer.outgoing);
        poll_fn(|ctx| self.poll_read(ctx, &mut lock)).await
    }
}

impl<Fmt: Format> Drop for FramerReader<Fmt> {
    fn drop(&mut self) {
        let framer = self.framer.clone();
        // TODO: don't detach
        Task::spawn(async move {
            std::mem::replace(&mut *framer.outgoing.lock().await, Outgoing::Closed).wake();
        })
        .detach()
    }
}

struct Deframer<Fmt: Format> {
    fmt: Fmt,
    incoming: Mutex<Incoming>,
}

#[derive(Debug)]
enum Incoming {
    Parsing {
        unparsed: BVec,
        waiting_read: Option<Waker>,
        timeout: Option<Timer>,
    },
    Queuing {
        unframed: Option<BVec>,
        framed: Option<BVec>,
        unparsed: BVec,
        waiting_write: Option<Waker>,
    },
    Closed,
}

impl Default for Incoming {
    fn default() -> Self {
        Incoming::Closed
    }
}

impl Incoming {
    fn wake(self) {
        match self {
            Incoming::Closed => (),
            Incoming::Parsing { waiting_read, .. } => {
                waiting_read.map(|w| w.wake());
            }
            Incoming::Queuing { waiting_write, .. } => {
                waiting_write.map(|w| w.wake());
            }
        }
    }
}

/// Writes framed bytes into the deframer.
pub struct DeframerWriter<Fmt: Format> {
    deframer: Arc<Deframer<Fmt>>,
}

/// Reads deframed packets from a deframer.
pub struct DeframerReader<Fmt: Format> {
    deframer: Arc<Deframer<Fmt>>,
}

/// Construct a new deframer, with an optional timeout for reads (such that bytes can be skipped).
pub fn new_deframer<Fmt: Format>(fmt: Fmt) -> (DeframerWriter<Fmt>, DeframerReader<Fmt>) {
    let deframer = Arc::new(Deframer {
        incoming: Mutex::new(Incoming::Parsing {
            unparsed: BVec(Vec::new()),
            waiting_read: None,
            timeout: fmt.deframe_timeout(false).map(Timer::new),
        }),
        fmt,
    });
    (DeframerWriter { deframer: deframer.clone() }, DeframerReader { deframer })
}

fn deframe_step<Fmt: Format>(
    mut unparsed: Vec<u8>,
    mut waiting_read: Option<Waker>,
    fmt: &Fmt,
) -> Result<Incoming, Error> {
    let Deframed { frame, unframed_bytes, new_start_pos } = fmt.deframe(&unparsed)?;
    assert!(unframed_bytes <= new_start_pos);
    let mut unframed = None;
    if unframed_bytes != 0 {
        let mut unframed_vec = unparsed.split_off(unframed_bytes);
        std::mem::swap(&mut unframed_vec, &mut unparsed);
        unframed = Some(BVec(unframed_vec));
    }
    if new_start_pos != unframed_bytes {
        unparsed.drain(..(new_start_pos - unframed_bytes));
    }
    if frame.is_some() || unframed.is_some() {
        waiting_read.take().map(|w| w.wake());
        Ok(Incoming::Queuing {
            framed: frame.map(BVec),
            unframed,
            unparsed: BVec(unparsed),
            waiting_write: None,
        })
    } else {
        Ok(Incoming::Parsing {
            timeout: make_timeout(fmt, unparsed.len()),
            unparsed: BVec(unparsed),
            waiting_read,
        })
    }
}

fn make_timeout<Fmt: Format>(fmt: &Fmt, unparsed_len: usize) -> Option<Timer> {
    fmt.deframe_timeout(unparsed_len > 0).map(Timer::new)
}

impl<Fmt: Format> DeframerWriter<Fmt> {
    fn poll_write(
        &self,
        ctx: &mut Context<'_>,
        bytes: &[u8],
        lock: &mut MutexTicket<'_, Incoming>,
    ) -> Poll<Result<(), Error>> {
        let mut incoming = ready!(lock.poll(ctx));
        match std::mem::replace(&mut *incoming, Incoming::Closed) {
            Incoming::Closed => Poll::Ready(Err(format_err!("Deframer closed during write"))),
            Incoming::Parsing { unparsed: BVec(mut unparsed), waiting_read, timeout: _ } => {
                unparsed.extend_from_slice(bytes);
                *incoming = deframe_step(unparsed, waiting_read, &self.deframer.fmt)?;
                Poll::Ready(Ok(()))
            }
            Incoming::Queuing { unparsed, framed, unframed, waiting_write: _ } => {
                *incoming = Incoming::Queuing {
                    unparsed,
                    framed,
                    unframed,
                    waiting_write: Some(ctx.waker().clone()),
                };
                Poll::Pending
            }
        }
    }

    /// Write some data into the deframer, to be deframed and read later.
    pub async fn write(&mut self, bytes: &[u8]) -> Result<(), Error> {
        if bytes.is_empty() {
            return Ok(());
        }
        let mut lock = MutexTicket::new(&self.deframer.incoming);
        poll_fn(|ctx| self.poll_write(ctx, bytes, &mut lock)).await
    }
}

impl<Fmt: Format> Drop for DeframerWriter<Fmt> {
    fn drop(&mut self) {
        let framer = self.deframer.clone();
        // TODO: don't detach
        Task::spawn(async move {
            std::mem::replace(&mut *framer.incoming.lock().await, Incoming::Closed).wake();
        })
        .detach()
    }
}

#[derive(Debug, PartialEq)]
/// Bytes ready by a DeframerReader.
pub enum ReadBytes {
    /// A frame to be processed.
    Framed(Vec<u8>),
    /// Garbage skipped between frames.
    Unframed(Vec<u8>),
}

impl<Fmt: Format> DeframerReader<Fmt> {
    fn poll_read(
        &self,
        ctx: &mut Context<'_>,
        lock: &mut MutexTicket<'_, Incoming>,
    ) -> Poll<Result<ReadBytes, Error>> {
        let mut incoming = ready!(lock.poll(ctx));
        loop {
            break match std::mem::replace(&mut *incoming, Incoming::Closed) {
                Incoming::Closed => Poll::Ready(Err(format_err!("Deframer closed during read"))),
                Incoming::Queuing {
                    unframed: Some(BVec(unframed)),
                    framed,
                    unparsed: BVec(unparsed),
                    waiting_write,
                } => {
                    if framed.is_some() {
                        *incoming = Incoming::Queuing {
                            unframed: None,
                            framed,
                            unparsed: BVec(unparsed),
                            waiting_write,
                        };
                    } else {
                        *incoming = deframe_step(unparsed, None, &self.deframer.fmt)?;
                        waiting_write.map(|w| w.wake());
                    }
                    Poll::Ready(Ok(ReadBytes::Unframed(unframed)))
                }
                Incoming::Queuing {
                    unframed: None,
                    framed: Some(BVec(bytes)),
                    unparsed: BVec(unparsed),
                    waiting_write,
                } => {
                    *incoming = deframe_step(unparsed, None, &self.deframer.fmt)?;
                    waiting_write.map(|w| w.wake());
                    Poll::Ready(Ok(ReadBytes::Framed(bytes)))
                }
                Incoming::Queuing { unframed: None, framed: None, .. } => unreachable!(),
                Incoming::Parsing { unparsed, timeout: None, waiting_read: _ } => {
                    *incoming = Incoming::Parsing {
                        unparsed,
                        waiting_read: Some(ctx.waker().clone()),
                        timeout: None,
                    };
                    Poll::Pending
                }
                Incoming::Parsing {
                    unparsed: BVec(mut unparsed),
                    timeout: Some(mut timeout),
                    waiting_read,
                } => match timeout.poll_unpin(ctx) {
                    Poll::Pending => {
                        *incoming = Incoming::Parsing {
                            unparsed: BVec(unparsed),
                            waiting_read: Some(ctx.waker().clone()),
                            timeout: Some(timeout),
                        };
                        Poll::Pending
                    }
                    Poll::Ready(()) => {
                        let mut unframed = unparsed.split_off(1);
                        std::mem::swap(&mut unframed, &mut unparsed);
                        waiting_read.map(|w| w.wake());
                        *incoming = Incoming::Queuing {
                            unframed: Some(BVec(unframed)),
                            framed: None,
                            unparsed: BVec(unparsed),
                            waiting_write: None,
                        };
                        continue;
                    }
                },
            };
        }
    }

    /// Read one frame from the deframer.
    pub async fn read(&mut self) -> Result<ReadBytes, Error> {
        let mut lock = MutexTicket::new(&self.deframer.incoming);
        poll_fn(|ctx| self.poll_read(ctx, &mut lock)).await
    }
}

impl<Fmt: Format> Drop for DeframerReader<Fmt> {
    fn drop(&mut self) {
        let framer = self.deframer.clone();
        // TODO: don't detach
        Task::spawn(async move {
            std::mem::replace(&mut *framer.incoming.lock().await, Incoming::Closed).wake();
        })
        .detach()
    }
}

#[cfg(test)]
mod test {

    use super::*;
    use byteorder::WriteBytesExt;
    use crc::crc32;

    /// Framing format that assumes an underlying transport that *MAY* lose/duplicate/corrupt some bytes
    /// but usually transports the full 8 bits in a byte (e.g. many serial transports).
    pub struct LossyBinary {
        duration_per_byte: Duration,
    }

    impl LossyBinary {
        /// Create a new LossyBinary format instance with some timeout waiting for bytes (if this is
        /// exceeded a byte will be skipped in the input).
        pub fn new(duration_per_byte: Duration) -> Self {
            Self { duration_per_byte }
        }
    }

    impl Format for LossyBinary {
        fn frame(&self, bytes: &[u8], outgoing: &mut Vec<u8>) -> Result<(), Error> {
            if bytes.len() > (std::u16::MAX as usize) + 1 {
                return Err(anyhow::format_err!(
                    "Packet length ({}) too long for stream framing",
                    bytes.len()
                ));
            }
            outgoing.reserve(2 + 4 + bytes.len() + 1);
            outgoing.write_u16::<byteorder::LittleEndian>((bytes.len() - 1) as u16)?;
            outgoing.write_u32::<byteorder::LittleEndian>(crc32::checksum_ieee(bytes))?;
            outgoing.extend_from_slice(bytes);
            outgoing.write_u8(10u8)?; // '\n'
            Ok(())
        }

        fn deframe(&self, bytes: &[u8]) -> Result<Deframed, Error> {
            let mut start = 0;
            loop {
                let buf = &bytes[start..];
                if buf.len() <= 7 {
                    return Ok(Deframed {
                        frame: None,
                        unframed_bytes: start,
                        new_start_pos: start,
                    });
                }
                let len = 1 + (u16::from_le_bytes([buf[0], buf[1]]) as usize);
                let crc = u32::from_le_bytes([buf[2], buf[3], buf[4], buf[5]]);
                if buf.len() < 7 + len {
                    // Not enough bytes to deframe: done for now
                    return Ok(Deframed {
                        frame: None,
                        unframed_bytes: start,
                        new_start_pos: start,
                    });
                }
                if buf[6 + len] != 10u8 {
                    // Does not end with an end marker: remove start byte and continue
                    start += 1;
                    continue;
                }
                let frame = &buf[6..6 + len];
                let crc_actual = crc32::checksum_ieee(frame);
                if crc != crc_actual {
                    // CRC mismatch: skip start marker and continue
                    start += 1;
                    continue;
                }
                // Successfully got a frame! Save it, and continue
                return Ok(Deframed {
                    frame: Some(frame.to_vec()),
                    unframed_bytes: start,
                    new_start_pos: start + 7 + len,
                });
            }
        }

        fn deframe_timeout(&self, have_pending_bytes: bool) -> Option<Duration> {
            if have_pending_bytes {
                Some(self.duration_per_byte)
            } else {
                None
            }
        }
    }

    fn join(mut a: Vec<u8>, mut b: Vec<u8>) -> Vec<u8> {
        a.append(&mut b);
        a
    }

    #[fuchsia_async::run(1, test)]
    async fn simple_frame_lossy_binary() -> Result<(), Error> {
        let (mut framer_writer, mut framer_reader) =
            new_framer(LossyBinary::new(Duration::from_millis(100)), 1024);
        framer_writer.write(&[1, 2, 3, 4]).await?;
        let (mut deframer_writer, mut deframer_reader) =
            new_deframer(LossyBinary::new(Duration::from_millis(100)));
        deframer_writer.write(framer_reader.read().await?.as_slice()).await?;
        assert_eq!(deframer_reader.read().await?, ReadBytes::Framed(vec![1, 2, 3, 4]));
        framer_writer.write(&[5, 6, 7, 8]).await?;
        deframer_writer.write(framer_reader.read().await?.as_slice()).await?;
        assert_eq!(deframer_reader.read().await?, ReadBytes::Framed(vec![5, 6, 7, 8]));
        Ok(())
    }

    #[fuchsia_async::run(1, test)]
    async fn skip_junk_start_0() -> Result<(), Error> {
        let (mut framer_writer, mut framer_reader) =
            new_framer(LossyBinary::new(Duration::from_millis(100)), 1024);
        framer_writer.write(&[1, 2, 3, 4]).await?;
        let (mut deframer_writer, mut deframer_reader) =
            new_deframer(LossyBinary::new(Duration::from_millis(100)));
        deframer_writer.write(join(vec![0], framer_reader.read().await?).as_slice()).await?;
        assert_eq!(deframer_reader.read().await?, ReadBytes::Unframed(vec![0]));
        assert_eq!(deframer_reader.read().await?, ReadBytes::Framed(vec![1, 2, 3, 4]));
        Ok(())
    }

    #[fuchsia_async::run(1, test)]
    async fn skip_junk_start_1() -> Result<(), Error> {
        let (mut framer_writer, mut framer_reader) =
            new_framer(LossyBinary::new(Duration::from_millis(100)), 1024);
        framer_writer.write(&[1, 2, 3, 4]).await?;
        let (mut deframer_writer, mut deframer_reader) =
            new_deframer(LossyBinary::new(Duration::from_millis(100)));
        deframer_writer.write(join(vec![1], framer_reader.read().await?).as_slice()).await?;
        assert_eq!(deframer_reader.read().await?, ReadBytes::Unframed(vec![1]));
        assert_eq!(deframer_reader.read().await?, ReadBytes::Framed(vec![1, 2, 3, 4]));
        Ok(())
    }
}
