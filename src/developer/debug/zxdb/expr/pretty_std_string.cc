// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/expr/pretty_std_string.h"

#include "src/developer/debug/zxdb/expr/format.h"
#include "src/developer/debug/zxdb/expr/format_node.h"
#include "src/developer/debug/zxdb/expr/format_options.h"
#include "src/developer/debug/zxdb/symbols/base_type.h"
#include "src/developer/debug/zxdb/symbols/modified_type.h"
#include "src/developer/debug/zxdb/symbols/symbol_data_provider.h"

namespace zxdb {

namespace {

// A hardcoded pretty-printer for our std::string implementation.
//
// Long-term, we'll want a better pretty-printing system that's more extensible and versionable
// with our C++ library. This is a first step to designing such a system.
//
// In libc++ std::string is an "extern template" which means that the char specialization of
// basic_string is in the shared library. Without symbols for libc++, there is no definition for
// std::string.
//
// As of this writing our libc++ doesn't have symbols, and it's also nice to allow people to
// print strings in their own program without all of the lib++ symbols (other containers don't
// require this so it can be surprising).
//
// As a result, this pretty-printer is designed to work with no symbol information, and getting
// a value with no size (the expression evaluator won't know what size to make in many cases).
// This complicates it considerably, but std::string is likely the only class that will need such
// handling.
//
// THE DEFINITION
// --------------
//
// Our libc++'s std::string implementation has two modes, a "short" mode where the string is stored
// inline in the string object, and a "long" mode where it stores a pointer to a heap-allocated
// buffer. These modes are differentiated with a bit on the last byte of the storage.
//
//   class basic_string {
//     // For little-endian:
//     static const size_type __short_mask = 0x80;
//     static const size_type __long_mask  = ~(size_type(~0) >> 1);  // High bit set.
//
//     bool is_long() const {return __r_.__s.__size_ & __short_mask; }
//
//     struct __rep {
//       // Long is used when "__s.__size_ & __short_mask" is true.
//       union {
//         struct __long {
//           value_type* __data_;
//           size_t __size_;
//           size_t __cap_;  // & with __long_mask to get.
//         } __l;
//
//         struct __short {
//           char value_type[23]
//           // padding of sizeof(char) - 1
//           struct {
//             unsigned char __size_;
//           };
//         } __s;
//
//         __raw __r;  // Can ignore, used only for rapidly copying the representation.
//       };
//     };
//
//     // actually "__compressed_pair<__rep, allocator> __r_" but effectively:
//     compressed_pair __r_;
//   };

constexpr uint32_t kStdStringSize = 24;

// Offset from beginning of the object to "__short.__size_" (last byte).
constexpr size_t kShortSizeOffset = 23;

// Bit that indicates the "short" representation.
constexpr uint64_t kShortMask = 0x80;

// Offsets within the data for the "long" representation.
constexpr uint64_t kLongPtrOffset = 0;
constexpr uint64_t kLongSizeOffset = 8;
constexpr uint64_t kLongCapacityOffset = 16;

fxl::RefPtr<BaseType> GetStdStringCharType() {
  return fxl::MakeRefCounted<BaseType>(BaseType::kBaseTypeSignedChar, 1, "char");
}
fxl::RefPtr<BaseType> GetSizeTType() {
  return fxl::MakeRefCounted<BaseType>(BaseType::kBaseTypeUnsigned, 8, "size_t");
}

// Returns true if this std::string uses the inline representation. It's assumed the data has
// al;ready been validated as being the correct length.
ErrOr<bool> IsInlineString(const TaggedData& mem) {
  FX_DCHECK(mem.size() == kStdStringSize);
  if (!mem.RangeIsValid(kShortSizeOffset, 1))
    return Err::OptimizedOut();
  return !(mem.bytes()[kShortSizeOffset] & kShortMask);
}

// Fills in the data pointer for the given std::string.
Err GetStringPtr(const ExprValue& value, uint64_t* ptr) {
  if (value.data().size() != kStdStringSize)
    return Err("Invalid std::string data.");

  ErrOr<bool> inline_or = IsInlineString(value.data());
  if (inline_or.has_error())
    return inline_or.err();

  if (inline_or.value()) {
    // The address is just the beginning of the string.
    if (value.source().type() != ExprValueSource::Type::kMemory || value.source().address() == 0)
      return Err("Can't get string pointer to a temporary.");
    *ptr = value.source().address();
  } else {
    if (!value.data().RangeIsValid(kLongPtrOffset, sizeof(uint64_t)))
      return Err::OptimizedOut();
    memcpy(ptr, &value.data().bytes()[kLongPtrOffset], sizeof(uint64_t));
  }
  return Err();
}

// Guarantees that any inline size is inside the buffer.
Err GetStringSize(const TaggedData& mem, uint64_t* size) {
  if (mem.size() != kStdStringSize)
    return Err("Invalid std::string data.");

  ErrOr<bool> inline_or = IsInlineString(mem);
  if (inline_or.has_error())
    return inline_or.err();

  if (inline_or.value()) {
    if (!mem.RangeIsValid(kShortSizeOffset, 1))
      return Err::OptimizedOut();
    *size = mem.bytes()[kShortSizeOffset];

    // Sanity check. The string could be corrupted and we don't want to report an inline size
    // greater than the inline buffer (including null).
    if (*size >= kStdStringSize - 1)
      return Err("std::string has invalid size for inline data (" + std::to_string(*size) + ")");
  } else {
    if (!mem.RangeIsValid(kLongSizeOffset, sizeof(uint64_t)))
      return Err::OptimizedOut();
    memcpy(size, &mem.bytes()[kLongSizeOffset], sizeof(uint64_t));
  }
  return Err();
}

Err GetStringCapacity(const TaggedData& mem, uint64_t* capacity) {
  if (mem.size() != kStdStringSize)
    return Err("Invalid std::string data.");

  ErrOr<bool> inline_or = IsInlineString(mem);
  if (inline_or.has_error())
    return inline_or.err();

  if (inline_or.value()) {
    *capacity = kShortSizeOffset - 1;  // Inline size is stuff before the short size minus null.
  } else {
    if (!mem.RangeIsValid(kLongCapacityOffset, sizeof(uint64_t)))
      return Err::OptimizedOut();
    memcpy(capacity, &mem.bytes()[kLongCapacityOffset], sizeof(uint64_t));

    // Mask off the high bit which is the "large" flag.
    *capacity &= 0x7fffffffffffffff;
  }
  return Err();
}

void FormatStdStringMemory(const TaggedData& mem, FormatNode* node, const FormatOptions& options,
                           const fxl::RefPtr<EvalContext>& context, fit::deferred_callback cb) {
  node->set_type("std::string");
  if (mem.size() != kStdStringSize)
    return node->SetDescribedError(Err("Invalid."));

  auto char_type = GetStdStringCharType();

  uint64_t string_size = 0;
  if (Err err = GetStringSize(mem, &string_size); err.has_error())
    return node->SetDescribedError(err);

  ErrOr<bool> inline_or = IsInlineString(mem);
  if (inline_or.has_error())
    return node->SetDescribedError(inline_or.err());

  if (inline_or.value()) {
    if (!mem.RangeIsValid(0, string_size))
      return node->SetDescribedError(Err::OptimizedOut());
    FormatCharArrayNode(node, char_type, mem.bytes().data(), string_size, true, false);
  } else {
    // Long representation (with pointer).
    if (!mem.RangeIsValid(kLongPtrOffset, sizeof(uint64_t)))
      return node->SetDescribedError(Err::OptimizedOut());

    uint64_t data_ptr;
    memcpy(&data_ptr, &mem.bytes()[kLongPtrOffset], sizeof(uint64_t));
    FormatCharPointerNode(node, data_ptr, char_type.get(), string_size, options, context,
                          std::move(cb));
  }
}

// Normally when we have a std::string we won't have the data because the definition is
// missing. But the "source" will usually be set and we can go fetch the right amount of data.
// This function calls the callback with a populated ExprValue if it can be made to have the correct
// size.
void EnsureStdStringMemory(const fxl::RefPtr<EvalContext>& context, const ExprValue& value,
                           EvalCallback cb) {
  if (value.data().size() != 0) {
    if (value.data().size() == kStdStringSize)
      return cb(value);
    return cb(Err("Invalid std::string type size."));
  }

  // Don't have the data, see if we can fetch it.
  if (value.source().type() != ExprValueSource::Type::kMemory || value.source().address() == 0)
    return cb(Err("Can't handle a temporary std::string."));

  context->GetDataProvider()->GetMemoryAsync(
      value.source().address(), kStdStringSize,
      [value, cb = std::move(cb)](const Err& err, std::vector<uint8_t> data) mutable {
        if (err.has_error())
          cb(err);
        else if (data.size() != kStdStringSize)
          cb(Err("Invalid memory."));
        else
          cb(ExprValue(value.type_ref(), std::move(data), value.source()));
      });
}

// Getters all need to do the same thing: ensure memory, error check, and then run on the result.
// This returns a callback that does that stuff, with the given "getter" implementation taking
// a complete string of a known correct size.
PrettyStdString::EvalFunction MakeGetter(fit::function<void(ExprValue, EvalCallback)> getter) {
  return [getter = std::move(getter)](const fxl::RefPtr<EvalContext>& context,
                                      const ExprValue& object_value, EvalCallback cb) mutable {
    EnsureStdStringMemory(
        context, object_value,
        [context, cb = std::move(cb), getter = std::move(getter)](ErrOrValue value) mutable {
          if (value.has_error())
            return cb(value);
          getter(value.value(), std::move(cb));
        });
  };
}

}  // namespace

void PrettyStdString::Format(FormatNode* node, const FormatOptions& options,
                             const fxl::RefPtr<EvalContext>& context, fit::deferred_callback cb) {
  EnsureStdStringMemory(context, node->value(),
                        [weak_node = node->GetWeakPtr(), options, context,
                         cb = std::move(cb)](ErrOrValue value) mutable {
                          if (!weak_node)
                            return;
                          if (value.has_error()) {
                            weak_node->set_err(value.err());
                            weak_node->set_state(FormatNode::kDescribed);
                          } else {
                            FormatStdStringMemory(value.value().data(), weak_node.get(), options,
                                                  context, std::move(cb));
                          }
                        });
}

PrettyStdString::EvalFunction PrettyStdString::GetGetter(const std::string& getter_name) const {
  if (getter_name == "data" || getter_name == "c_str") {
    return MakeGetter([](ExprValue value, EvalCallback cb) {
      uint64_t ptr = 0;
      if (Err err = GetStringPtr(value, &ptr); err.has_error())
        return cb(err);

      auto char_ptr =
          fxl::MakeRefCounted<ModifiedType>(DwarfTag::kPointerType, GetStdStringCharType());
      cb(ExprValue(ptr, char_ptr));
    });
  }
  if (getter_name == "size" || getter_name == "length") {
    return MakeGetter([](ExprValue value, EvalCallback cb) {
      uint64_t string_size = 0;
      if (Err err = GetStringSize(value.data(), &string_size); err.has_error())
        return cb(err);
      cb(ExprValue(string_size, GetSizeTType()));
    });
  }
  if (getter_name == "capacity") {
    return MakeGetter([](ExprValue value, EvalCallback cb) {
      uint64_t cap = 0;
      if (Err err = GetStringCapacity(value.data(), &cap); err.has_error())
        return cb(err);
      cb(ExprValue(cap, GetSizeTType()));
    });
  }
  if (getter_name == "empty") {
    return MakeGetter([](ExprValue value, EvalCallback cb) {
      uint64_t string_size = 0;
      if (Err err = GetStringSize(value.data(), &string_size); err.has_error())
        return cb(err);
      cb(ExprValue(string_size == 0));
    });
  }

  return EvalFunction();
}

PrettyStdString::EvalArrayFunction PrettyStdString::GetArrayAccess() const {
  return [](const fxl::RefPtr<EvalContext>& context, const ExprValue& object_value, int64_t index,
            EvalCallback cb) {
    EnsureStdStringMemory(
        context, object_value, [context, cb = std::move(cb), index](ErrOrValue value) mutable {
          if (value.has_error())
            return cb(value.err());

          const TaggedData& string_data = value.value().data();
          if (IsInlineString(string_data)) {
            // Use the inline data. Need to range check since we're indexing into our local
            // address space.
            if (index >= static_cast<int64_t>(kShortSizeOffset) || index < 0)
              return cb(Err("String index out of range."));

            if (!string_data.RangeIsValid(index, 1))
              return cb(Err::OptimizedOut());

            // Inline array starts from the beginning of the string.
            return cb(ExprValue(GetStdStringCharType(), {string_data.bytes()[index]},
                                value.value().source().GetOffsetInto(index)));
          } else {
            uint64_t ptr = 0;
            if (Err err = GetStringPtr(value.value(), &ptr); err.has_error())
              return cb(err);

            context->GetDataProvider()->GetMemoryAsync(
                ptr, 1,
                [context, ptr, cb = std::move(cb)](const Err& err,
                                                   std::vector<uint8_t> data) mutable {
                  if (err.has_error())
                    return cb(err);
                  if (data.size() == 0)
                    return cb(Err("Invalid address 0x%" PRIx64, ptr));

                  cb(ExprValue(GetStdStringCharType(), {data[0]}, ExprValueSource(ptr)));
                });
          }
        });
  };
}

}  // namespace zxdb
