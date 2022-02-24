// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT
#include "kernel/semaphore.h"

#include <zircon/compiler.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <kernel/thread_lock.h>

void Semaphore::Post() {
  Guard<MonitoredSpinLock, IrqSave> guard{ThreadLock::Get(), SOURCE_TAG};

  // Either the number of waiters in the wait queue, or the semaphore count,
  // must be 0.  It should never be possible for there to be waiters, and a
  // positive count.
  DEBUG_ASSERT((count_ == 0) || waitq_.IsEmpty());

  // If we have no waiters, increment the count.  Otherwise, release a waiter.
  if (waitq_.IsEmpty()) {
    ++count_;
  } else {
    waitq_.WakeOne(ZX_OK);
  }
}

zx_status_t Semaphore::Wait(const Deadline& deadline) {
  Guard<MonitoredSpinLock, IrqSave> guard{ThreadLock::Get(), SOURCE_TAG};

  DEBUG_ASSERT((count_ == 0) || waitq_.IsEmpty());

  // If the count is positive, simply decrement it and get out.
  if (count_ > 0) {
    --count_;
    return ZX_OK;
  }

  // Wait in an interruptible state.  We will either be woken by a Post
  // operation, or by a timeout or signal.  Whatever happens, return the reason
  // the wait operation ended.
  return waitq_.Block(deadline, Interruptible::Yes);
}
