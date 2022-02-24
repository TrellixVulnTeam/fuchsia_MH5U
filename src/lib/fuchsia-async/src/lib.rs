// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A futures-rs executor design specifically for Fuchsia OS.

#![deny(missing_docs)]

mod runtime;
pub use self::runtime::*;

mod handle;
pub use self::handle::channel::{Channel, RecvMsg};
pub use self::handle::on_signals::OnSignals;
pub use self::handle::socket::Socket;

/// Asynchronous networking abstractions.
pub mod net;

#[cfg(target_os = "fuchsia")]
pub use self::handle::{
    fifo::{Fifo, FifoEntry, FifoReadable, FifoWritable, ReadEntry, WriteEntry},
    rwhandle::RWHandle,
};

/// An emulation library for Zircon handles on non-Fuchsia platforms.
#[cfg(not(target_os = "fuchsia"))]
pub mod emulated_handle {
    pub use super::handle::{
        AsHandleRef, Channel, EmulatedHandleRef, EventPair, Handle, HandleBased, HandleDisposition,
        HandleInfo, HandleOp, HandleRef, MessageBuf, MessageBufEtc, ObjectType, Peered, Rights,
        Signals, Socket, SocketOpts,
    };
}

/// A future which can be used by multiple threads at once.
pub mod atomic_future;

// Re-export pin_mut as its used by the async proc macros
pub use pin_utils::pin_mut;

pub use fuchsia_async_macro::{run, run_singlethreaded, run_until_stalled};

// TODO(cramertj) remove once async/awaitification has occurred
#[cfg(target_os = "fuchsia")]
pub mod temp;

// Reexport futures for use in macros;
#[doc(hidden)]
pub mod futures {
    pub use futures::*;
}

/// Testing support for repeated runs
pub mod test_support;
