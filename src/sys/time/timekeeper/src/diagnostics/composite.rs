// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::diagnostics::{Diagnostics, Event};

/// A trivial `Diagnostics` implementation that simply sends every event it receives to two other
/// `Diagnostics` implementations.
pub struct CompositeDiagnostics<L: Diagnostics, R: Diagnostics> {
    /// The first `Diagnostics` implementation to receive events.
    left: L,
    /// The second `Diagnostics` implementation to receive events.
    right: R,
}

impl<L: Diagnostics, R: Diagnostics> CompositeDiagnostics<L, R> {
    /// Contructs a new `CompositeDiagnostics` instance that forwards all events to the supplied
    /// diagnostics implementations.
    pub fn new(left: L, right: R) -> Self {
        Self { left, right }
    }
}

impl<L: Diagnostics, R: Diagnostics> Diagnostics for CompositeDiagnostics<L, R> {
    fn record(&self, event: Event) {
        let event_clone = event.clone();
        self.left.record(event_clone);
        self.right.record(event);
    }
}

#[cfg(test)]
mod test {
    use {
        super::*,
        crate::diagnostics::FakeDiagnostics,
        crate::enums::{ClockUpdateReason, Role, TimeSourceError, Track},
        std::sync::Arc,
    };

    const UPDATE_EVENT: Event =
        Event::UpdateClock { track: Track::Monitor, reason: ClockUpdateReason::TimeStep };
    const TIME_SOURCE_FAILED_EVENT: Event =
        Event::TimeSourceFailed { role: Role::Primary, error: TimeSourceError::CallFailed };

    #[fuchsia::test]
    fn log_events() {
        let left = Arc::new(FakeDiagnostics::new());
        let right = Arc::new(FakeDiagnostics::new());
        let composite = CompositeDiagnostics::new(Arc::clone(&left), Arc::clone(&right));
        left.assert_events(&[]);
        right.assert_events(&[]);

        composite.record(UPDATE_EVENT);
        left.assert_events(&[UPDATE_EVENT]);
        right.assert_events(&[UPDATE_EVENT]);

        composite.record(TIME_SOURCE_FAILED_EVENT);
        left.assert_events(&[UPDATE_EVENT, TIME_SOURCE_FAILED_EVENT]);
        right.assert_events(&[UPDATE_EVENT, TIME_SOURCE_FAILED_EVENT]);
    }
}
