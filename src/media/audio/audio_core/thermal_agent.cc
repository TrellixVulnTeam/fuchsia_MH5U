// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/thermal_agent.h"

#include <lib/syslog/cpp/macros.h>

#include <unordered_set>

#include <rapidjson/document.h>

#include "src/media/audio/audio_core/audio_device_manager.h"
#include "src/media/audio/audio_core/device_config.h"
#include "src/media/audio/audio_core/reporter.h"

namespace media::audio {
namespace {

// Finds the nominal config string for the specified target. Returns no value if the specified
// target could not be found.
std::optional<std::string> FindNominalConfigForTarget(
    const std::vector<ThermalConfig::StateTransition>& nominal_states,
    const std::string& target_name, const DeviceConfig& device_config) {
  // First check if target is present in v1 effects list.
  // TODO(fxbug.dev/80067) This will be removed when we have transitioned to looking up nominal
  // config directly from ThermalConfig.
  const PipelineConfig::EffectV1* effect = device_config.FindEffectV1(target_name);
  if (effect) {
    return effect->effect_config;
  }

  // Then look in ThermalConfig
  for (auto& s : nominal_states) {
    if (s.target_name() == target_name) {
      return s.config();
    }
  }

  return std::nullopt;
}

// Constructs a map {target_name: configs_by_thermal_state}, where configs_by_thermal_state
// is a vector of configurations for the target indexed by thermal state.
std::unordered_map<std::string, std::vector<std::string>> PopulateTargetConfigurations(
    const ThermalConfig& thermal_config, const DeviceConfig& device_config) {
  const auto& entries = thermal_config.entries();
  const size_t num_thermal_states = entries.size() + 1;
  std::unordered_map<std::string, std::vector<std::string>> result;

  Reporter::Singleton().SetNumThermalStates(num_thermal_states);

  // "Bad" targets have no nominal configuration. We record them so the name of every such target
  // can be logged only once.
  std::unordered_set<std::string> bad_targets;

  for (size_t i = 0; i < entries.size(); i++) {
    const auto& entry = entries[i];

    for (const auto& transition : entry.state_transitions()) {
      const auto& target_name = transition.target_name();
      if (bad_targets.find(target_name) != bad_targets.end()) {
        continue;
      }

      auto configs_it = result.find(target_name);

      // This target isn't in target_configurations. If there's no corresponding nominal config,
      // record it as a bad target and continue. Otherwise, initialize this target's entry in
      // `result`.
      if (configs_it == result.end()) {
        auto nominal_config =
            FindNominalConfigForTarget(thermal_config.nominal_states(), target_name, device_config);
        if (!nominal_config.has_value()) {
          bad_targets.insert(target_name);
          FX_LOGS(ERROR) << "Thermal config references unknown target '" << target_name << "'.";
          continue;
        }

        configs_it = result.insert({target_name, {}}).first;
        auto& configs = configs_it->second;
        configs.reserve(num_thermal_states);
        configs.push_back(nominal_config.value());
      }

      // `transition` specifies that this target should change from its previous configuration at
      // state `i` to `transition.config()` at state `i+1`. Copy the last element until entry `i`
      // is populated, and then copy the new config into position `i+1`.
      std::vector<std::string>& configs = configs_it->second;
      for (size_t j = configs.size(); j < i + 1; j++) {
        configs.push_back(configs.back());
      }
      configs.push_back(transition.config());
    }
  }

  // Extend the configs for each target to the appropriate length -- any target not present in the
  // final state transition will have missing elements.
  for (auto& entry : result) {
    auto& configs = entry.second;
    if (configs.size() < num_thermal_states) {
      for (size_t j = configs.size(); j < num_thermal_states + 1; j++) {
        configs.push_back(configs.back());
      }
    }
  }

  return result;
}

}  // namespace

// static
std::unique_ptr<ThermalAgent> ThermalAgent::CreateAndServe(Context* context) {
  auto& thermal_config = context->process_config().thermal_config();
  if (thermal_config.entries().empty()) {
    FX_LOGS(INFO) << "No thermal config found, so we won't start the thermal agent";
    return nullptr;
  }

  return std::make_unique<ThermalAgent>(
      context->component_context().svc()->Connect<fuchsia::thermal::Controller>(), thermal_config,
      context->process_config().device_config(),
      [context](const std::string& target_name, const std::string& config) {
        async::PostTask(
            context->threading_model().FidlDomain().dispatcher(),
            [context, instance = target_name, config = config]() {
              context->effects_controller()->UpdateEffect(
                  instance, config,
                  [instance = instance, config = config](
                      fuchsia::media::audio::EffectsController_UpdateEffect_Result result) {
                    if (result.is_err()) {
                      std::ostringstream err;
                      if (result.err() == fuchsia::media::audio::UpdateEffectError::NOT_FOUND) {
                        err << "effect with name " << instance << " was not found";
                      } else {
                        err << "message " << config << " was rejected";
                      }
                      FX_LOGS_FIRST_N(ERROR, 10) << "Unable to apply thermal policy: " << err.str();
                    }
                  });
            });
      });
}

ThermalAgent::ThermalAgent(fuchsia::thermal::ControllerPtr thermal_controller,
                           const ThermalConfig& thermal_config, const DeviceConfig& device_config,
                           SetConfigCallback set_config_callback)
    : thermal_controller_(std::move(thermal_controller)),
      binding_(this),
      set_config_callback_(std::move(set_config_callback)) {
  FX_DCHECK(thermal_controller_);
  FX_DCHECK(set_config_callback_);

  TRACE_DURATION_BEGIN("audio", "ThermalState_0");

  if (thermal_config.entries().empty()) {
    FX_LOGS(ERROR) << "No thermal config, so we won't start the thermal agent";
    thermal_controller_ = nullptr;
    return;
  }

  targets_ = PopulateTargetConfigurations(thermal_config, device_config);

  thermal_controller_.set_error_handler([this](zx_status_t status) {
    FX_PLOGS(ERROR, status) << "Connection to fuchsia.thermal.Controller failed: ";
    thermal_controller_.set_error_handler(nullptr);
    thermal_controller_.Unbind();
  });

  std::vector<fuchsia::thermal::TripPoint> trip_points;
  trip_points.reserve(thermal_config.entries().size());
  for (const auto& entry : thermal_config.entries()) {
    trip_points.push_back(entry.trip_point());
  }

  thermal_controller_->Subscribe(
      binding_.NewBinding(), fuchsia::thermal::ActorType::AUDIO, std::move(trip_points),
      [this](fuchsia::thermal::Controller_Subscribe_Result result) {
        if (result.is_err()) {
          FX_CHECK(result.err() != fuchsia::thermal::Error::INVALID_ARGUMENTS);
          FX_LOGS(ERROR) << "fuchsia.thermal.Controller/Subscribe failed";
        }

        thermal_controller_.set_error_handler(nullptr);
        thermal_controller_.Unbind();
      });
}

namespace {
std::optional<std::string> ParseThermalConfigComment(std::string& config) {
  rapidjson::Document doc;
  rapidjson::ParseResult result = doc.Parse(config);
  if (!result.IsError() && doc["_comment"].IsString()) {
    return doc["_comment"].GetString();
  } else {
    return std::nullopt;
  }
}
}  // namespace

// Handle a thermal state change from fuchsia::thermal::Controller.
// After doing the actual work, update our telemetry and invoke the FIDL completion.
void ThermalAgent::SetThermalState(uint32_t state, SetThermalStateCallback callback) {
  if (current_state_ == state) {
    callback();
    FX_LOGS(INFO) << "No thermal state change (was already " << state << ")";
    return;
  }

  TRACE_DURATION_END("audio",
                     std::string("ThermalState_" + std::to_string(current_state_)).c_str());
  TRACE_DURATION_BEGIN("audio", std::string("ThermalState_" + std::to_string(state)).c_str());

  for (auto& [target_name, configs_by_state] : targets_) {
    FX_CHECK(state < configs_by_state.size());
    FX_CHECK(current_state_ < configs_by_state.size());
    if (configs_by_state[state] != configs_by_state[current_state_]) {
      auto comment = ParseThermalConfigComment(configs_by_state[state]);
      FX_LOGS(INFO) << "Set thermal state to " << state << (comment ? " - " + comment.value() : "");
      set_config_callback_(target_name, configs_by_state[state]);
    }
  }

  auto previous_state = current_state_;
  current_state_ = state;

  Reporter::Singleton().SetThermalState(state);

  callback();
  FX_LOGS(INFO) << "Thermal state change (from " << previous_state << " to " << state
                << ") is complete";
}

}  // namespace media::audio
