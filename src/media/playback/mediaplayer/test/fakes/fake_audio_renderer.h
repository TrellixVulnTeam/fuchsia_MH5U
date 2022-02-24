// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_TEST_FAKES_FAKE_AUDIO_RENDERER_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_TEST_FAKES_FAKE_AUDIO_RENDERER_H_

#include <fuchsia/media/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/fzl/vmo-mapper.h>
#include <lib/syslog/cpp/macros.h>

#include <memory>
#include <queue>
#include <vector>

#include "lib/fidl/cpp/binding.h"
#include "lib/fidl/cpp/binding_set.h"
#include "lib/media/cpp/timeline_function.h"
#include "src/media/playback/mediaplayer/test/fakes/packet_info.h"

namespace media_player {
namespace test {

// Implements AudioRenderer for testing.
class FakeAudioRenderer : public fuchsia::media::AudioRenderer,
                          public fuchsia::media::audio::GainControl {
 public:
  FakeAudioRenderer();

  ~FakeAudioRenderer() override;

  // Binds the renderer.
  void Bind(fidl::InterfaceRequest<fuchsia::media::AudioRenderer> request);

  // Indicates that the renderer should print out supplied packet info.
  void DumpPackets() { dump_packets_ = true; }

  // Indicates that the renderer should verify supplied packets against the
  // indicated PacketInfos.
  void ExpectPackets(const std::vector<PacketInfo>&& expected_packets_info) {
    packet_expecters_.emplace_back(std::move(expected_packets_info));
  }

  // Returns true if everything has gone as expected.
  bool expected() const;

  uint64_t received() const { return packets_received_; }

  float gain() const { return gain_; }
  bool mute() const { return mute_; }

  bool is_bound() const { return binding_.is_bound(); }

  // Sets a flag indicating whether this fake renderer should retain packets
  // (true) or retire them in a timeline manner (false).
  void SetRetainPackets(bool retain_packets) {
    retain_packets_ = retain_packets;
    if (!retain_packets_) {
      MaybeScheduleRetirement();
    }
  }

  void DelayPacketRetirement(int64_t packet_pts) { delay_packet_retirement_pts_ = packet_pts; }

  // AudioRenderer implementation.
  void SetPcmStreamType(fuchsia::media::AudioStreamType format) override;

  void AddPayloadBuffer(uint32_t id, zx::vmo payload_buffer) override;

  void RemovePayloadBuffer(uint32_t id) override;

  void SetPtsUnits(uint32_t tick_per_second_numerator,
                   uint32_t tick_per_second_denominator) override;

  void SetPtsContinuityThreshold(float threshold_seconds) override;

  void SetReferenceClock(zx::clock ref_clock) override;

  void GetReferenceClock(GetReferenceClockCallback callback) override;

  void SendPacket(fuchsia::media::StreamPacket packet, SendPacketCallback callback) override;

  void SendPacketNoReply(fuchsia::media::StreamPacket packet) override;

  void EndOfStream() final;

  void DiscardAllPackets(DiscardAllPacketsCallback callback) override;

  void DiscardAllPacketsNoReply() override;

  void Play(int64_t reference_time, int64_t media_time, PlayCallback callback) override;

  void PlayNoReply(int64_t reference_time, int64_t media_time) override;

  void Pause(PauseCallback callback) override;

  void PauseNoReply() override;

  void BindGainControl(fidl::InterfaceRequest<fuchsia::media::audio::GainControl> request) override;

  void EnableMinLeadTimeEvents(bool enabled) override;

  void GetMinLeadTime(GetMinLeadTimeCallback callback) override;

  void SetUsage(fuchsia::media::AudioRenderUsage usage) final { FX_NOTIMPLEMENTED(); }

  // GainControl interface.
  void SetGain(float gain_db) override;
  void SetGainWithRamp(float gain_db, zx_duration_t duration_ns,
                       fuchsia::media::audio::RampType ramp_type) final {
    FX_NOTIMPLEMENTED();
  }
  void SetMute(bool muted) override;

 private:
  class PacketExpecter {
   public:
    PacketExpecter(const std::vector<PacketInfo>&& info);

    bool IsExpected(const fuchsia::media::StreamPacket& packet, const void* start);

    bool done() const { return iter_ == info_.end(); }

    void LogExpectation() const;

   private:
    std::vector<PacketInfo> info_;
    std::vector<PacketInfo>::iterator iter_;
  };

  // Determines if we care currently playing.
  bool progressing() { return timeline_function_.invertible(); }

  // Schedules the retirement of the oldest queued packet if there are any
  // packets and if we're playing.
  void MaybeScheduleRetirement();

  async_dispatcher_t* dispatcher_;
  fidl::Binding<fuchsia::media::AudioRenderer> binding_;
  fidl::BindingSet<fuchsia::media::audio::GainControl> gain_control_bindings_;

  fuchsia::media::AudioStreamType format_;
  fzl::VmoMapper vmo_mapper_;
  float threshold_seconds_ = 0.0f;
  float gain_ = 1.0f;
  bool mute_ = false;
  const int64_t min_lead_time_ns_ = ZX_MSEC(100);
  media::TimelineRate pts_rate_ = media::TimelineRate::NsPerSecond;
  int64_t restart_media_time_ = fuchsia::media::NO_TIMESTAMP;
  bool retain_packets_ = false;
  int64_t delay_packet_retirement_pts_ = fuchsia::media::NO_TIMESTAMP;

  // Converts Reference time in ns units to presentation time in |pts_rate_|
  // units.
  media::TimelineFunction timeline_function_;

  bool dump_packets_ = false;
  uint64_t packets_received_;
  std::vector<PacketExpecter> packet_expecters_;

  std::queue<std::pair<fuchsia::media::StreamPacket, SendPacketCallback>> packet_queue_;

  bool expected_ = true;
};

}  // namespace test
}  // namespace media_player

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_TEST_FAKES_FAKE_AUDIO_RENDERER_H_
