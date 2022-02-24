// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fitx/result.h>
#include <lib/zx/status.h>
#include <zircon/errors.h>

#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <zxtest/zxtest.h>

namespace {

struct nothing {};

// Basic properties.
static_assert(!std::is_constructible_v<fitx::result<int>>);
static_assert(std::is_constructible_v<fitx::result<int>, fitx::success<>>);
static_assert(!std::is_constructible_v<fitx::result<int>, fitx::failed>);
static_assert(!std::is_constructible_v<fitx::result<int>, nothing>);
static_assert(!std::is_constructible_v<fitx::result<int>, fitx::success<nothing>>);
static_assert(std::is_constructible_v<fitx::result<int>, fitx::error<int>>);
static_assert(!std::is_constructible_v<fitx::result<int>, fitx::error<nothing>>);

static_assert(!std::is_constructible_v<fitx::result<int, int>>);
static_assert(!std::is_constructible_v<fitx::result<int, int>, fitx::success<>>);
static_assert(!std::is_constructible_v<fitx::result<int, int>, fitx::failed>);
static_assert(!std::is_constructible_v<fitx::result<int, int>, int>);
static_assert(std::is_constructible_v<fitx::result<int, int>, fitx::success<int>>);
static_assert(!std::is_constructible_v<fitx::result<int, int>, nothing>);
static_assert(!std::is_constructible_v<fitx::result<int, int>, fitx::success<nothing>>);
static_assert(std::is_constructible_v<fitx::result<int, int>, fitx::error<int>>);
static_assert(!std::is_constructible_v<fitx::result<int, int>, fitx::error<nothing>>);

static_assert(!std::is_constructible_v<fitx::result<fitx::failed>>);
static_assert(std::is_constructible_v<fitx::result<fitx::failed>, fitx::success<>>);
static_assert(std::is_constructible_v<fitx::result<fitx::failed>, fitx::failed>);
static_assert(!std::is_constructible_v<fitx::result<fitx::failed>, nothing>);
static_assert(!std::is_constructible_v<fitx::result<fitx::failed>, fitx::success<nothing>>);
static_assert(!std::is_constructible_v<fitx::result<fitx::failed>, fitx::error<int>>);
static_assert(!std::is_constructible_v<fitx::result<fitx::failed>, fitx::error<nothing>>);
static_assert(std::is_constructible_v<fitx::result<fitx::failed>, fitx::error<fitx::failed>>);

static_assert(!std::is_constructible_v<fitx::result<fitx::failed, int>>);
static_assert(!std::is_constructible_v<fitx::result<fitx::failed, int>, fitx::success<>>);
static_assert(std::is_constructible_v<fitx::result<fitx::failed, int>, fitx::failed>);
static_assert(!std::is_constructible_v<fitx::result<fitx::failed, int>, int>);
static_assert(std::is_constructible_v<fitx::result<fitx::failed, int>, fitx::success<int>>);
static_assert(!std::is_constructible_v<fitx::result<fitx::failed, int>, nothing>);
static_assert(!std::is_constructible_v<fitx::result<fitx::failed, int>, fitx::success<nothing>>);
static_assert(!std::is_constructible_v<fitx::result<fitx::failed, int>, fitx::error<int>>);
static_assert(!std::is_constructible_v<fitx::result<fitx::failed, int>, fitx::error<nothing>>);
static_assert(std::is_constructible_v<fitx::result<fitx::failed, int>, fitx::error<fitx::failed>>);

// Ensure that success/error types and helpers do not return references to their arguments when
// deducing the value/error types.
[[maybe_unused]] constexpr auto return_success(int value) { return fitx::success(value); }
[[maybe_unused]] constexpr auto return_error(int value) { return fitx::error(value); }
[[maybe_unused]] constexpr auto return_ok(int value) { return fitx::ok(value); }
[[maybe_unused]] constexpr auto return_as_error(int value) { return fitx::as_error(value); }
static_assert(std::is_same_v<fitx::success<int>, decltype(return_success(10))>);
static_assert(std::is_same_v<fitx::error<int>, decltype(return_error(10))>);
static_assert(std::is_same_v<fitx::success<int>, decltype(return_ok(10))>);
static_assert(std::is_same_v<fitx::error<int>, decltype(return_as_error(10))>);

#if 0 || TEST_DOES_NOT_COMPILE
static_assert(fitx::result<fitx::success<>>{});
static_assert(fitx::result<int, fitx::failed>{});
#endif

static_assert(fitx::result<fitx::failed>{fitx::ok()}.is_ok() == true);
static_assert(fitx::result<fitx::failed>{fitx::ok()}.is_error() == false);
static_assert(fitx::result<fitx::failed>{fitx::failed()}.is_ok() == false);
static_assert(fitx::result<fitx::failed>{fitx::failed()}.is_error() == true);

static_assert(fitx::result<int>{fitx::ok()}.is_ok() == true);
static_assert(fitx::result<int>{fitx::ok()}.is_error() == false);
static_assert(fitx::result<int>{fitx::error(0)}.is_ok() == false);
static_assert(fitx::result<int>{fitx::error(0)}.is_error() == true);

static_assert(fitx::result<int, int>{fitx::ok(10)}.is_ok() == true);
static_assert(fitx::result<int, int>{fitx::ok(10)}.is_error() == false);
static_assert(fitx::result<int, int>{fitx::ok(10)}.value() == 10);
static_assert(*fitx::result<int, int>{fitx::ok(10)} == 10);
static_assert(fitx::result<int, int>{fitx::ok(10)}.value_or(20) == 10);
static_assert(fitx::result<int, int>{fitx::error(10)}.is_ok() == false);
static_assert(fitx::result<int, int>{fitx::error(10)}.is_error() == true);
static_assert(fitx::result<int, int>{fitx::error(10)}.error_value() == 10);
static_assert(fitx::result<int, int>{fitx::error(10)}.value_or(20) == 20);

// Agumenting errors.
struct augmented_error {
  struct yes {};
  struct no {};

