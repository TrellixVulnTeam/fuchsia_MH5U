// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "weave_config_manager.h"

#include <lib/syslog/cpp/macros.h>

#include <vector>

#include <Weave/Support/NLDLLUtil.h>

#include "rapidjson/schema.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "src/lib/files/file.h"
#include "src/lib/json_parser/json_parser.h"
#include "third_party/modp_b64/modp_b64.h"
#include "weave_device_platform_error.h"

namespace nl::Weave::DeviceLayer::Internal {

NL_DLL_EXPORT
const char kEnvironmentStorePath[] = "/data/environment.json";

WeaveConfigManager& WeaveConfigManager::GetInstance() {
  static WeaveConfigManager manager;
  return manager;
}

WeaveConfigManager::WeaveConfigManager() : WeaveConfigManager(kEnvironmentStorePath) {}

WeaveConfigManager::WeaveConfigManager(std::string path) : config_store_path_(std::move(path)) {
  json::JSONParser json_parser_;
  if (files::IsFile(config_store_path_)) {
    config_ = json_parser_.ParseFromFile(config_store_path_);
    FX_DCHECK(!json_parser_.HasError())
        << "Failed to load configuration: " << json_parser_.error_str();
  } else {
    config_.SetObject();
  }
}

WEAVE_ERROR WeaveConfigManager::ReadConfigValue(const std::string& key, bool* value) const {
  rapidjson::Value config_value;
  WEAVE_ERROR error = WEAVE_NO_ERROR;
  if ((error = ReadKVPair(key, config_value)) != WEAVE_NO_ERROR) {
    return error;
  }
  if (!config_value.IsBool()) {
    return WEAVE_DEVICE_PLATFORM_ERROR_CONFIG_TYPE_MISMATCH;
  }
  *value = config_value.GetBool();
  return WEAVE_NO_ERROR;
}

WEAVE_ERROR WeaveConfigManager::ReadConfigValue(const std::string& key, uint16_t* value) const {
  uint32_t int_value;
  WEAVE_ERROR error = ReadConfigValue(key, &int_value);
  if (error != WEAVE_NO_ERROR) {
    return error;
  }
  if (int_value > UINT16_MAX) {
    return WEAVE_DEVICE_ERROR_CONFIG_NOT_FOUND;
  }
  *value = static_cast<uint16_t>(int_value);
  return WEAVE_NO_ERROR;
}

WEAVE_ERROR WeaveConfigManager::ReadConfigValue(const std::string& key, uint32_t* value) const {
  rapidjson::Value config_value;
  WEAVE_ERROR error = WEAVE_NO_ERROR;
  if ((error = ReadKVPair(key, config_value)) != WEAVE_NO_ERROR) {
    return error;
  }
  if (!config_value.IsUint()) {
    return WEAVE_DEVICE_PLATFORM_ERROR_CONFIG_TYPE_MISMATCH;
  }
  *value = config_value.GetUint();
  return WEAVE_NO_ERROR;
}

WEAVE_ERROR WeaveConfigManager::ReadConfigValue(const std::string& key, uint64_t* value) const {
  rapidjson::Value config_value;
  WEAVE_ERROR error = WEAVE_NO_ERROR;
  if ((error = ReadKVPair(key, config_value)) != WEAVE_NO_ERROR) {
    return error;
  }
  if (!config_value.IsUint64()) {
    return WEAVE_DEVICE_PLATFORM_ERROR_CONFIG_TYPE_MISMATCH;
  }
  *value = config_value.GetUint64();
  return WEAVE_NO_ERROR;
}

WEAVE_ERROR WeaveConfigManager::ReadConfigValueStr(const std::string& key, char* value,
                                                   size_t value_size, size_t* out_size) const {
  rapidjson::Value config_value;
  WEAVE_ERROR error = WEAVE_NO_ERROR;
  if ((error = ReadKVPair(key, config_value)) != WEAVE_NO_ERROR) {
    return error;
  }
  if (!config_value.IsString()) {
    return WEAVE_DEVICE_PLATFORM_ERROR_CONFIG_TYPE_MISMATCH;
  }
  const std::string string_value(config_value.GetString());
  *out_size = string_value.size();
  if (value == nullptr) {
    return WEAVE_NO_ERROR;
  }
  if (value_size < (*out_size + 1)) {
    return WEAVE_ERROR_BUFFER_TOO_SMALL;
  }
  strncpy(value, string_value.c_str(), *out_size);
  value[*out_size] = 0;
  return WEAVE_NO_ERROR;
}

WEAVE_ERROR WeaveConfigManager::ReadConfigValueBin(const std::string& key, uint8_t* value,
                                                   size_t value_size, size_t* out_size) const {
  rapidjson::Value config_value;
  WEAVE_ERROR error = WEAVE_NO_ERROR;
  if ((error = ReadKVPair(key, config_value)) != WEAVE_NO_ERROR) {
    return error;
  }
  if (!config_value.IsString()) {
    return WEAVE_DEVICE_PLATFORM_ERROR_CONFIG_TYPE_MISMATCH;
  }
  std::string string_value(config_value.GetString());
  const std::string decoded_value(modp_b64_decode(string_value));
  *out_size = decoded_value.size();
  if (value == nullptr) {
    return WEAVE_NO_ERROR;
  }
  if (value_size < *out_size) {
    return WEAVE_ERROR_BUFFER_TOO_SMALL;
  }
  memcpy(value, decoded_value.c_str(), decoded_value.size());
  return WEAVE_NO_ERROR;
}

WEAVE_ERROR WeaveConfigManager::ReadConfigValueArray(const std::string& key,
                                                     std::vector<std::string>& out) const {
  rapidjson::Value config_value;
  WEAVE_ERROR error = WEAVE_NO_ERROR;
  if ((error = ReadKVPair(key, config_value)) != WEAVE_NO_ERROR) {
    return error;
  }
  if (!config_value.IsArray()) {
    return WEAVE_DEVICE_PLATFORM_ERROR_CONFIG_TYPE_MISMATCH;
  }

  for (rapidjson::SizeType i = 0; i < config_value.Size(); i++) {
    if (!config_value[i].IsString()) {
      return WEAVE_DEVICE_PLATFORM_ERROR_CONFIG_TYPE_MISMATCH;
    }
    out.push_back(config_value[i].GetString());
  }
  return WEAVE_NO_ERROR;
}

WEAVE_ERROR WeaveConfigManager::WriteConfigValue(const std::string& key, bool value) {
  return WriteKVPair(key, rapidjson::Value(value).Move());
}

WEAVE_ERROR WeaveConfigManager::WriteConfigValue(const std::string& key, uint32_t value) {
  return WriteKVPair(key, rapidjson::Value(value).Move());
}

WEAVE_ERROR WeaveConfigManager::WriteConfigValue(const std::string& key, uint64_t value) {
  return WriteKVPair(key, rapidjson::Value(value).Move());
}

WEAVE_ERROR WeaveConfigManager::WriteConfigValueStr(const std::string& key, const char* value,
                                                    size_t value_size) {
  rapidjson::Value string_value;
  {
    const std::lock_guard<std::mutex> write_lock(config_mutex_);
    string_value.SetString(value, value_size, config_.GetAllocator());
  }
  return WriteKVPair(key, string_value.Move());
}

WEAVE_ERROR WeaveConfigManager::WriteConfigValueBin(const std::string& key, const uint8_t* value,
                                                    size_t value_size) {
  std::string binary_string(reinterpret_cast<const char*>(value), value_size);
  std::string encoded_string(modp_b64_encode(binary_string));
  return WriteConfigValueStr(key, encoded_string.c_str(), encoded_string.size());
}

WEAVE_ERROR WeaveConfigManager::WriteConfigValueArray(const std::string& key,
                                                      std::vector<std::string>& value) {
  rapidjson::Value array_value(rapidjson::kArrayType);
  {
    const std::lock_guard<std::mutex> write_lock(config_mutex_);
    for (auto& config_value : value) {
      rapidjson::Value string_value;
      string_value.SetString(config_value.c_str(), config_value.size(), config_.GetAllocator());
      array_value.PushBack(string_value, config_.GetAllocator());
    }
  }
  return WriteKVPair(key, array_value.Move());
}

WEAVE_ERROR WeaveConfigManager::ClearConfigValue(const std::string& key) {
  const std::lock_guard<std::mutex> write_lock(config_mutex_);
  config_.RemoveMember(key);
  return CommitKVPairs();
}

bool WeaveConfigManager::ConfigValueExists(const std::string& key) const {
  rapidjson::Value value;
  return ReadKVPair(key, value) == WEAVE_NO_ERROR;
}

WEAVE_ERROR WeaveConfigManager::FactoryResetConfig() {
  const std::lock_guard<std::mutex> write_lock(config_mutex_);
  config_.RemoveAllMembers();
  return CommitKVPairs();
}

WEAVE_ERROR WeaveConfigManager::ReadKVPair(const std::string& key, rapidjson::Value& value) const {
  const std::lock_guard<std::mutex> read_lock(config_mutex_);
  if (!config_.HasMember(key)) {
    return WEAVE_DEVICE_ERROR_CONFIG_NOT_FOUND;
  }
  value.CopyFrom(config_[key], config_.GetAllocator());
  return value.IsNull() ? WEAVE_DEVICE_ERROR_CONFIG_NOT_FOUND : WEAVE_NO_ERROR;
}

WEAVE_ERROR WeaveConfigManager::WriteKVPair(const std::string& key, rapidjson::Value& value) {
  const std::lock_guard<std::mutex> write_lock(config_mutex_);
  rapidjson::Value key_value;
  key_value.SetString(key.c_str(), key.size(), config_.GetAllocator());
  rapidjson::Value::MemberIterator it = config_.FindMember(key_value);
  if (it != config_.MemberEnd()) {
    it->value = value;
  } else {
    config_.AddMember(key_value, value, config_.GetAllocator());
  }
  return CommitKVPairs();
}

WEAVE_ERROR WeaveConfigManager::CommitKVPairs() {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  config_.Accept(writer);

  const std::string output(buffer.GetString());
  return files::WriteFile(config_store_path_, output) ? WEAVE_NO_ERROR
                                                      : WEAVE_ERROR_PERSISTED_STORAGE_FAIL;
}

WEAVE_ERROR WeaveConfigManager::SetConfiguration(const std::string& path,
                                                 const std::string& schema_path,
                                                 bool should_replace) {
  json::JSONParser json_parser;
  rapidjson::Document default_config;
  rapidjson::Document schema_config;

  if (!files::IsFile(path)) {
    FX_LOGS(ERROR) << "Default configuration file not found at " << path;
    return WEAVE_ERROR_PERSISTED_STORAGE_FAIL;
  }

  if (!files::IsFile(schema_path)) {
    FX_LOGS(ERROR) << "Schema configuration file not found at " << schema_path;
    return WEAVE_ERROR_PERSISTED_STORAGE_FAIL;
  }

  default_config = json_parser.ParseFromFile(path);
  if (json_parser.HasError()) {
    FX_LOGS(ERROR) << "Failed to parse default configuration file: " << json_parser.error_str();
    return WEAVE_DEVICE_PLATFORM_ERROR_CONFIG_INVALID;
  }

  schema_config = json_parser.ParseFromFile(schema_path);
  if (json_parser.HasError()) {
    FX_LOGS(ERROR) << "Failed to parse schema configuration file: " << json_parser.error_str();
    return WEAVE_DEVICE_PLATFORM_ERROR_CONFIG_INVALID;
  }

  rapidjson::SchemaDocument schema(schema_config);
  rapidjson::SchemaValidator validator(schema);
  if (!default_config.Accept(validator)) {
    return WEAVE_DEVICE_PLATFORM_ERROR_CONFIG_INVALID;
  }

  for (rapidjson::Value::MemberIterator it = default_config.MemberBegin();
       it != default_config.MemberEnd(); ++it) {
    rapidjson::Value::MemberIterator config_it = config_.FindMember(it->name);
    if (config_it == config_.MemberEnd()) {
      config_.AddMember(it->name, it->value, config_.GetAllocator());
    } else if (should_replace) {
      config_.RemoveMember(config_it->name);
      config_.AddMember(it->name, it->value, config_.GetAllocator());
    }
  }
  return CommitKVPairs();
}

}  // namespace nl::Weave::DeviceLayer::Internal
