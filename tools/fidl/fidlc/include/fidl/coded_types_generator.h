// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_FIDL_FIDLC_INCLUDE_FIDL_CODED_TYPES_GENERATOR_H_
#define TOOLS_FIDL_FIDLC_INCLUDE_FIDL_CODED_TYPES_GENERATOR_H_

#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "coded_ast.h"
#include "flat/compiler.h"
#include "flat_ast.h"

namespace fidl {

class CodedTypesGenerator {
 public:
  explicit CodedTypesGenerator(const flat::Libraries* all_libraries)
      : target_library_decl_order_(all_libraries->target_library()->declaration_order),
        all_libraries_decl_order_(all_libraries->DeclarationOrder()) {}

  void CompileCodedTypes();

  const std::vector<const flat::Decl*>& target_library_decl_order() {
    return target_library_decl_order_;
  }
  const std::vector<const flat::Decl*>& all_libraries_decl_order() {
    return all_libraries_decl_order_;
  }
  const std::vector<std::unique_ptr<coded::Type>>& coded_types() const { return coded_types_; }

  const coded::Type* CodedTypeFor(flat::Name::Key name) const {
    auto it = named_coded_types_.find(name);
    return it != named_coded_types_.end() ? it->second.get() : nullptr;
  }

  std::vector<const coded::Type*> AllCodedTypes() const;

 private:
  // Returns a pointer owned by coded_types_.
  const coded::Type* CompileType(const flat::Type* type, coded::CodingContext context);
  void CompileFields(const flat::Decl* decl);
  void CompileDecl(const flat::Decl* decl);
  void CompileXRef(const coded::Type* type);

  // Representation of the fields of a struct member after it has been flattened.
  struct FlattenedStructMember {
    explicit FlattenedStructMember(const flat::StructMember& member);

    const flat::Type* type;
    const SourceSpan name;
    const uint32_t inline_size_v1;
    const uint32_t inline_size_v2;
    uint32_t offset_v1;
    uint32_t offset_v2;
    uint32_t padding;

   private:
    FlattenedStructMember(const flat::Type* type, SourceSpan name, fidl::TypeShape typeshape_v1,
                          fidl::TypeShape typeshape_v2, fidl::FieldShape fieldshape_v1,
                          fidl::FieldShape fieldshape_v2);

    FlattenedStructMember(const flat::Type* type, SourceSpan name, uint32_t inline_size_v1,
                          uint32_t inline_size_v2, uint32_t offset_v1, uint32_t offset_v2,
                          uint32_t padding);
  };

  // Flatten a list of flat-AST struct members by recursively descending and expanding.
  // e.g.:
  // struct A { int8 x; };
  // struct B { A y; int8 z; };
  // becomes the equivalent of
  // struct B { int8 x; int8 z; };
  std::vector<FlattenedStructMember> FlattenedStructMembers(const flat::Struct& input);

  template <typename FlatType, typename CodedType>
  using TypeMap = std::map<const FlatType*, const CodedType*, utils::PtrCompare<const FlatType>>;

  template <typename T, typename P = const T*>
  struct MaybeCodedTypeCompare {
    bool operator()(const std::pair<bool, P>& lhs, const std::pair<bool, P>& rhs) const {
      utils::PtrCompare<T> comparator;
      bool a_less_b = comparator(lhs.second, rhs.second);
      bool b_less_a = comparator(rhs.second, lhs.second);
      if (!a_less_b && !b_less_a) {
        // types are equivalent
        return lhs.first < rhs.first;
      } else {
        return a_less_b;
      }
    }
  };

  const std::vector<const flat::Decl*>& target_library_decl_order_;
  std::vector<const flat::Decl*> all_libraries_decl_order_;

  // All flat::Types here are owned by all_libraries passed in the constructor,
  // and all coded::Types are owned by by named_coded_types_ or coded_types_.
  TypeMap<flat::PrimitiveType, coded::PrimitiveType> primitive_type_map_;
  TypeMap<flat::HandleType, coded::HandleType> handle_type_map_;
  TypeMap<flat::TransportSideType, coded::Type> channel_end_map_;
  TypeMap<flat::IdentifierType, coded::ProtocolHandleType> protocol_type_map_;
  TypeMap<flat::ArrayType, coded::ArrayType> array_type_map_;
  TypeMap<flat::VectorType, coded::VectorType> vector_type_map_;
  TypeMap<flat::StringType, coded::StringType> string_type_map_;
  TypeMap<flat::IdentifierType, coded::StructPointerType> struct_type_map_;

  std::map<flat::Name::Key, std::unique_ptr<coded::Type>> named_coded_types_;
  std::vector<std::unique_ptr<coded::Type>> coded_types_;
};

// Compute if a type is "memcpy-compatible", in that it can safely be memcpy'd during encode.
// This means that the type doesn't contain pointers, padding, envelopes or handles.
coded::MemcpyCompatibility ComputeMemcpyCompatibility(const flat::Type* type);

}  // namespace fidl

#endif  // TOOLS_FIDL_FIDLC_INCLUDE_FIDL_CODED_TYPES_GENERATOR_H_
