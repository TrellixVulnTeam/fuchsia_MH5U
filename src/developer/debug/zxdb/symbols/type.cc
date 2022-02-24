// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/type.h"

#include <lib/syslog/cpp/macros.h>

#include "src/developer/debug/zxdb/symbols/symbol_utils.h"

namespace zxdb {

Type::Type(DwarfTag kind) : Symbol(kind) { FX_DCHECK(DwarfTagIsType(kind)); }

Type::~Type() = default;

const Type* Type::AsType() const { return this; }

const Type* Type::StripCV() const { return this; }

const Type* Type::StripCVT() const { return this; }

}  // namespace zxdb
