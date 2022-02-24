// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/gesture_manager/arena/gesture_arena.h"

#include <lib/syslog/cpp/macros.h>

#include <utility>

#include "src/ui/a11y/lib/gesture_manager/arena/recognizer.h"

namespace a11y {

namespace {

using AccessibilityPointerEvent = fuchsia::ui::input::accessibility::PointerEvent;
using Phase = fuchsia::ui::input::PointerEventPhase;

}  // namespace

PointerStreamTracker::PointerStreamTracker(OnStreamHandledCallback on_stream_handled_callback)
    : on_stream_handled_callback_(std::move(on_stream_handled_callback)) {
  FX_DCHECK(on_stream_handled_callback_);
}

void PointerStreamTracker::Reset() {
  handled_.reset();
  pointer_event_callbacks_.clear();
  active_streams_.clear();
}

void PointerStreamTracker::RejectPointerEvents() {
  InvokePointerEventCallbacks(fuchsia::ui::input::accessibility::EventHandling::REJECTED);
  // It is also necessary to clear the active streams, because as they were rejected, the input
  // system will not send the rest of the stream to us.
  active_streams_.clear();
}

void PointerStreamTracker::ConsumePointerEvents() {
  InvokePointerEventCallbacks(fuchsia::ui::input::accessibility::EventHandling::CONSUMED);
}

void PointerStreamTracker::InvokePointerEventCallbacks(
    fuchsia::ui::input::accessibility::EventHandling handled) {
  handled_ = handled;

  for (const auto& kv : pointer_event_callbacks_) {
    const auto [device_id, pointer_id] = kv.first;
    for (uint32_t times = 1; times <= kv.second; ++times) {
      on_stream_handled_callback_(device_id, pointer_id, handled);
    }
  }
  pointer_event_callbacks_.clear();
}

void PointerStreamTracker::OnEvent(const AccessibilityPointerEvent& pointer_event) {
  // Note that at some point we must answer whether the pointer event stream was
  // consumed / rejected. For this reason, for each ADD event we store the
  // callback that will be responsible for signaling how the events were
  // handled. It is important also to mention that for now, is all or nothing:
  // consume or reject all pointer events that were sent to the arena, and not
  // per a pointer event ID basis. Although this can be implemented, there is no
  // use case for this right now.
  const StreamID stream_id(pointer_event.device_id(), pointer_event.pointer_id());
  switch (pointer_event.phase()) {
    case Phase::ADD: {
      if (handled_) {
        on_stream_handled_callback_(pointer_event.device_id(), pointer_event.pointer_id(),
                                    *handled_);
      } else {
        pointer_event_callbacks_[stream_id]++;
      }
      active_streams_.insert(stream_id);
      break;
    }
    case Phase::REMOVE:
      active_streams_.erase(stream_id);
      break;
    default:
      break;
  };
}

// Represents a contest member in an arena.
//
// The member is able to affect its state so long as the arena exists and |Accept| or |Reject| has
// not already been called. The associated recognizer receives pointer events so long as this
// |ContestMember| remains alive and not defeated.
//
// Keep in mind that non-|ContestMember| methods are not visible outside of |GestureArena|.
class GestureArena::ArenaContestMember : public ContestMember {
 public:
  ArenaContestMember(fxl::WeakPtr<GestureArena> arena, ArenaMember* arena_member)
      : arena_(arena), arena_member_(arena_member), weak_ptr_factory_(this) {
    FX_DCHECK(arena_member_);
  }

  ~ArenaContestMember() override {
    Reject();  // no-op if unnecessary
  }

  fxl::WeakPtr<ArenaContestMember> GetWeakPtr() { return weak_ptr_factory_.GetWeakPtr(); }

  GestureRecognizer* recognizer() const { return arena_member_->recognizer; }

  // |ContestMember|
  void Accept() override {
    if (arena_ && arena_member_->status == Status::kUndecided) {
      arena_member_->status = Status::kAccepted;
      arena_->HandleEvents(true);
      // Do |FinalizeState| last in case it releases this member.
      FinalizeState();
    }
  }

