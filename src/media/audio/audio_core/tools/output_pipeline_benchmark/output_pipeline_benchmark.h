// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_TOOLS_OUTPUT_PIPELINE_BENCHMARK_OUTPUT_PIPELINE_BENCHMARK_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_TOOLS_OUTPUT_PIPELINE_BENCHMARK_OUTPUT_PIPELINE_BENCHMARK_H_

#include <optional>

#include <perftest/results.h>

#include "gperftools/profiler.h"
#include "lib/syslog/cpp/macros.h"
#include "src/media/audio/audio_core/output_pipeline.h"
#include "src/media/audio/audio_core/process_config.h"
#include "src/media/audio/audio_core/stream.h"
#include "src/media/audio/audio_core/stream_usage.h"
#include "src/media/audio/lib/analysis/generators.h"
#include "src/media/audio/lib/clock/audio_clock.h"
#include "src/media/audio/lib/clock/testing/fake_audio_clock_factory.h"
#include "src/media/audio/lib/effects_loader/effects_loader_v2.h"
#include "src/media/audio/lib/format/audio_buffer.h"

namespace media::audio {

class OutputPipelineBenchmark {
 public:
  struct Input {
    RenderUsage usage;
    uint32_t fps;
    uint32_t channels;

    std::string ToString() const;
    static Input FromString(const std::string& str);
  };

  enum class VolumeSetting {
    Muted,       // muted for the entire run
    Constant,    // constant, not-muted volume for the entire run
    StepChange,  // discrete volume change just before each mix job
    RampChange,  // ramped volume change just before each mix job
  };

  struct Scenario {
    std::vector<Input> inputs;
    VolumeSetting volume = VolumeSetting::Constant;

    std::string ToString() const;
    static Scenario FromString(const std::string& str);
  };

  explicit OutputPipelineBenchmark(sys::ComponentContext& context)
      : context_(context),
        output_pipeline_(CreateOutputPipeline(CreateEffectsLoaderV2(context_))) {}

  void PrintLegend(zx::duration mix_period);

  // Create inputs for the given scenario, then repeatedly call output_pipeline_->ReadLock
  // until at least min_duration real time has passed.
  void Run(Scenario scenario, int64_t runs_per_scenario, zx::duration mix_period,
           perftest::ResultsSet* results, bool print_summary);

  const ProcessConfig& process_config() const {
    FX_CHECK(process_config_.has_value());
    return *process_config_;
  }

 private:
  std::shared_ptr<OutputPipeline> CreateOutputPipeline(
      std::unique_ptr<EffectsLoaderV2> effects_loader_v2);
  std::shared_ptr<ReadableStream> CreateInput(const Input& input);
  static std::unique_ptr<EffectsLoaderV2> CreateEffectsLoaderV2(sys::ComponentContext& context);

  std::shared_ptr<testing::FakeAudioClockFactory> clock_factory_ =
      std::make_shared<testing::FakeAudioClockFactory>();
  std::unique_ptr<AudioClock> device_clock_ =
      clock_factory_->CreateDeviceFixed(zx::time(0), 0, AudioClock::kMonotonicDomain);

  sys::ComponentContext& context_;
  std::unique_ptr<EffectsLoaderV2> effects_loader_v2_;
  std::shared_ptr<OutputPipeline> output_pipeline_;
  std::optional<ProcessConfig> process_config_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_TOOLS_OUTPUT_PIPELINE_BENCHMARK_OUTPUT_PIPELINE_BENCHMARK_H_
