// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_BIN_SESSIONMGR_ANNOTATIONS_H_
#define SRC_MODULAR_BIN_SESSIONMGR_ANNOTATIONS_H_

#include <fuchsia/element/cpp/fidl.h>
#include <fuchsia/modular/cpp/fidl.h>

#include <vector>

namespace modular::annotations {

// Separator between a |fuchsia::element::AnnotationKey| namespace and value when converting keys
// to and from a |fuchsia::modular::Annotation| that stores the key as a single string.
constexpr char kNamespaceValueSeparator = '|';

using Annotation = fuchsia::modular::Annotation;

// Merges the annotations from `b` onto `a`.
//
// * If `a` and `b` contain an annotation with the same key, the result will contain the one from
//   `b`, effectively overwriting it, then:
// * Annotations with a null value are omitted from the result.
// * Order is not guaranteed.
std::vector<Annotation> Merge(std::vector<Annotation> a, std::vector<Annotation> b);

// Helper function for translating annotation values to types ingestable by Inpect framework.
// TODO(fxbug.dev/37645): Template this to return the proper properties
std::string ToInspect(const fuchsia::modular::AnnotationValue& value);

// Converts a |fuchsia::modular::Annotation| key to a |fuchsia::element::AnnotationKey|.
//
// If the key contains a separator from being previously converted from an element
// |AnnotationKey|, the key is parsed to extract a namespace and value. Otherwise, the
// resulting |AnnotationKey| uses the "global" namespace and the key for the value, as-is.
fuchsia::element::AnnotationKey ToElementAnnotationKey(const std::string& key);

// Converts a |fuchsia::modular::Annotation| to a equivalent |fuchsia::element::Annotation|.
fuchsia::element::Annotation ToElementAnnotation(const fuchsia::modular::Annotation& annotation);

// Converts a vector of |fuchsia::modular::Annotation|s to a vector of
// |fuchsia::element::Annotation|s.
std::vector<fuchsia::element::Annotation> ToElementAnnotations(
    const std::vector<fuchsia::modular::Annotation>& annotations);

}  // namespace modular::annotations

namespace element::annotations {

// The global key namespace, used for keys shared across all clients.
constexpr char kGlobalNamespace[] = "global";

// Converts a |fuchsia::element::AnnotationKey| to a |fuchsia::modular::Annotation| key.
//
// If the key namespace is "global", the value is returned as-is. Otherwise, the key namespace and
// value are escaped and joined with a separator.
std::string ToModularAnnotationKey(const fuchsia::element::AnnotationKey& key);

// Converts a |fuchsia::element::Annotation| to an equivalent |fuchsia::modular::Annotation|.
fuchsia::modular::Annotation ToModularAnnotation(const fuchsia::element::Annotation& annotation);

// Converts a vector of |fuchsia::modular::Annotation|s to a vector of equivalent
// |fuchsia::element::Annotation|s.
std::vector<fuchsia::modular::Annotation> ToModularAnnotations(
    const std::vector<fuchsia::element::Annotation>& annotations);

// Returns true if the given |AnnotationKey| is valid.
//
// Valid keys must have a non-empty namespace.
bool IsValidKey(const fuchsia::element::AnnotationKey& key);

}  // namespace element::annotations

namespace std {

// A specialization of std::equal_to that allows |AnnotationKey| to be stored in |unordered_set|s
// and |unordered_map|s.
template <>
struct equal_to<fuchsia::element::AnnotationKey> {
  bool operator()(const fuchsia::element::AnnotationKey& lhs,
                  const fuchsia::element::AnnotationKey& rhs) const {
    return fidl::Equals(lhs, rhs);
  }
};

// A specialization of std::hash that allows |AnnotationKey| to be stored in |unordered_set|s
// and |unordered_map|s.
template <>
struct hash<fuchsia::element::AnnotationKey> {
  size_t operator()(const fuchsia::element::AnnotationKey& key) const {
    return std::hash<std::string>()(key.namespace_) ^ std::hash<std::string>()(key.value);
  }
};

}  // namespace std

#endif  // SRC_MODULAR_BIN_SESSIONMGR_ANNOTATIONS_H_
