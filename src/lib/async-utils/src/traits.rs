// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Traits that are useful for working with async code, but do not fit into a more specific
//! category. These are often extension traits for types that are not defined by `async_utils`.

use core::task::Poll;

/// An extension trait for `core::task::Poll` that provides convenient adapters for extracting
/// output values.
pub trait PollExt<T> {
    /// Returns the value contained in a Poll::Ready value or panics.
    fn unwrap(self) -> T;

    /// Returns the value contained in a Poll::Ready value or panics with a custom message.
    fn expect(self, msg: &str) -> T;

    /// Expect a Poll to be Pending, or panics with a custom message.
    fn expect_pending(&self, msg: &str);

    /// Turns a Poll into a Result, mapping Poll::Ready(value) to Ok(value) and
    /// Poll::Pending to Err(error)
    fn ready_or<E>(self, error: E) -> Result<T, E>;

    /// Turns a Poll into a Result, mapping Poll::Ready(value) to Ok(value) and
    /// Poll::Pending to Err(error())
    fn ready_or_else<E, F>(self, error: F) -> Result<T, E>
    where
        F: FnOnce() -> E;
}

impl<T> PollExt<T> for Poll<T> {
    #[inline]
    #[track_caller]
    fn unwrap(self) -> T {
        match self {
            Poll::Ready(val) => val,
            Poll::Pending => panic!("called `Poll::unwrap()` on a `Pending` value"),
        }
    }

    #[inline]
    #[track_caller]
    fn expect(self, msg: &str) -> T {
        match self {
            Poll::Ready(val) => val,
            Poll::Pending => panic!("{}", msg),
        }
    }

    #[inline]
    #[track_caller]
    fn expect_pending(&self, msg: &str) {
        if self.is_ready() {
            panic!("{}", msg);
        }
    }

    fn ready_or<E>(self, error: E) -> Result<T, E> {
        self.ready_or_else(|| error)
    }

    fn ready_or_else<E, F>(self, error: F) -> Result<T, E>
    where
        F: FnOnce() -> E,
    {
        match self {
            Poll::Ready(val) => Ok(val),
            Poll::Pending => Err(error()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn poll_unwrap_ready_returns_value() {
        let p = Poll::Ready("value");
        assert_eq!(p.unwrap(), "value");
    }

    #[test]
    #[should_panic]
    fn poll_unwrap_pending_panics() {
        let p: Poll<()> = Poll::Pending;
        p.unwrap();
    }

    #[test]
    fn poll_expect_ready_returns_value() {
        let p = Poll::Ready("value");
        assert_eq!(p.expect("missing value"), "value");
    }

    #[test]
    fn poll_expect_pending() {
        let p: Poll<()> = Poll::Pending;
        p.expect_pending("is pending");
    }

    #[test]
    #[should_panic]
    fn poll_expect_pending_on_ready() {
        let p = Poll::Ready("value");
        p.expect_pending("value is not pending");
    }

    #[test]
    #[should_panic(expected = "missing value")]
    fn poll_expect_pending_panics_with_message() {
        let p: Poll<()> = Poll::Pending;
        p.expect("missing value");
    }

    #[test]
    fn poll_ready_or_ready_returns_ok() {
        let p = Poll::Ready("value");
        assert_eq!(p.ready_or(()), Ok("value"));
    }

    #[test]
    fn poll_ready_or_pending_returns_error() {
        let p: Poll<()> = Poll::Pending;
        assert_eq!(p.ready_or(()), Err(()));
    }

    #[test]
    fn poll_ready_or_else_ready_returns_ok() {
        let p = Poll::Ready("value");
        assert_eq!(p.ready_or_else(|| ()), Ok("value"));
    }

    #[test]
    fn poll_ready_or_else_pending_returns_error() {
        let p: Poll<()> = Poll::Pending;
        assert_eq!(p.ready_or_else(|| ()), Err(()));
    }
}
