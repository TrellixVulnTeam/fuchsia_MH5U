// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/namespace.h"

#include "src/developer/debug/zxdb/symbols/symbol_utils.h"

namespace zxdb {

Namespace::Namespace() : Symbol(DwarfTag::kNamespace) {}

Namespace::Namespace(std::string n) : Symbol(DwarfTag::kNamespace), assigned_name_(std::move(n)) {}

Namespace::~Namespace() = default;

const Namespace* Namespace::AsNamespace() const { return this; }

Identifier Namespace::ComputeIdentifier() const {
  const std::string& assigned = GetAssignedName();

  Identifier result = GetSymbolScopePrefix(this);
  result.AppendComponent(IdentifierComponent(assigned));
  return result;
}

}  // namespace zxdb
