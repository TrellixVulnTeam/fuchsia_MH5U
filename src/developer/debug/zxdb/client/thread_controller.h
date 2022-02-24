// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_THREAD_CONTROLLER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_THREAD_CONTROLLER_H_

#include <vector>

#include "lib/fit/function.h"
#include "src/developer/debug/ipc/protocol.h"
#include "src/developer/debug/zxdb/client/frame_fingerprint.h"
#include "src/developer/debug/zxdb/common/address_range.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace zxdb {

class Breakpoint;
class Err;
class Frame;
class Location;
class Thread;

// Abstract base class that provides the policy decisions for various types of thread stepping.
//
// Once installed, the thread will ask the topmost thread controller how (and whether) to continue.
// All thread controllers installed on a thread will get notified for each exception and indicate
// whether they want to handle the stop or continue. Each thread controller is queried for each
// stop since completions could happen in the in any order.
//
// The thread may also delete thread controllers. This can happen when the thread is terminated or
// when there is an internal error stepping. If a controller has a callback it executes on
// completion it should be prepared to issue the callback from its destructor in such a way to
// indicate that the step operation failed.
//
// Thread controllers run synchronously. This is sometimes limiting but otherwise some logic would
// be very difficult to follow. This means that the thread controller can't request memory and
// do something different based on that. There is some opportunity for asynchronous work via the
// Thread's AddPostStopTask() function. This can inject asynchronous work after the thread
// controllers run but before the stop or continue is processed.
class ThreadController {
 public:
  enum StopOp {
    // Resume the thread. A controller can indicate "continue" but if another indicates "stop", the
    // "stop" will take precedence.
    kContinue,

    // Keeps the thread stopped and reports the stop to the user. The controller is marked done and
    // should be deleted. This takes precedence over any "continue" votes.
    kStopDone,

    // Reports that the controller doesn't know what to do with this thread stop. This is
    // effectively a neutral vote for what should happen in response to a thread stop. If all active
    // controllers report "unexpected", the thread will stop.
    kUnexpected
  };

  // How the thread should run when it is executing this controller.
  struct ContinueOp {
    // Factory helper functions.
    static ContinueOp Continue() {
      return ContinueOp();  // Defaults are good for this case.
    }
    static ContinueOp StepInstruction() {
      ContinueOp result;
      result.how = debug_ipc::ResumeRequest::How::kStepInstruction;
      return result;
    }
    static ContinueOp StepInRange(AddressRange range) {
      ContinueOp result;
      result.how = debug_ipc::ResumeRequest::How::kStepInRange;
      result.range = range;
      return result;
    }
    // See synthetic_stop_ below.
    static ContinueOp SyntheticStop() {
      ContinueOp result;
      result.synthetic_stop_ = true;
      return result;
    }

    // A synthetic stop means that the thread remains stopped but a synthetic stop notification is
    // broadcast to make it look like the thread did continued and stopped again. This will call
    // back into the top controller's OnThreadStop().
    //
    // This is useful when modifying the stack for inline routines, where the code didn't execute
    // but from a user perspective they stepped into an inline subroutine. In this case the thread
    // controller will update the Stack to reflect the new state, and return
    // ContinueOp::SyntheticStop().
    //
    // Why isn't this a StopOp instead? This only makes sense as the initial state of the
    // ThreadController that decides it doesn't need to do anything but wants to pretend that it
    // did. When a ThreadController is in OnThreadStop and about to return a StopOp, returning kStop
    // is a real thread stop and nothing needs to be synthetic.
    //
    // See GetContinueOp() for more.
    bool synthetic_stop_ = false;

    // Valid when synthetic_stop = false.
    debug_ipc::ResumeRequest::How how = debug_ipc::ResumeRequest::How::kResolveAndContinue;

    // When how == kStepInRange, this defines the address range to step in. As long as the
    // instruction pointer is inside, execution will continue.
    AddressRange range;
  };

  ThreadController();

  virtual ~ThreadController();

  // Registers the thread with the controller. The controller will be owned by the thread (possibly
  // indirectly) so the pointer will remain valid for the rest of the lifetime of the controller.
  //
  // The implementation should call SetThread() with the thread.
  //
  // When the implementation is ready, it will issue the given callback to run the thread. The
  // callback can be issued reentrantly from inside this function if the controller is ready
  // or fails synchronously.
  //
  // If the callback does not specify an error, the thread will be resumed when it is called. If the
  // callback has an error, it will be reported and the thread will remain stopped.
  virtual void InitWithThread(Thread* thread, fit::callback<void(const Err&)> cb) = 0;

