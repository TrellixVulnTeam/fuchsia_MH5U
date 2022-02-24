// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fuchsia_zircon::{self as zx, sys::zx_status_t, AsHandleRef},
    parking_lot::Mutex,
    std::sync::Arc,
};

use crate::{
    log::*,
    transport::uart::{SerialReadState, IO_COMPLETE, IO_ERROR},
};

#[repr(C)]
pub struct serial_impl_async_protocol_t {
    _private: [u8; 0],
}

#[cfg(test)]
impl serial_impl_async_protocol_t {
    pub fn new() -> serial_impl_async_protocol_t {
        serial_impl_async_protocol_t { _private: [] }
    }
}

/// Opaque type representing a `parking_lot::Mutex<SerialReadState>`.
#[repr(C)]
pub struct mutex_serial_read_t {
    _private: [u8; 0],
}

/// Contains the declarations of externally defined functions
#[cfg(not(test))]
mod sys {
    use super::*;

    #[link(name = "serial_impl_async_wrapper", kind = "static")]
    extern "C" {
        pub fn serial_write_async(
            serial: *mut serial_impl_async_protocol_t,
            buffer: *const u8,
            length: usize,
            evt_ptr: *const fuchsia_zircon::Event,
        );
        pub fn serial_read_async(
            serial: *mut serial_impl_async_protocol_t,
            ctx: *const mutex_serial_read_t,
        );
        pub fn serial_cancel_all(serial: *mut serial_impl_async_protocol_t);
        pub fn free_serial_impl_async_protocol(serial: *mut serial_impl_async_protocol_t);
    }
}

/// Contains fake implementations of externally defined functions.
#[cfg(test)]
mod sys {
    use super::*;

    pub unsafe fn serial_write_async(
        _serial: *mut serial_impl_async_protocol_t,
        _buffer: *const u8,
        _length: usize,
        _evt_ptr: *const fuchsia_zircon::Event,
    ) {
    }
    pub unsafe fn serial_read_async(
        _serial: *mut serial_impl_async_protocol_t,
        _ctx: *const mutex_serial_read_t,
    ) {
    }
    pub unsafe fn serial_cancel_all(_serial: *mut serial_impl_async_protocol_t) {}
    pub unsafe fn free_serial_impl_async_protocol(_serial: *mut serial_impl_async_protocol_t) {}
}

/// Owns a void ptr to a `serial_impl_async_protocol_t` object. The only thing that can be done with
/// this pointer is to pass out references and free the memory by dropping it.
#[repr(transparent)]
pub struct Serial(*mut serial_impl_async_protocol_t);

impl Serial {
    pub(super) unsafe fn new(ptr: *mut serial_impl_async_protocol_t) -> Serial {
        Serial(ptr)
    }

    /// The returned pointer should be used with caution because it does not respect
    /// the lifetime of the borrow in this call. The pointer will be dangling after the
    /// `Serial` is dropped. It is the responsibility of the caller to ensure that the
    /// pointer isn't used beyond the lifetime of this object.
    pub fn as_ptr(&self) -> *mut serial_impl_async_protocol_t {
        self.0
    }

    /// Cancel all outstanding I/O requests.
    pub unsafe fn cancel_all(&self) {
        sys::serial_cancel_all(self.0);
    }
}

#[cfg(test)]
impl Serial {
    pub unsafe fn fake() -> Serial {
        Serial::new(Box::into_raw(Box::new(serial_impl_async_protocol_t::new())))
    }
}

// Send is needed to pass the Serial object from the main thread to the rust managed thread
// and from the rust managed thread. Once Serial is passed to rust, it is never used by the
// main thread.
unsafe impl Send for Serial {}

impl Drop for Serial {
    fn drop(&mut self) {
        unsafe {
            sys::serial_cancel_all(self.0);
            sys::free_serial_impl_async_protocol(self.0);
        }
    }
}

pub type WriteAsyncFn = dyn Fn(&Serial, &[u8], Arc<zx::Event>);

// This is unsafe because it calls into code which reads `serial` which is a
// `*mut serial_impl_async_read_t`.
pub fn serial_write_async(serial: &Serial, buffer: &[u8], event: Arc<zx::Event>) {
    #[cfg(feature = "extra-tracing")]
    trace_duration!("Transport::UartWrite");
    unsafe {
        sys::serial_write_async(
            serial.as_ptr(),
            buffer.as_ptr(),
            buffer.len(),
            Arc::into_raw(event),
        );
    }
}

pub type ReadAsyncFn = dyn Fn(*mut serial_impl_async_protocol_t, Arc<Mutex<SerialReadState>>);

