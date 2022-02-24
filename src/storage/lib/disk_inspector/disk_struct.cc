// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "disk_inspector/disk_struct.h"

#include <lib/syslog/cpp/macros.h>

#include <iostream>
#include <utility>

#include <re2/re2.h>

#include "disk_primitive.h"

namespace disk_inspector {

std::unique_ptr<DiskStruct> DiskStruct::Create(std::string name, uint64_t size) {
  return std::unique_ptr<DiskStruct>(new DiskStruct(std::move(name), size));
}

// TODO(tianruichen): Optimize memory usage of a DiskStruct by storing only a single copy
// of each DiskObj in some sort of global mapping and have DiskStruct only contain references.
void DiskStruct::AddField(std::string key, FieldType type, uint64_t field_offset, uint64_t count,
                          std::unique_ptr<DiskStruct> disk_struct) {
  ZX_DEBUG_ASSERT(fields_.find(key) == fields_.end());
  FieldInfo info;
  info.count = static_cast<int64_t>(count);
  info.offset = field_offset;
  switch (type) {
    case FieldType::kUint8: {
      auto element = std::make_unique<Primitive<uint8_t>>("uint8_t");
      info.element_size = element->GetSize();
      info.element = std::move(element);
      break;
    }
    case FieldType::kUint16: {
      auto element = std::make_unique<Primitive<uint16_t>>("uint16_t");
      info.element_size = element->GetSize();
      info.element = std::move(element);
      break;
    }
    case FieldType::kUint32: {
      auto element = std::make_unique<Primitive<uint32_t>>("uint32_t");
      info.element_size = element->GetSize();
      info.element = std::move(element);
      break;
    }
    case FieldType::kUint64: {
      auto element = std::make_unique<Primitive<uint64_t>>("uint64_t");
      info.element_size = element->GetSize();
      info.element = std::move(element);
      break;
    }
    case FieldType::kDiskStruct: {
      ZX_DEBUG_ASSERT_MSG(disk_struct != nullptr,
                          "disk_struct must be set for kDiskStruct field type.");
      info.element_size = disk_struct->GetSize();
      info.element = std::move(disk_struct);
      break;
    }
    default: {
      FX_LOGS(ERROR) << "Field " << key << " uses an unsupported type to be parsed by DiskStruct";
      info.element_size = 0;
      info.element = nullptr;
      info.count = -1;
      return;
    }
  }
  fields_[key] = std::move(info);
  field_list_.emplace_back(key);
}

zx_status_t DiskStruct::WriteField(void* position, std::vector<std::string> keys,
                                   std::vector<uint64_t> indices, const std::string& value) {
  ZX_DEBUG_ASSERT(!keys.empty());
  ZX_DEBUG_ASSERT(!indices.empty());
  std::string key = keys[0];
  keys.erase(keys.begin());
  uint64_t index = indices[0];
  indices.erase(indices.begin());
  auto field = fields_.find(key);
  if (field == fields_.end()) {
    FX_LOGS(ERROR) << "Field name " << key << " is not in struct.";
    return ZX_ERR_INVALID_ARGS;
  };
  FieldInfo& info = field->second;
  if (info.count < 0) {
    FX_LOGS(ERROR) << "Cannot write to unparsable field " << key;
    return ZX_ERR_INVALID_ARGS;
  }
  if (info.count == 0 && index != 0) {
    FX_LOGS(ERROR) << "Index (" << index << ") for field " << key << " should be 0.";
    return ZX_ERR_INVALID_ARGS;
  }
  if (info.count > 0 && index >= static_cast<uint64_t>(info.count)) {
    FX_LOGS(ERROR) << "Field " << key << " index " << index << " greater than number of elements "
                   << info.count;
    return ZX_ERR_INVALID_ARGS;
  }
  void* element_position =
      reinterpret_cast<char*>(position) + info.offset + info.element_size * index;
  DiskObj* element = info.element.get();
  element->WriteField(element_position, keys, indices, value);
  return ZX_OK;
}

std::string InsertTabAfterNewLine(const std::string& input) {
  std::string output = input;
  re2::RE2::GlobalReplace(&output, re2::RE2("\n"), "\n\t");
  return output;
}

std::string DiskStruct::ToString(void* position, const PrintOptions& options) {
  std::ostringstream stream;
  stream << "Name: " << name_ << "\n";
  for (const auto& field_name : field_list_) {
    const FieldInfo& info = fields_.find(field_name)->second;
    if (info.count < 0) {
      stream << "\t" << field_name << ": Not supported. Cannot parse.\n";
      continue;
    }
    if (info.count == 0) {
      void* element_position = reinterpret_cast<char*>(position) + info.offset;
      stream << "\t" << field_name << ": "
             << InsertTabAfterNewLine(info.element->ToString(element_position, options)) << "\n";
      continue;
    }
    if (info.count > 0) {
      stream << "\t" << field_name << ":";
      if (options.hide_array) {
        stream << " " << info.element->GetTypeName() << "[" << info.count << "] = { ... }\n";
      } else {
        stream << "\n";
        for (uint32_t i = 0; i < info.count; ++i) {
          void* element_position =
              reinterpret_cast<char*>(position) + info.offset + i * info.element_size;
          stream << "\t\t" << info.element->GetTypeName() + " #" << i << ": "
                 << InsertTabAfterNewLine(info.element->ToString(element_position, options))
                 << "\n";
        }
      }
    }
  }
  return stream.str();
}

}  // namespace disk_inspector