  // |ContestMember|
  void Reject() override {
    if (arena_ && arena_member_->status == Status::kUndecided) {
      arena_member_->status = Status::kRejected;
      weak_ptr_factory_.InvalidateWeakPtrs();
      // |FinalizeState| won't affect us since we didn't claim a win.
      FinalizeState();
      // On the other hand, do |OnDefeat| last in case it releases this member.
      recognizer()->OnDefeat();
    }
  }

 private:
  void FinalizeState() {
    FX_DCHECK(arena_->undecided_members_);
    --arena_->undecided_members_;
    arena_->TryToResolve();
  }

  fxl::WeakPtr<GestureArena> arena_;
  ArenaMember* const arena_member_;

  fxl::WeakPtrFactory<ArenaContestMember> weak_ptr_factory_;
};

GestureArena::GestureArena(PointerStreamTracker::OnStreamHandledCallback on_stream_handled_callback)
    : streams_(std::move(on_stream_handled_callback)), weak_ptr_factory_(this) {}

void GestureArena::Add(GestureRecognizer* recognizer) {
  // Initialize status to |kRejected| rather than |kUndecided| just for peace of mind for the case
  // where we add while active. Really, since we use a counter for undecided members, this could be
  // either, just not |kAccepted|.
  arena_members_.push_back({.recognizer = recognizer, .status = ContestMember::Status::kRejected});
}

// Possible |Remove| implementation:
// fxr/c/fuchsia/+/341227/11/src/ui/a11y/lib/gesture_manager/arena/gesture_arena.cc#151

void GestureArena::OnEvent(const fuchsia::ui::input::accessibility::PointerEvent& pointer_event) {
  FX_CHECK(!arena_members_.empty()) << "The a11y Gesture arena is listening for pointer events "
                                       "but has no added gesture recognizer.";
  if (IsIdle()) {
    // An idle arena received a new event. Starts a new contest.
    StartNewContest();
  }

  streams_.OnEvent(pointer_event);
  DispatchEvent(pointer_event);
}

void GestureArena::TryToResolve() {
  if (undecided_members_ == 0) {
    bool winner_assigned = false;
    for (auto& member : arena_members_) {
      if (member.status == ContestMember::Status::kAccepted) {
        if (winner_assigned) {
          member.recognizer->OnDefeat();
        } else {
          winner_assigned = true;
          FX_LOGS(INFO) << "Gesture Arena: " << member.recognizer->DebugName() << " Won.";
          member.recognizer->OnWin();
        }
      }
    }

    if (!winner_assigned) {
      HandleEvents(false);
    }
  }
}

void GestureArena::DispatchEvent(
    const fuchsia::ui::input::accessibility::PointerEvent& pointer_event) {
  for (auto& member : arena_members_) {
    if (member.contest_member) {
      member.recognizer->HandleEvent(pointer_event);
    }
  }
}

void GestureArena::StartNewContest() {
  weak_ptr_factory_.InvalidateWeakPtrs();
  streams_.Reset();

  undecided_members_ = arena_members_.size();

  for (auto& member : arena_members_) {
    member.status = ContestMember::Status::kUndecided;
    auto contest_member =
        std::make_unique<ArenaContestMember>(weak_ptr_factory_.GetWeakPtr(), &member);
    member.contest_member = contest_member->GetWeakPtr();
    member.recognizer->OnContestStarted(std::move(contest_member));
  }
}

void GestureArena::HandleEvents(bool consumed_by_member) {
  if (consumed_by_member) {
    streams_.ConsumePointerEvents();
  } else {
    streams_.RejectPointerEvents();
  }
}

bool GestureArena::IsHeld() const {
  for (const auto& member : arena_members_) {
    if (member.contest_member) {
      return true;
    }
  }
  return false;
}

bool GestureArena::IsIdle() const { return !(streams_.is_active() || IsHeld()); }

}  // namespace a11y