  // Error can be augmented by type yes but not type no.
  constexpr void operator+=(yes) {}
};

template <typename T>
constexpr bool agument_compiles() {
  {
    fitx::result<augmented_error> result = fitx::error<augmented_error>();
    result += fitx::error<T>();
  }
  {
    fitx::result<augmented_error, int> result = fitx::error<augmented_error>();
    result += fitx::error<T>();
  }
  return true;
}

static_assert(agument_compiles<augmented_error::yes>());
#if 0 || TEST_DOES_NOT_COMPILE
static_assert(agument_compiles<augmented_error::no>());
#endif

// Arrow operator and arrow operator forwarding.
struct test_members {
  int a;
  int b;
};

static_assert(fitx::result<fitx::failed, test_members> { zx::ok(test_members{10, 20}) } -> a == 10);
static_assert(fitx::result<fitx::failed, test_members> { zx::ok(test_members{10, 20}) } -> b == 20);
static_assert(fitx::result<fitx::failed, std::optional<test_members>> {
  zx::ok(test_members{10, 20})
} -> a == 10);
static_assert(fitx::result<fitx::failed, std::optional<test_members>> {
  zx::ok(test_members{10, 20})
} -> b == 20);

// Status-only, no value.
static_assert(zx::status<>{zx::ok()}.is_ok() == true);
static_assert(zx::status<>{zx::ok()}.is_error() == false);
static_assert(zx::status<>{zx::ok()}.status_value() == ZX_OK);

static_assert(zx::status<>{zx::error{ZX_ERR_INVALID_ARGS}}.is_ok() == false);
static_assert(zx::status<>{zx::error{ZX_ERR_INVALID_ARGS}}.is_error() == true);
static_assert(zx::status<>{zx::error{ZX_ERR_INVALID_ARGS}}.error_value() == ZX_ERR_INVALID_ARGS);
static_assert(zx::status<>{zx::error{ZX_ERR_INVALID_ARGS}}.status_value() == ZX_ERR_INVALID_ARGS);

static_assert(zx::status<>{zx::make_status(ZX_OK)}.is_ok() == true);
static_assert(zx::status<>{zx::make_status(ZX_OK)}.is_error() == false);
static_assert(zx::status<>{zx::make_status(ZX_OK)}.status_value() == ZX_OK);

static_assert(zx::status<>{zx::make_status(ZX_ERR_INVALID_ARGS)}.is_ok() == false);
static_assert(zx::status<>{zx::make_status(ZX_ERR_INVALID_ARGS)}.is_error() == true);
static_assert(zx::status<>{zx::make_status(ZX_ERR_INVALID_ARGS)}.error_value() ==
              ZX_ERR_INVALID_ARGS);
static_assert(zx::status<>{zx::make_status(ZX_ERR_INVALID_ARGS)}.status_value() ==
              ZX_ERR_INVALID_ARGS);

// Status or value.
static_assert(zx::status<int>{zx::ok(10)}.is_ok() == true);
static_assert(zx::status<int>{zx::ok(10)}.is_error() == false);
static_assert(zx::status<int>{zx::ok(10)}.status_value() == ZX_OK);
static_assert(zx::status<int>{zx::ok(10)}.value() == 10);
static_assert(*zx::status<int>{zx::ok(10)} == 10);

static_assert(zx::status<int>{zx::error{ZX_ERR_INVALID_ARGS}}.is_ok() == false);
static_assert(zx::status<int>{zx::error{ZX_ERR_INVALID_ARGS}}.is_error() == true);
static_assert(zx::status<int>{zx::error{ZX_ERR_INVALID_ARGS}}.error_value() == ZX_ERR_INVALID_ARGS);
static_assert(zx::status<int>{zx::error{ZX_ERR_INVALID_ARGS}}.status_value() ==
              ZX_ERR_INVALID_ARGS);

// Status or value via make_status.
static_assert(zx::make_status(ZX_OK, 10).is_ok() == true);
static_assert(zx::make_status(ZX_OK, 10).is_error() == false);
static_assert(zx::make_status(ZX_OK, 10).status_value() == ZX_OK);
static_assert(zx::make_status(ZX_OK, 10).value() == 10);
static_assert(*zx::make_status(ZX_OK, 10) == 10);

static_assert(zx::make_status(ZX_ERR_INVALID_ARGS, 0).is_ok() == false);
static_assert(zx::make_status(ZX_ERR_INVALID_ARGS, 0).is_error() == true);
static_assert(zx::make_status(ZX_ERR_INVALID_ARGS, 0).error_value() == ZX_ERR_INVALID_ARGS);
static_assert(zx::make_status(ZX_ERR_INVALID_ARGS, 0).status_value() == ZX_ERR_INVALID_ARGS);

struct default_constructible {
  default_constructible() = default;
};

struct non_default_constructible {
  non_default_constructible() = delete;
  non_default_constructible(int) {}
};

struct copyable {
  copyable() = default;
  copyable(const copyable&) = default;
  copyable& operator=(const copyable&) = default;
  copyable(copyable&&) noexcept = default;
  copyable& operator=(copyable&&) noexcept = default;
};

static_assert(std::is_copy_constructible_v<copyable>);
static_assert(std::is_move_constructible_v<copyable>);
static_assert(std::is_nothrow_move_constructible_v<copyable>);

struct move_only {
  move_only() = default;
  move_only(const move_only&) = delete;
  move_only& operator=(const move_only&) = delete;
  move_only(move_only&&) noexcept = default;
  move_only& operator=(move_only&&) noexcept = default;
};

static_assert(!std::is_copy_constructible_v<move_only>);
static_assert(std::is_move_constructible_v<move_only>);
static_assert(std::is_nothrow_move_constructible_v<move_only>);

struct trivial {
  trivial() = default;
  ~trivial() = default;
};

static_assert(std::is_trivially_constructible_v<trivial, trivial>);
static_assert(std::is_trivially_copy_constructible_v<trivial>);
static_assert(std::is_trivially_move_constructible_v<trivial>);
static_assert(std::is_trivially_destructible_v<trivial>);

struct non_trivial {
  non_trivial() {}
  ~non_trivial() {}
};

static_assert(!std::is_trivially_constructible_v<non_trivial, non_trivial>);
static_assert(!std::is_trivially_copy_constructible_v<non_trivial>);
static_assert(!std::is_trivially_move_constructible_v<non_trivial>);
static_assert(!std::is_trivially_destructible_v<non_trivial>);

struct non_trivial_copyable : copyable, non_trivial {};

static_assert(!std::is_trivially_constructible_v<non_trivial_copyable, non_trivial_copyable>);
static_assert(!std::is_trivially_copy_constructible_v<non_trivial_copyable>);
static_assert(!std::is_trivially_move_constructible_v<non_trivial_copyable>);
static_assert(!std::is_trivially_destructible_v<non_trivial_copyable>);

// Assert that fitx::result maintains the properties common to its error and
// value types.
static_assert(std::is_trivially_copy_constructible_v<fitx::result<trivial, trivial>>);
static_assert(!std::is_trivially_copy_constructible_v<fitx::result<trivial, non_trivial>>);
static_assert(!std::is_trivially_copy_constructible_v<fitx::result<non_trivial, trivial>>);
static_assert(!std::is_trivially_copy_constructible_v<fitx::result<non_trivial, non_trivial>>);

static_assert(std::is_trivially_move_constructible_v<fitx::result<trivial, trivial>>);
static_assert(!std::is_trivially_move_constructible_v<fitx::result<trivial, non_trivial>>);
static_assert(!std::is_trivially_move_constructible_v<fitx::result<non_trivial, trivial>>);
static_assert(!std::is_trivially_move_constructible_v<fitx::result<non_trivial, non_trivial>>);

static_assert(std::is_trivially_destructible_v<fitx::result<trivial, trivial>>);
static_assert(!std::is_trivially_destructible_v<fitx::result<trivial, non_trivial>>);
static_assert(!std::is_trivially_destructible_v<fitx::result<non_trivial, trivial>>);
static_assert(!std::is_trivially_destructible_v<fitx::result<non_trivial, non_trivial>>);

static_assert(
    !std::is_default_constructible_v<fitx::result<default_constructible, default_constructible>>);
static_assert(!std::is_default_constructible_v<
              fitx::result<default_constructible, non_default_constructible>>);
static_assert(!std::is_default_constructible_v<
              fitx::result<non_default_constructible, default_constructible>>);
static_assert(!std::is_default_constructible_v<
              fitx::result<non_default_constructible, non_default_constructible>>);

static_assert(std::is_copy_constructible_v<fitx::result<copyable, copyable>>);
static_assert(!std::is_copy_constructible_v<fitx::result<copyable, move_only>>);
static_assert(!std::is_copy_constructible_v<fitx::result<move_only, copyable>>);
static_assert(!std::is_copy_constructible_v<fitx::result<move_only, move_only>>);
static_assert(
    std::is_copy_constructible_v<fitx::result<non_trivial_copyable, non_trivial_copyable>>);

static_assert(std::is_copy_assignable_v<fitx::result<copyable, copyable>>);
static_assert(!std::is_copy_assignable_v<fitx::result<copyable, move_only>>);
static_assert(!std::is_copy_assignable_v<fitx::result<move_only, copyable>>);
static_assert(!std::is_copy_assignable_v<fitx::result<move_only, move_only>>);
static_assert(std::is_copy_assignable_v<fitx::result<non_trivial_copyable, non_trivial_copyable>>);

static_assert(std::is_trivially_copy_constructible_v<fitx::result<trivial, trivial>>);
static_assert(!std::is_trivially_copy_constructible_v<fitx::result<trivial, non_trivial>>);
static_assert(!std::is_trivially_copy_constructible_v<fitx::result<non_trivial, trivial>>);
static_assert(!std::is_trivially_copy_constructible_v<fitx::result<non_trivial, non_trivial>>);
static_assert(!std::is_trivially_copy_constructible_v<
              fitx::result<non_trivial_copyable, non_trivial_copyable>>);

static_assert(std::is_move_constructible_v<fitx::result<copyable, copyable>>);
static_assert(std::is_move_constructible_v<fitx::result<copyable, move_only>>);
static_assert(std::is_move_constructible_v<fitx::result<move_only, copyable>>);
static_assert(std::is_move_constructible_v<fitx::result<move_only, move_only>>);
static_assert(
    std::is_move_constructible_v<fitx::result<non_trivial_copyable, non_trivial_copyable>>);

static_assert(std::is_move_assignable_v<fitx::result<copyable, copyable>>);
static_assert(std::is_move_assignable_v<fitx::result<copyable, move_only>>);
static_assert(std::is_move_assignable_v<fitx::result<move_only, copyable>>);
static_assert(std::is_move_assignable_v<fitx::result<move_only, move_only>>);
static_assert(std::is_move_assignable_v<fitx::result<non_trivial_copyable, non_trivial_copyable>>);

static_assert(std::is_nothrow_move_constructible_v<fitx::result<copyable, copyable>>);
static_assert(std::is_nothrow_move_constructible_v<fitx::result<copyable, move_only>>);
static_assert(std::is_nothrow_move_constructible_v<fitx::result<move_only, copyable>>);
static_assert(std::is_nothrow_move_constructible_v<fitx::result<move_only, move_only>>);
static_assert(
    std::is_nothrow_move_constructible_v<fitx::result<non_trivial_copyable, non_trivial_copyable>>);

static_assert(std::is_trivially_move_constructible_v<fitx::result<trivial, trivial>>);
static_assert(!std::is_trivially_move_constructible_v<fitx::result<trivial, non_trivial>>);
static_assert(!std::is_trivially_move_constructible_v<fitx::result<non_trivial, trivial>>);
static_assert(!std::is_trivially_move_constructible_v<fitx::result<non_trivial, non_trivial>>);
static_assert(!std::is_trivially_move_constructible_v<
              fitx::result<non_trivial_copyable, non_trivial_copyable>>);

// Assert that fitx::error maintains the properties of its error type.
static_assert(std::is_trivially_copy_constructible_v<fitx::error<trivial>>);
static_assert(!std::is_trivially_copy_constructible_v<fitx::error<non_trivial>>);

static_assert(std::is_trivially_move_constructible_v<fitx::error<trivial>>);
static_assert(!std::is_trivially_move_constructible_v<fitx::error<non_trivial>>);

static_assert(std::is_trivially_destructible_v<fitx::error<trivial>>);
static_assert(!std::is_trivially_destructible_v<fitx::error<non_trivial>>);

static_assert(std::is_default_constructible_v<fitx::error<default_constructible>>);
static_assert(!std::is_default_constructible_v<fitx::error<non_default_constructible>>);

static_assert(std::is_copy_constructible_v<fitx::error<copyable>>);
static_assert(!std::is_copy_constructible_v<fitx::error<move_only>>);

static_assert(std::is_trivially_copy_constructible_v<fitx::error<trivial>>);
static_assert(!std::is_trivially_copy_constructible_v<fitx::error<non_trivial>>);

static_assert(std::is_move_constructible_v<fitx::error<copyable>>);
static_assert(std::is_move_constructible_v<fitx::error<move_only>>);

static_assert(std::is_nothrow_move_constructible_v<fitx::error<copyable>>);
static_assert(std::is_nothrow_move_constructible_v<fitx::error<move_only>>);

static_assert(std::is_trivially_move_constructible_v<fitx::error<trivial>>);
static_assert(!std::is_trivially_move_constructible_v<fitx::error<non_trivial>>);

namespace comparison_tests {

struct greater {};
struct less {};
struct empty {};

constexpr bool operator==(greater, greater) { return true; }
constexpr bool operator<=(greater, greater) { return true; }
constexpr bool operator>=(greater, greater) { return true; }
constexpr bool operator!=(greater, greater) { return false; }
constexpr bool operator<(greater, greater) { return false; }
constexpr bool operator>(greater, greater) { return false; }

constexpr bool operator==(less, less) { return true; }
constexpr bool operator<=(less, less) { return true; }
constexpr bool operator>=(less, less) { return true; }
constexpr bool operator!=(less, less) { return false; }
constexpr bool operator<(less, less) { return false; }
constexpr bool operator>(less, less) { return false; }

constexpr bool operator==(greater, less) { return false; }
constexpr bool operator<=(greater, less) { return false; }
constexpr bool operator>=(greater, less) { return true; }
constexpr bool operator!=(greater, less) { return true; }
constexpr bool operator<(greater, less) { return false; }
constexpr bool operator>(greater, less) { return true; }

constexpr bool operator==(less, greater) { return false; }
constexpr bool operator<=(less, greater) { return true; }
constexpr bool operator>=(less, greater) { return false; }
constexpr bool operator!=(less, greater) { return true; }
constexpr bool operator<(less, greater) { return true; }
constexpr bool operator>(less, greater) { return false; }

// Note these definitions match the empty-to-other, other-to-empty, and
// empty-to-empty comparison behavior of fitx::result for convenience in
// exhaustive testing.
constexpr bool operator==(empty, greater) { return false; }
constexpr bool operator<=(empty, greater) { return true; }
constexpr bool operator>=(empty, greater) { return false; }
constexpr bool operator!=(empty, greater) { return true; }
constexpr bool operator<(empty, greater) { return true; }
constexpr bool operator>(empty, greater) { return false; }

constexpr bool operator==(greater, empty) { return false; }
constexpr bool operator<=(greater, empty) { return false; }
constexpr bool operator>=(greater, empty) { return true; }
constexpr bool operator!=(greater, empty) { return true; }
constexpr bool operator<(greater, empty) { return false; }
constexpr bool operator>(greater, empty) { return true; }

constexpr bool operator==(empty, less) { return false; }
constexpr bool operator<=(empty, less) { return true; }
constexpr bool operator>=(empty, less) { return false; }
constexpr bool operator!=(empty, less) { return true; }
constexpr bool operator<(empty, less) { return true; }
constexpr bool operator>(empty, less) { return false; }

constexpr bool operator==(less, empty) { return false; }
constexpr bool operator<=(less, empty) { return false; }
constexpr bool operator>=(less, empty) { return true; }
constexpr bool operator!=(less, empty) { return true; }
constexpr bool operator<(less, empty) { return false; }
constexpr bool operator>(less, empty) { return true; }

constexpr bool operator==(empty, empty) { return true; }
constexpr bool operator<=(empty, empty) { return true; }
constexpr bool operator>=(empty, empty) { return true; }
constexpr bool operator!=(empty, empty) { return false; }
constexpr bool operator<(empty, empty) { return false; }
constexpr bool operator>(empty, empty) { return false; }

template <typename T, typename U>
constexpr bool match_comparisons(T, U) {
  constexpr T lhs{};
  constexpr U rhs{};

  constexpr fitx::result<empty, T> ok_lhs{fitx::ok(lhs)};
  constexpr fitx::result<empty, U> ok_rhs{fitx::ok(rhs)};
  constexpr fitx::result<empty, T> error_lhs{fitx::error(empty{})};
  constexpr fitx::result<empty, U> error_rhs{fitx::error(empty{})};

  // Both result operands.
  static_assert((ok_lhs == ok_rhs) == (lhs == rhs));
  static_assert((ok_lhs != ok_rhs) == (lhs != rhs));
  static_assert((ok_lhs <= ok_rhs) == (lhs <= rhs));
  static_assert((ok_lhs >= ok_rhs) == (lhs >= rhs));
  static_assert((ok_lhs < ok_rhs) == (lhs < rhs));
  static_assert((ok_lhs > ok_rhs) == (lhs > rhs));

  static_assert((error_lhs == ok_rhs) == (empty{} == rhs));
  static_assert((error_lhs != ok_rhs) == (empty{} != rhs));
  static_assert((error_lhs <= ok_rhs) == (empty{} <= rhs));
  static_assert((error_lhs >= ok_rhs) == (empty{} >= rhs));
  static_assert((error_lhs < ok_rhs) == (empty{} < rhs));
  static_assert((error_lhs > ok_rhs) == (empty{} > rhs));

  static_assert((ok_lhs == error_rhs) == (lhs == empty{}));
  static_assert((ok_lhs != error_rhs) == (lhs != empty{}));
  static_assert((ok_lhs <= error_rhs) == (lhs <= empty{}));
  static_assert((ok_lhs >= error_rhs) == (lhs >= empty{}));
  static_assert((ok_lhs < error_rhs) == (lhs < empty{}));
  static_assert((ok_lhs > error_rhs) == (lhs > empty{}));

  static_assert((error_lhs == error_rhs) == (empty{} == empty{}));
  static_assert((error_lhs != error_rhs) == (empty{} != empty{}));
  static_assert((error_lhs <= error_rhs) == (empty{} <= empty{}));
  static_assert((error_lhs >= error_rhs) == (empty{} >= empty{}));
  static_assert((error_lhs < error_rhs) == (empty{} < empty{}));
  static_assert((error_lhs > error_rhs) == (empty{} > empty{}));

  // Right hand result only.
  static_assert((lhs == ok_rhs) == (lhs == rhs));
  static_assert((lhs != ok_rhs) == (lhs != rhs));
  static_assert((lhs <= ok_rhs) == (lhs <= rhs));
  static_assert((lhs >= ok_rhs) == (lhs >= rhs));
  static_assert((lhs < ok_rhs) == (lhs < rhs));
  static_assert((lhs > ok_rhs) == (lhs > rhs));

  static_assert((lhs == error_rhs) == (lhs == empty{}));
  static_assert((lhs != error_rhs) == (lhs != empty{}));
  static_assert((lhs <= error_rhs) == (lhs <= empty{}));
  static_assert((lhs >= error_rhs) == (lhs >= empty{}));
  static_assert((lhs < error_rhs) == (lhs < empty{}));
  static_assert((lhs > error_rhs) == (lhs > empty{}));

  // Left hand result only.
  static_assert((ok_lhs == rhs) == (lhs == rhs));
  static_assert((ok_lhs != rhs) == (lhs != rhs));
  static_assert((ok_lhs <= rhs) == (lhs <= rhs));
  static_assert((ok_lhs >= rhs) == (lhs >= rhs));
  static_assert((ok_lhs < rhs) == (lhs < rhs));
  static_assert((ok_lhs > rhs) == (lhs > rhs));

  static_assert((error_lhs == rhs) == (empty{} == rhs));
  static_assert((error_lhs != rhs) == (empty{} != rhs));
  static_assert((error_lhs <= rhs) == (empty{} <= rhs));
  static_assert((error_lhs >= rhs) == (empty{} >= rhs));
  static_assert((error_lhs < rhs) == (empty{} < rhs));
  static_assert((error_lhs > rhs) == (empty{} > rhs));

  return true;
}

static_assert(match_comparisons(greater{}, greater{}));
static_assert(match_comparisons(greater{}, less{}));
static_assert(match_comparisons(less{}, greater{}));
static_assert(match_comparisons(less{}, less{}));

}  // namespace comparison_tests

#if defined(__Fuchsia__)

TEST(LibZxCommon, Abort) {
  // Validate that accessing the error of a non-error result aborts.
  ASSERT_DEATH(([] {
    fitx::result<nothing, int> result{fitx::ok(10)};
    EXPECT_FALSE(result.is_error());
    EXPECT_TRUE(result.is_ok());
    result.error_value();
  }));
  ASSERT_DEATH(([] {
    const fitx::result<nothing, int> result{fitx::ok(10)};
    EXPECT_FALSE(result.is_error());
    EXPECT_TRUE(result.is_ok());
    result.error_value();
  }));
  ASSERT_DEATH(([] {
    fitx::result<nothing, int> result{fitx::ok(10)};
    EXPECT_FALSE(result.is_error());
    EXPECT_TRUE(result.is_ok());
    result.take_error();
  }));

  // Validate that accessing the value of an error result aborts.
  ASSERT_DEATH(([] {
    fitx::result<nothing, int> result{fitx::error(nothing{})};
    EXPECT_TRUE(result.is_error());
    EXPECT_FALSE(result.is_ok());
    result.value();
  }));
  ASSERT_DEATH(([] {
    const fitx::result<nothing, int> result{fitx::error(nothing{})};
    EXPECT_TRUE(result.is_error());
    EXPECT_FALSE(result.is_ok());
    result.value();
  }));
  ASSERT_DEATH(([] {
    fitx::result<nothing, int> result{fitx::error(nothing{})};
    EXPECT_TRUE(result.is_error());
    EXPECT_FALSE(result.is_ok());
    std::move(result).value();
  }));
  ASSERT_DEATH(([] {
    const fitx::result<nothing, int> result{fitx::error(nothing{})};
    EXPECT_TRUE(result.is_error());
    EXPECT_FALSE(result.is_ok());
    std::move(result).value();
  }));
  ASSERT_DEATH(([] {
    fitx::result<nothing, int> result{fitx::error(nothing{})};
    *result;
  }));
  ASSERT_DEATH(([] {
    const fitx::result<nothing, int> result{fitx::error(nothing{})};
    *result;
  }));
  ASSERT_DEATH(([] {
    fitx::result<nothing, int> result{fitx::error(nothing{})};
    *std::move(result);
  }));
  ASSERT_DEATH(([] {
    const fitx::result<nothing, int> result{fitx::error(nothing{})};
    *std::move(result);
  }));
  ASSERT_DEATH(([] {
    fitx::result<nothing, test_members> result{fitx::error(nothing{})};
    EXPECT_TRUE(result.is_error());
    EXPECT_FALSE(result.is_ok());
    (void)result->a;
  }));
  ASSERT_DEATH(([] {
    const fitx::result<nothing, test_members> result{fitx::error(nothing{})};
    EXPECT_TRUE(result.is_error());
    EXPECT_FALSE(result.is_ok());
    (void)result->a;
  }));
  ASSERT_DEATH(([] {
    fitx::result<nothing, std::optional<test_members>> result{fitx::error(nothing{})};
    EXPECT_TRUE(result.is_error());
    EXPECT_FALSE(result.is_ok());
    (void)result->a;
  }));
  ASSERT_DEATH(([] {
    const fitx::result<nothing, std::optional<test_members>> result{fitx::error(nothing{})};
    EXPECT_TRUE(result.is_error());
    EXPECT_FALSE(result.is_ok());
    (void)result->a;
  }));

  // Validate that attempting to use ZX_OK as an explicit error aborts.
  ASSERT_DEATH(([] {
    zx::status<> status{zx::error_status(ZX_OK)};
    (void)status;
  }));

  // Validate that forwarding ZX_OK does not abort.
  ASSERT_NO_DEATH(([] {
    zx::status<> status{zx::make_status(ZX_OK)};
    EXPECT_FALSE(status.is_error());
    EXPECT_TRUE(status.is_ok());
  }));

  // Validate that accessing the error of a non-error zx::status through
  // status_value() does not abort.
  ASSERT_NO_DEATH(([] {
    zx::status<int> status{zx::ok(10)};
    EXPECT_FALSE(status.is_error());
    EXPECT_TRUE(status.is_ok());
    EXPECT_EQ(ZX_OK, status.status_value());
  }));
  ASSERT_NO_DEATH(([] {
    const zx::status<int> status{zx::ok(10)};
    EXPECT_FALSE(status.is_error());
    EXPECT_TRUE(status.is_ok());
    EXPECT_EQ(ZX_OK, status.status_value());
  }));

  // Validate the other error accessors abort.
  ASSERT_DEATH(([] {
    zx::status<int> status{zx::ok(10)};
    EXPECT_FALSE(status.is_error());
    EXPECT_TRUE(status.is_ok());
    EXPECT_EQ(ZX_OK, status.error_value());
  }));
  ASSERT_DEATH(([] {
    const zx::status<int> status{zx::ok(10)};
    EXPECT_FALSE(status.is_error());
    EXPECT_TRUE(status.is_ok());
    EXPECT_EQ(ZX_OK, status.error_value());
  }));
  ASSERT_DEATH(([] {
    zx::status<int> status{zx::ok(10)};
    EXPECT_FALSE(status.is_error());
    EXPECT_TRUE(status.is_ok());
    status.take_error();
  }));
}

#endif  // defined(__Fuchsia__)

// Validate copy/move construction and assignment.
enum non_default_t { non_default_v };

template <size_t index>
struct counter {
  counter() { default_constructor_count++; }