  // Returns how to continue the thread when running this controller. This will be called after
  // InitWithThread and after every subsequent kContinue response from OnThreadStop to see how the
  // controller wishes to run.
  //
  // A thread controller can return a "synthetic stop" from this function which will schedule an
  // OnThreadStop() call in the future without running the thread. This can be used to adjust the
  // ambiguous inline stack state (see Stack object) to implement step commands.
  //
  // GetContinueOp() should not change thread state and controllers should be prepared for only
  // InitWithThread() followe by OnThreadStop() calls. When thread controllers embed other thread
  // controllers, the embedding controller may create the nested one and want it to evaluate the
  // current stop, and this happens without ever continuing.
  virtual ContinueOp GetContinueOp() = 0;

  // Notification that the thread has stopped. The return value indicates what the thread should do
  // in response.
  //
  // At this call, the stop location will be thread().GetStack()[0]. Thread controllers will only
  // be called when there is a valid location for the stop, so there is guaranteed to be at least
  // one stack entry (in constrast to general thread exception observers).
  //
  // ARGUMENTS
  // ---------
  // The exception type may be "kNone" if the exception type shouldn't matter to this controller.
  // Controllers should treak "kNone" as being relevant to themselves. When a controller is used as
  // a component of another controller, the exception type may have been "consumed" and a nested
  // controller merely needs to evaluate its opinion of the current location.
  //
  // The stop type and breakpoint information should be passed to the first thread controller that
  // handles the stop (this might be a sub controller if a controller is delegating the current
  // execution to another one). Other controllers that might handle the stop (say, if a second
  // sub-controller is created when the first one is done) don't care and might get confused by stop
  // information originally handled by another one. In this second case, "kNone" and an empty
  // breakpoint list should be sent to OnThreadStop().
  //
  // RETURN VALUE
  // ------------
  // If the ThreadController returns |kStop|, its assumed the controller has completed its job and
  // it will be deleted. |kContinue| doesn't necessarily mean the thread will continue, as there
  // could be multiple controllers active and any of them can report "stop". When a thread is being
  // continued, the main controller will get GetContinueOp() called to see what type of continuation
  // it wants.
  virtual StopOp OnThreadStop(debug_ipc::ExceptionType stop_type,
                              const std::vector<fxl::WeakPtr<Breakpoint>>& hit_breakpoints) = 0;

  // Writes the log message prefixed with the thread controller type. Callers should pass constant
  // strings through here so the Log function takes almost no time if it's disabled: in the future
  // we may want to make this run-time enable-able
  void Log(const char* format, ...) const;

  // Returns the given frame's function name or a placeholder string if unavailable. Does nothing if
  // logging is disabled (computing this is non-trivial).
  static std::string FrameFunctionNameForLog(const Frame* frame);

 protected:
  // How the frame argument to SetInlineFrameIfAmbiguous() is interpreted.
  enum class InlineFrameIs {
    // Set the inline frame equal to the given one.
    kEqual,

    // Set the inline frame to the frame immediately before the given one. This exists so that
    // calling code can reference the previuos frame without actually having to compute the
    // fingerprint of the previous frame (it may not be available if previous stack frames haven't
    // been synced).
    kOneBefore
  };

  Thread* thread() { return thread_; }
  void SetThread(Thread* thread);

  // Returns the name of this thread controller. This will be visible in logs. This should be
  // something simple and short like "Step" or "Step Over".
  virtual const char* GetName() const = 0;

  // The beginning of an inline function is ambiguous about whether you're at the beginning of the
  // function or about to call it (see Stack object for more).
  //
  // Many stepping functions know what frame they think they should be in, and identify this based
  // on the frame fingerprint. As a concrete example, if a "finish" command exits a stack frame, but
  // the next instruction is the beginning of an inlined function, the "finish" controller would
  // like to say you're in the stack it returned to, not the inlined function.
  //
  // This function checks if there is ambiguity of inline frames and whether one of those ambiguous
  // frames matches the given fingerprint. In this case, it will set the top stack frame to be the
  // requested one.
  //
  // If there is no ambiguity or one of the possibly ambiguous frames doesn't match the given
  // fingerprint, the inline frame hide count will be unchanged.
  void SetInlineFrameIfAmbiguous(InlineFrameIs comparison, FrameFingerprint fingerprint);

  // Tells the owner of this class that this ThreadController has completed its work. Normally
  // returning kStop from OnThreadStop() will do this, but if the controller has another way to get
  // events (like breakpoints), it may notice out-of-band that its work is done.
  //
  // This function will likely cause |this| to be deleted.
  void NotifyControllerDone();

  // Returns true if this controller has debug logging enabled. This is only valid after the thread
  // has been set.
  bool enable_debug_logging() const { return enable_debug_logging_; }

 private:
  Thread* thread_ = nullptr;

  // Initialized from the setting when the thread is known.
  bool enable_debug_logging_ = false;

  FXL_DISALLOW_COPY_AND_ASSIGN(ThreadController);
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_CLIENT_THREAD_CONTROLLER_H_
