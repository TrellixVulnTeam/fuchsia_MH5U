// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_SHELL_INTERPRETER_SRC_SCOPE_H_
#define SRC_DEVELOPER_SHELL_INTERPRETER_SRC_SCOPE_H_

#include <lib/syslog/cpp/macros.h>

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/developer/shell/interpreter/src/code.h"
#include "src/developer/shell/interpreter/src/nodes.h"
#include "src/developer/shell/interpreter/src/value.h"

namespace shell {
namespace interpreter {

class Thread;

// Base class for all scope variables.
class Variable {
 public:
  Variable(NodeId id, const std::string& name, size_t index, std::unique_ptr<Type> type,
           bool is_mutable)
      : id_(id), name_(name), index_(index), type_(std::move(type)), is_mutable_(is_mutable) {}

  NodeId id() const { return id_; }
  const std::string& name() const { return name_; }
  size_t index() const { return index_; }
  const Type* type() const { return type_.get(); }
  bool is_mutable() const { return is_mutable_; }

  // Clear the variable for this execution scope. For reference counted objects, that also releases
  // the object.
  void Clear(ExecutionScope* scope) { type_->ClearVariable(scope, index_); }

 private:
  // Id of the node which defines the variable.
  const NodeId id_;
  // Name of the variable.
  const std::string name_;
  // Index in the execution scope's data_ vector.
  const size_t index_;
  // The type of the variable.
  std::unique_ptr<Type> type_;
  // True if we can modify the variable initial value.
  const bool is_mutable_;
};

// Defines a scope. This can be a global scope (at the isolate level) or a scope associated to
// a thread, a function, a block, ...
// This scope is used during compilation and, eventually, during execution for generic code.
class Scope {
 public:
  Scope() = default;

  size_t size() const { return current_index_; }

  // Returns the variable with the given name.
  const Variable* GetVariable(const std::string& name) const {
    auto result = variables_.find(name);
    if (result == variables_.end()) {
      return nullptr;
    }
    return result->second.get();
  }

  const Variable* SearchVariable(const NodeId& node_id) const {
    for (const auto& variable : variables_) {
      if (variable.second->id() == node_id) {
        return variable.second.get();
      }
    }
    return nullptr;
  }

  // Creates a variable.
  Variable* CreateVariable(NodeId id, const std::string& name, std::unique_ptr<Type> type,
                           bool is_mutable) {
    size_t size = type->Size();
    FX_DCHECK(size > 0);
    AlignIndex(size);
    auto variable =
        std::make_unique<Variable>(id, name, current_index_, std::move(type), is_mutable);
    auto returned_value = variable.get();
    variables_.emplace(std::make_pair(name, std::move(variable)));
    current_index_ += size;
    return returned_value;
  }

  // Shutdown the scope. That releases all the allocated objects.
  void Shutdown(ExecutionScope* execution_scope);

 private:
  void AlignIndex(size_t size) { current_index_ = (current_index_ + (size - 1)) & ~(size - 1); }

  // All the variables for this scope.
  std::map<std::string, std::unique_ptr<Variable>> variables_;

  // Current index in the data_ field of an execution scope.
  size_t current_index_ = 0;
};

// Defines the storage for one scope. It can be the global storage for a global scope or a local
// storage for a function.
class ExecutionScope {
 public:
  ExecutionScope() = default;

  // Returns the size of the allocated global data.
  size_t size() const { return data_.size(); }

  // Resizes the storage to be able to store the newly created variables.
  void Resize(size_t new_size) {
    // We can only add variables.
    FX_DCHECK(new_size >= data_.size());
    data_.resize(new_size);
  }

  // Retrieves a pointer to the storage.
  uint8_t* Data(size_t index, size_t size) {
    FX_DCHECK(index + size <= data_.size());
    return data_.data() + index;
  }
  template <typename T>
  T* Data(size_t index) {
    return reinterpret_cast<T*>(Data(index, sizeof(T)));
  }

  // Retrieves a pointer to the storage.
  const uint8_t* Data(size_t index, size_t size) const {
    FX_DCHECK(index + size <= data_.size());
    return data_.data() + index;
  }
  template <typename T>
  const T* Data(size_t index) const {
    return reinterpret_cast<const T*>(Data(index, sizeof(T)));
  }

  // Loads the current content of |variable| for this storage into |value|.
  void Load(const Variable* variable, Value* value) const {
    variable->type()->LoadVariable(this, variable->index(), value);
  }

  // Executes |code| for |context| |thread| for this storage.
  void Execute(ExecutionContext* context, Thread* thread, std::unique_ptr<code::Code> code);

 private:
  // The stored data.
  std::vector<uint8_t> data_;
};

}  // namespace interpreter
}  // namespace shell

#endif  // SRC_DEVELOPER_SHELL_INTERPRETER_SRC_SCOPE_H_