  counter(non_default_t) { non_default_constructor_count++; }

  counter(const counter&) { copy_constructor_count++; }
  counter& operator=(const counter&) {
    copy_assignment_count++;
    return *this;
  }

  counter(counter&&) noexcept { move_constructor_count++; }
  counter& operator=(counter&&) noexcept {
    move_assignment_count++;
    return *this;
  }

  ~counter() { destructor_count++; }

  static void reset() {
    default_constructor_count = 0;
    non_default_constructor_count = 0;
    copy_constructor_count = 0;
    copy_assignment_count = 0;
    move_constructor_count = 0;
    move_assignment_count = 0;
    destructor_count = 0;
  }

  static int constructor_count() {
    return default_constructor_count + non_default_constructor_count + copy_constructor_count +
           move_constructor_count;
  }
  static int assignment_count() { return copy_assignment_count + move_assignment_count; }
  static int alive_count() { return constructor_count() - destructor_count; }

  static int copy_count() { return copy_constructor_count + copy_assignment_count; }
  static int move_count() { return move_constructor_count + move_assignment_count; }

  inline static int default_constructor_count{0};
  inline static int non_default_constructor_count{0};
  inline static int copy_constructor_count{0};
  inline static int copy_assignment_count{0};
  inline static int move_constructor_count{0};
  inline static int move_assignment_count{0};
  inline static int destructor_count{0};
};

using counter_a = counter<0>;
using counter_b = counter<1>;

fitx::result<counter_a, counter_b> get_values() { return fitx::ok(counter_b{non_default_v}); }
fitx::result<counter_a, counter_b> get_error() { return fitx::error(non_default_v); }

TEST(LibZxCommon, BasicConstructorDestructor) {
  counter_a::reset();
  counter_b::reset();

  {
    auto result = get_values();

    EXPECT_EQ(0, counter_a::constructor_count());
    EXPECT_EQ(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::default_constructor_count);
    EXPECT_NE(0, counter_b::constructor_count());
    EXPECT_NE(0, counter_b::alive_count());
  }

  EXPECT_EQ(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_NE(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();
}

TEST(LibZxCommon, Assignment) {
  // Fill with non-zero values to prevent the compiler from optimizing the assignment to a
  // constructor call.
  auto result1 = get_values();
  auto result2 = get_values();

  counter_a::reset();
  counter_b::reset();

  // This should be a move assignment of the value (counter_b).
  result1 = std::move(result2);
  EXPECT_EQ(0, counter_a::copy_count());
  EXPECT_EQ(0, counter_b::copy_count());
  EXPECT_EQ(0, counter_a::move_count());
  EXPECT_EQ(1, counter_b::move_count());

  counter_b::reset();

  // This should be a copy assignment of the value (counter_b).
  result2 = result1;
  EXPECT_EQ(0, counter_a::copy_count());
  EXPECT_EQ(1, counter_b::copy_count());
  EXPECT_EQ(0, counter_a::move_count());
  EXPECT_EQ(0, counter_b::move_count());
}

TEST(LibZxCommon, Accessors) {
  counter_a::reset();
  counter_b::reset();

  {
    auto result = get_values();
    auto b = result.value();

    static_assert(std::is_same_v<decltype(b), counter_b>);
    static_assert(std::is_same_v<decltype(result.value()), counter_b&>);
    static_assert(std::is_same_v<decltype(*result), counter_b&>);

    EXPECT_EQ(0, counter_a::constructor_count());
    EXPECT_EQ(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::default_constructor_count);
    EXPECT_NE(0, counter_b::constructor_count());
    EXPECT_NE(0, counter_b::alive_count());
  }

  EXPECT_EQ(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_NE(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();

  {
    const auto result = get_values();
    auto b = result.value();

    static_assert(std::is_same_v<decltype(b), counter_b>);
    static_assert(std::is_same_v<decltype(result.value()), const counter_b&>);
    static_assert(std::is_same_v<decltype(*result), const counter_b&>);

    EXPECT_EQ(0, counter_a::constructor_count());
    EXPECT_EQ(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::default_constructor_count);
    EXPECT_NE(0, counter_b::constructor_count());
    EXPECT_NE(0, counter_b::alive_count());
  }

  EXPECT_EQ(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_NE(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();

  {
    const auto& result = get_values();
    auto b = result.value();

    static_assert(std::is_same_v<decltype(b), counter_b>);
    static_assert(std::is_same_v<decltype(result.value()), const counter_b&>);
    static_assert(std::is_same_v<decltype(*result), const counter_b&>);

    EXPECT_EQ(0, counter_a::constructor_count());
    EXPECT_EQ(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::default_constructor_count);
    EXPECT_NE(0, counter_b::constructor_count());
    EXPECT_NE(0, counter_b::alive_count());
  }

  EXPECT_EQ(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_NE(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();

  {
    auto result = get_values();
    auto b = std::move(result).value();

    static_assert(std::is_same_v<decltype(b), counter_b>);
    static_assert(std::is_same_v<decltype(std::move(result).value()), counter_b&&>);
    static_assert(std::is_same_v<decltype(*std::move(result)), counter_b&&>);

    EXPECT_EQ(0, counter_a::constructor_count());
    EXPECT_EQ(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::default_constructor_count);
    EXPECT_NE(0, counter_b::constructor_count());
    EXPECT_NE(0, counter_b::alive_count());
  }

  EXPECT_EQ(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_NE(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();

  {
    const auto result = get_values();
    auto b = std::move(result).value();

    static_assert(std::is_same_v<decltype(b), counter_b>);
    static_assert(std::is_same_v<decltype(std::move(result).value()), const counter_b&&>);
    static_assert(std::is_same_v<decltype(*std::move(result)), const counter_b&&>);

    EXPECT_EQ(0, counter_a::constructor_count());
    EXPECT_EQ(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::default_constructor_count);
    EXPECT_NE(0, counter_b::constructor_count());
    EXPECT_NE(0, counter_b::alive_count());
  }

  EXPECT_EQ(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_NE(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();

  {
    auto result = get_values();
    auto b = result.take_value();

    static_assert(std::is_same_v<decltype(b), fitx::success<counter_b>>);
    static_assert(std::is_same_v<decltype(result.take_value()), fitx::success<counter_b>>);

    EXPECT_EQ(0, counter_a::constructor_count());
    EXPECT_EQ(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::default_constructor_count);
    EXPECT_NE(0, counter_b::constructor_count());
    EXPECT_NE(0, counter_b::alive_count());
  }

  EXPECT_EQ(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_NE(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();
}

TEST(LibZxCommon, ErrorResults) {
  counter_a::reset();
  counter_b::reset();

  {
    auto result = get_error();
    auto error = result.error_value();

    static_assert(std::is_same_v<decltype(error), counter_a>);

    EXPECT_EQ(0, counter_a::default_constructor_count);
    EXPECT_NE(0, counter_a::constructor_count());
    EXPECT_NE(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::constructor_count());
    EXPECT_EQ(0, counter_b::alive_count());
  }

  EXPECT_NE(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_EQ(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();

  {
    auto result = get_error();
    auto& error = result.error_value();

    static_assert(std::is_same_v<decltype(error), counter_a&>);

    EXPECT_EQ(0, counter_a::default_constructor_count);
    EXPECT_NE(0, counter_a::constructor_count());
    EXPECT_NE(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::constructor_count());
    EXPECT_EQ(0, counter_b::alive_count());
  }

  EXPECT_NE(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_EQ(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();

  {
    auto result = get_error();
    const auto& error = result.error_value();

    static_assert(std::is_same_v<decltype(error), const counter_a&>);

    EXPECT_EQ(0, counter_a::default_constructor_count);
    EXPECT_NE(0, counter_a::constructor_count());
    EXPECT_NE(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::constructor_count());
    EXPECT_EQ(0, counter_b::alive_count());
  }

  EXPECT_NE(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_EQ(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();

  {
    auto result = get_error();
    auto error = result.take_error();

    static_assert(std::is_same_v<decltype(error), fitx::error<counter_a>>);

    EXPECT_EQ(0, counter_a::default_constructor_count);
    EXPECT_NE(0, counter_a::constructor_count());
    EXPECT_NE(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::constructor_count());
    EXPECT_EQ(0, counter_b::alive_count());
  }

  EXPECT_NE(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_EQ(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();

  {
    auto result = get_error();
    const auto& error = result.take_error();

    static_assert(std::is_same_v<decltype(error), const fitx::error<counter_a>&>);

    EXPECT_EQ(0, counter_a::default_constructor_count);
    EXPECT_NE(0, counter_a::constructor_count());
    EXPECT_NE(0, counter_a::alive_count());

    EXPECT_EQ(0, counter_b::constructor_count());
    EXPECT_EQ(0, counter_b::alive_count());
  }

  EXPECT_NE(0, counter_a::constructor_count());
  EXPECT_EQ(0, counter_a::alive_count());

  EXPECT_EQ(0, counter_b::constructor_count());
  EXPECT_EQ(0, counter_b::alive_count());

  counter_a::reset();
  counter_b::reset();
}

// status_string() is only defined in userspace Fuchsia code
#if defined(__Fuchsia__)

TEST(LibZxCommon, StatusString) {
  {
    zx::status<> status = zx::ok();
    EXPECT_STREQ(status.status_string(), zx_status_get_string(ZX_OK));
  }

  {
    zx::status<> status = zx::error(ZX_ERR_NO_MEMORY);
    EXPECT_STREQ(status.status_string(), zx_status_get_string(ZX_ERR_NO_MEMORY));
  }

  {
    zx::status<int> status = zx::ok(10);
    EXPECT_STREQ(status.status_string(), zx_status_get_string(ZX_OK));
  }

  {
    zx::status<int> status = zx::error(ZX_ERR_NO_MEMORY);
    EXPECT_STREQ(status.status_string(), zx_status_get_string(ZX_ERR_NO_MEMORY));
  }
}

#endif  // defined(__Fuchsia__)

struct ErrorMsg {
  zx_status_t status;
  std::vector<std::string> details{};

  void operator+=(std::string value) { details.push_back(std::move(value)); }
};

TEST(LibZxCommon, AugmentError) {
  {
    fitx::result<std::string> result = fitx::error("Bad outcome!");
    result += fitx::error("More details!");
    EXPECT_STREQ(result.error_value(), "Bad outcome!More details!");
  }

  {
    fitx::result<std::string, int> result = fitx::error("Bad outcome!");
    result += fitx::error("More details!");
    EXPECT_STREQ(result.error_value(), "Bad outcome!More details!");
  }

  {
    fitx::result<ErrorMsg> result = fitx::error(ErrorMsg{ZX_ERR_NOT_FOUND});
    EXPECT_EQ(0, result.error_value().details.size());

    result += fitx::error("More details!");
    ASSERT_EQ(1, result.error_value().details.size());
    EXPECT_STREQ(result.error_value().details[0], "More details!");
  }

  {
    fitx::result<ErrorMsg, int> result = fitx::error(ErrorMsg{ZX_ERR_NOT_FOUND});
    EXPECT_EQ(0, result.error_value().details.size());

    result += fitx::error("More details!");
    ASSERT_EQ(1, result.error_value().details.size());
    EXPECT_STREQ(result.error_value().details[0], "More details!");
  }
}

// Ensure that the r-value overloads of value() and error_value() work as expected.
//
// The r-value overloads cause expressions such as the following:
//
//   MyFunction().value()
//   std::move(result).value()
//
// to be moves and not copies.
TEST(ResultTests, ResultRvalueOverloads) {
  // result.value() &&
  {
    fitx::result<int, move_only> result = fitx::success<move_only>();
    move_only value = std::move(result).value();
    (void)value;
  }

  // result.error_value() &&
  {
    fitx::result<move_only, int> moved_error = fitx::error<move_only>();
    move_only value = std::move(moved_error).error_value();
    (void)value;
  }
}

// Test that operator*() functions on single-value result types.
TEST(ResultTests, OperatorStar) {
  {
    fitx::result<int, move_only> result = fitx::success<move_only>();
    move_only value = std::move(*result);
    (void)value;
  }
  {
    fitx::result<int, move_only> result = fitx::success<move_only>();
    move_only value = *std::move(result);
    (void)value;
  }
}

TEST(LibZxCommon, MakeStatusWithValueType) {
  auto divide = [](int x, int y, int* output) -> zx_status_t {
    if (y == 0) {
      return ZX_ERR_INVALID_ARGS;
    }
    *output = x / y;
    return ZX_OK;
  };

  {
    int n;
    auto status = zx::make_status(divide(9, 3, &n), n);
    ASSERT_TRUE(status.is_ok());
    ASSERT_EQ(status.value(), 3);
  }

  {
    int n;
    auto status = zx::make_status(divide(9, 0, &n), n);
    ASSERT_TRUE(status.is_error());
    ASSERT_EQ(status.error_value(), ZX_ERR_INVALID_ARGS);
  }
}

TEST(LibZxCommon, MakeStatusWithReferenceType) {
  auto divide = [](int x, int y, int& output) -> zx_status_t {
    if (y == 0) {
      return ZX_ERR_INVALID_ARGS;
    }
    output = x / y;
    return ZX_OK;
  };

  {
    int v;
    int& r = v;
    auto status = zx::make_status(divide(9, 3, r), r);
    ASSERT_TRUE(status.is_ok());
    ASSERT_EQ(status.value(), 3);
  }

  {
    int v;
    int& r = v;
    auto status = zx::make_status(divide(9, 0, r), r);
    ASSERT_TRUE(status.is_error());
    ASSERT_EQ(status.error_value(), ZX_ERR_INVALID_ARGS);
  }
}

TEST(LibZxCommon, MakeStatusWithMoveOnlyType) {
  struct Num {
    constexpr Num(int i) : v(i) {}

    // Move only.
    constexpr Num(const Num&) = delete;
    constexpr Num& operator=(const Num&) const = delete;
    constexpr Num(Num&&) = default;
    constexpr Num& operator=(Num&&) = default;

    int v;
  };
  auto divide = [](int x, int y, Num& output) -> zx_status_t {
    if (y == 0) {
      return ZX_ERR_INVALID_ARGS;
    }
    output = Num(x / y);
    return ZX_OK;
  };

  {
    Num n(0);
    auto status = zx::make_status(divide(9, 3, n), std::move(n));
    ASSERT_TRUE(status.is_ok());
    ASSERT_EQ(status.value().v, 3);
  }

  {
    Num n(0);
    auto status = zx::make_status(divide(9, 0, n), std::move(n));
    ASSERT_TRUE(status.is_error());
    ASSERT_EQ(status.error_value(), ZX_ERR_INVALID_ARGS);
  }
}

TEST(LibZxCommon, Swap) {
  {
    fitx::result<char> result1 = fitx::ok();
    fitx::result<char> result2 = fitx::ok();
    EXPECT_TRUE(result1.is_ok());
    EXPECT_TRUE(result2.is_ok());

    result1.swap(result2);
    EXPECT_TRUE(result1.is_ok());
    EXPECT_TRUE(result2.is_ok());
  }
  {
    fitx::result<char> result1 = fitx::error('a');
    fitx::result<char> result2 = fitx::error('b');
    EXPECT_EQ(result1.error_value(), 'a');
    EXPECT_EQ(result2.error_value(), 'b');

    result1.swap(result2);
    EXPECT_EQ(result1.error_value(), 'b');
    EXPECT_EQ(result2.error_value(), 'a');
  }
  {
    fitx::result<char> result1 = fitx::ok();
    fitx::result<char> result2 = fitx::error('a');
    EXPECT_TRUE(result1.is_ok());
    EXPECT_TRUE(result2.is_error());
    EXPECT_EQ(result2.error_value(), 'a');

    result1.swap(result2);
    EXPECT_TRUE(result1.is_error());
    EXPECT_TRUE(result2.is_ok());
    EXPECT_EQ(result1.error_value(), 'a');
  }
  {
    fitx::result<char, int> result1 = fitx::ok(42);
    fitx::result<char, int> result2 = fitx::ok(43);
    EXPECT_EQ(result1.value(), 42);
    EXPECT_EQ(result2.value(), 43);

    result1.swap(result2);
    EXPECT_EQ(result1.value(), 43);
    EXPECT_EQ(result2.value(), 42);
  }
  {
    fitx::result<char, int> result1 = fitx::error('a');
    fitx::result<char, int> result2 = fitx::error('b');
    EXPECT_EQ(result1.error_value(), 'a');
    EXPECT_EQ(result2.error_value(), 'b');

    result1.swap(result2);
    EXPECT_EQ(result1.error_value(), 'b');
    EXPECT_EQ(result2.error_value(), 'a');
  }
  {
    fitx::result<char, int> result1 = fitx::ok(42);
    fitx::result<char, int> result2 = fitx::error('a');
    EXPECT_TRUE(result1.is_ok());
    EXPECT_TRUE(result2.is_error());
    EXPECT_EQ(result1.value(), 42);
    EXPECT_EQ(result2.error_value(), 'a');

    result1.swap(result2);
    EXPECT_TRUE(result1.is_error());
    EXPECT_TRUE(result2.is_ok());
    EXPECT_EQ(result1.error_value(), 'a');
    EXPECT_EQ(result2.value(), 42);
  }
  // Non-trivial
  {
    fitx::result<std::string> result1 = fitx::ok();
    fitx::result<std::string> result2 = fitx::ok();
    EXPECT_TRUE(result1.is_ok());
    EXPECT_TRUE(result2.is_ok());

    result1.swap(result2);
    EXPECT_TRUE(result1.is_ok());
    EXPECT_TRUE(result2.is_ok());
  }
  {
    fitx::result<std::string> result1 = fitx::error("asdf");
    fitx::result<std::string> result2 = fitx::error("jkl");
    EXPECT_STREQ(result1.error_value(), "asdf");
    EXPECT_STREQ(result2.error_value(), "jkl");

    result1.swap(result2);
    EXPECT_EQ(result1.error_value(), "jkl");
    EXPECT_EQ(result2.error_value(), "asdf");
  }
  {
    fitx::result<std::string> result1 = fitx::ok();
    fitx::result<std::string> result2 = fitx::error("asdf");
    EXPECT_TRUE(result1.is_ok());
    EXPECT_TRUE(result2.is_error());
    EXPECT_STREQ(result2.error_value(), "asdf");

    result1.swap(result2);
    EXPECT_TRUE(result1.is_error());
    EXPECT_TRUE(result2.is_ok());
    EXPECT_EQ(result1.error_value(), "asdf");
  }
  {
    fitx::result<std::string, std::string> result1 = fitx::ok("asdf");
    fitx::result<std::string, std::string> result2 = fitx::ok("jkl");
    EXPECT_STREQ(result1.value(), "asdf");
    EXPECT_STREQ(result2.value(), "jkl");

    result1.swap(result2);
    EXPECT_EQ(result1.value(), "jkl");
    EXPECT_EQ(result2.value(), "asdf");
  }
  {
    fitx::result<std::string, std::string> result1 = fitx::error("asdf");
    fitx::result<std::string, std::string> result2 = fitx::error("jkl");
    EXPECT_STREQ(result1.error_value(), "asdf");
    EXPECT_STREQ(result2.error_value(), "jkl");

    result1.swap(result2);
    EXPECT_EQ(result1.error_value(), "jkl");
    EXPECT_EQ(result2.error_value(), "asdf");
  }
  {
    fitx::result<std::string, std::string> result1 = fitx::ok("asdf");
    fitx::result<std::string, std::string> result2 = fitx::error("jkl");
    EXPECT_TRUE(result1.is_ok());
    EXPECT_TRUE(result2.is_error());
    EXPECT_STREQ(result1.value(), "asdf");
    EXPECT_STREQ(result2.error_value(), "jkl");

    result1.swap(result2);
    EXPECT_TRUE(result1.is_error());
    EXPECT_TRUE(result2.is_ok());
    EXPECT_EQ(result1.error_value(), "jkl");
    EXPECT_EQ(result2.value(), "asdf");
  }
}

}  // anonymous namespace