pub fn serial_read_async(
    serial: *mut serial_impl_async_protocol_t,
    read_state: Arc<parking_lot::Mutex<SerialReadState>>,
) {
    #[cfg(feature = "extra-tracing")]
    trace_duration!("Transport::UartRead");
    unsafe {
        sys::serial_read_async(serial, Arc::into_raw(read_state) as *const mutex_serial_read_t);
    }
}

/// Convert a zircon status to the appropriate event signal.
fn status_to_signal(status: zx::Status) -> zx::Signals {
    match status {
        zx::Status::OK => IO_COMPLETE,
        _ => IO_ERROR,
    }
}

#[no_mangle]
pub extern "C" fn serial_read_complete(
    arc_read_state: *const parking_lot::Mutex<SerialReadState>,
    status: zx_status_t,
    buffer: *const u8,
    length: usize,
) {
    #[cfg(feature = "extra-tracing")]
    trace_duration!("Transport::UartReadComplete");

    let read_state = unsafe {
        assert!(!arc_read_state.is_null());
        Arc::from_raw(arc_read_state)
    };

    let status = zx::Status::from_raw(status);
    let signal = status_to_signal(status);
    match status {
        zx::Status::OK => {
            let mut guard = read_state.lock();
            let state: &mut SerialReadState = &mut guard;
            let buffer = unsafe {
                assert!(!buffer.is_null());
                std::slice::from_raw_parts(buffer, length)
            };
            state.push_read_bytes(buffer);
            if state.check_for_next_packet().is_ok() {
                // Queue another read.
                let serial_ptr = state.serial_ptr;
                drop(state);
                drop(guard);
                serial_read_async(serial_ptr, read_state);
            }
        }
        zx::Status::NOT_SUPPORTED => {
            bt_log_warn!("Async read already pending");
            trace_instant!("Transport::UartDoubleRead", fuchsia_trace::Scope::Thread);
            std::mem::forget(read_state);
        }
        error_status => {
            bt_log_warn!("Async read error {}", error_status);
            trace_instant!("Transport::UartError", fuchsia_trace::Scope::Thread);
            let _ = read_state.lock().event.signal_handle(zx::Signals::NONE, signal);
            std::mem::forget(read_state);
        }
    }
}

#[no_mangle]
pub extern "C" fn serial_write_complete(arc_event_ptr: *const zx::Event, status: zx_status_t) {
    let event = unsafe {
        assert!(!arc_event_ptr.is_null());
        Arc::from_raw(arc_event_ptr)
    };
    let status = zx::Status::from_raw(status);
    if status == zx::Status::NOT_SUPPORTED {
        bt_log_warn!("Async write already pending");
    } else if status != zx::Status::OK {
        bt_log_warn!("Async write error {}", status);
    }
    if let Err(status) = event.signal_handle(zx::Signals::NONE, status_to_signal(status)) {
        bt_log_warn!("Could not signal handle: {}", status);
    };
}

#[cfg(test)]
mod tests {
    use super::*;
    use fuchsia_zircon::DurationNum;
    use std::sync::Arc;

    // The Drop trait must be implemented for `Serial` to free owned memory.
    // This test will fail to compile if this is not the case.
    #[test]
    fn serial_implements_drop_trait() {
        assert!(std::mem::needs_drop::<Serial>());
    }

    #[test]
    fn ensure_status_to_signal_conversions() {
        let signal = status_to_signal(zx::Status::OK);
        assert_eq!(signal, IO_COMPLETE);
        let signal = status_to_signal(zx::Status::NOT_SUPPORTED);
        assert_eq!(signal, IO_ERROR);
        let signal = status_to_signal(zx::Status::INTERNAL);
        assert_eq!(signal, IO_ERROR);
    }

    #[test]
    fn serial_write_complete_succeeds() {
        let event = Arc::new(zx::Event::create().unwrap());
        let event_ = event.clone();
        let _ = std::thread::spawn(move || {
            serial_write_complete(Arc::into_raw(event_), zx::sys::ZX_OK)
        });
        // wait_handle call will be terminated if signal is not set for 5s.
        let res = event
            .wait_handle(IO_COMPLETE | IO_ERROR, zx::Time::after(5.seconds()))
            .expect("waiting on a valid event handle");
        assert_eq!(res, IO_COMPLETE);
    }

    #[test]
    #[should_panic]
    fn serial_write_null_event_panics() {
        serial_write_complete(std::ptr::null(), zx::sys::ZX_OK);
    }
}
