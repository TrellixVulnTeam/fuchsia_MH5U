// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_PROCESS_CONFIG_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_PROCESS_CONFIG_H_

#include <lib/syslog/cpp/macros.h>

#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/media/audio/audio_core/device_config.h"
#include "src/media/audio/audio_core/loudness_transform.h"
#include "src/media/audio/audio_core/mix_profile_config.h"
#include "src/media/audio/audio_core/stream_usage.h"
#include "src/media/audio/audio_core/thermal_config.h"
#include "src/media/audio/audio_core/volume_curve.h"

namespace media::audio {

class ProcessConfig;

class ProcessConfigBuilder {
 public:
  ProcessConfigBuilder& SetDefaultVolumeCurve(VolumeCurve curve);
  ProcessConfigBuilder& AddDeviceProfile(
      std::pair<std::optional<std::vector<audio_stream_unique_id_t>>,
                DeviceConfig::OutputDeviceProfile>
          keyed_profile);
  ProcessConfigBuilder& AddDeviceProfile(
      std::pair<std::optional<std::vector<audio_stream_unique_id_t>>,
                DeviceConfig::InputDeviceProfile>
          keyed_profile);
  ProcessConfigBuilder& SetMixProfile(MixProfileConfig mix_profile_config);
  ProcessConfigBuilder& AddThermalPolicyEntry(ThermalConfig::Entry thermal_policy_entry);
  ProcessConfigBuilder& AddThermalNominalState(ThermalConfig::StateTransition nominal_state);
  ProcessConfig Build();

  VolumeCurve default_volume_curve() {
    return default_volume_curve_.value_or(
        VolumeCurve::DefaultForMinGain(VolumeCurve::kDefaultGainForMinVolume));
  }

 private:
  std::optional<VolumeCurve> default_volume_curve_;
  std::vector<std::pair<std::vector<audio_stream_unique_id_t>, DeviceConfig::OutputDeviceProfile>>
      output_device_profiles_;
  std::optional<DeviceConfig::OutputDeviceProfile> default_output_device_profile_;
  std::vector<std::pair<std::vector<audio_stream_unique_id_t>, DeviceConfig::InputDeviceProfile>>
      input_device_profiles_;
  std::optional<DeviceConfig::InputDeviceProfile> default_input_device_profile_;
  MixProfileConfig mix_profile_config_;
  std::vector<ThermalConfig::Entry> thermal_config_entries_;
  std::vector<ThermalConfig::StateTransition> thermal_nominal_states_;
};

class ProcessConfig {
 public:
  class HandleImpl {
   public:
    ~HandleImpl() { ProcessConfig::instance_ = {}; }

    // Disallow copy/move.
    HandleImpl(const HandleImpl&) = delete;
    HandleImpl& operator=(const HandleImpl&) = delete;
    HandleImpl(HandleImpl&& o) = delete;
    HandleImpl& operator=(HandleImpl&& o) = delete;

   private:
    friend class ProcessConfig;
    HandleImpl() = default;
  };
  using Handle = std::unique_ptr<HandleImpl>;

  // Sets the |ProcessConfig|.
  //
  // |ProcessConfig::instance()| will return a reference to |config| as long as the returned
  // |ProcessConfig::Handle| exists. It's illegal to call |set_instance| while a
  // |ProcessConfig::Handle| is active.
  [[nodiscard]] static ProcessConfig::Handle set_instance(ProcessConfig config) {
    FX_CHECK(!ProcessConfig::instance_);
    ProcessConfig::instance_ = {std::move(config)};
    return std::unique_ptr<HandleImpl>(new HandleImpl);
  }
  // Returns the |ProcessConfig|. Must be called while there is a
  static const ProcessConfig& instance() {
    FX_CHECK(ProcessConfig::instance_);
    return *ProcessConfig::instance_;
  }

  using Builder = ProcessConfigBuilder;
  ProcessConfig(VolumeCurve curve, DeviceConfig device_config, MixProfileConfig mix_profile_config,
                ThermalConfig thermal_config)
      : default_volume_curve_(std::move(curve)),
        default_loudness_transform_(
            std::make_shared<MappedLoudnessTransform>(default_volume_curve_)),
        device_config_(std::move(device_config)),
        mix_profile_config_(mix_profile_config),
        thermal_config_(std::move(thermal_config)) {}

  const VolumeCurve& default_volume_curve() const { return default_volume_curve_; }
  const DeviceConfig& device_config() const { return device_config_; }
  const MixProfileConfig& mix_profile_config() const { return mix_profile_config_; }
  const ThermalConfig& thermal_config() const { return thermal_config_; }
  const std::shared_ptr<LoudnessTransform>& default_loudness_transform() const {
    return default_loudness_transform_;
  }

 private:
  static std::optional<ProcessConfig> instance_;

  VolumeCurve default_volume_curve_;
  std::shared_ptr<LoudnessTransform> default_loudness_transform_;
  DeviceConfig device_config_;
  MixProfileConfig mix_profile_config_;
  ThermalConfig thermal_config_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_PROCESS_CONFIG_H_
