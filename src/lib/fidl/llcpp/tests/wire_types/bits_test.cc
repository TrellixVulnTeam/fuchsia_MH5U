// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/test.types/cpp/wire.h>
#include <lib/stdcompat/optional.h>

#include <gtest/gtest.h>

template <typename T>
class Bits : public ::testing::Test {};

TYPED_TEST_SUITE_P(Bits);

TYPED_TEST_P(Bits, BitwiseOperators) {
  TypeParam b_or_d = TypeParam::kB | TypeParam::kD;
  EXPECT_EQ(static_cast<uint8_t>(b_or_d), 6u /* 2 | 4*/);

  TypeParam b_or_e = TypeParam::kB | TypeParam::kE;
  EXPECT_EQ(static_cast<uint8_t>(b_or_e), 10u /* 2 | 8*/);

  TypeParam not_b = ~TypeParam::kB;
  EXPECT_EQ(static_cast<uint8_t>(not_b), 12u /* ~2 & (2 | 4 | 8)*/);

  TypeParam not_d = ~TypeParam::kD;
  EXPECT_EQ(static_cast<uint8_t>(not_d), 10u /* ~4 & (2 | 4 | 8)*/);

  TypeParam not_e = ~TypeParam::kE;
  EXPECT_EQ(static_cast<uint8_t>(not_e), 6u /* ~8 & (2 | 4 | 8)*/);

  TypeParam b_and_not_e = TypeParam::kB & ~TypeParam::kE;
  EXPECT_EQ(static_cast<uint8_t>(b_and_not_e), 2u /* 2 & 6*/);

  TypeParam b_or_d_and_b_or_e = (TypeParam::kB | TypeParam::kD) & (TypeParam::kB | TypeParam::kE);
  EXPECT_EQ(static_cast<uint8_t>(b_or_d_and_b_or_e), 2u /* 6 & 10*/);

  TypeParam b_xor_not_e = TypeParam::kB ^ ~TypeParam::kE;
  EXPECT_EQ(static_cast<uint8_t>(b_xor_not_e), 4u /* 4 ^ 6*/);

  TypeParam b_or_d_xor_b_or_e = (TypeParam::kB | TypeParam::kD) ^ (TypeParam::kB | TypeParam::kE);
  EXPECT_EQ(static_cast<uint8_t>(b_or_d_xor_b_or_e), 12u /* 6 ^ 10*/);
}

TYPED_TEST_P(Bits, BitwiseAssignOperators) {
  TypeParam b_or_d = TypeParam::kB;
  b_or_d |= TypeParam::kD;
  EXPECT_EQ(static_cast<uint8_t>(b_or_d), 6u /* 2 | 4*/);

  TypeParam b_and_not_e = TypeParam::kB;
  b_and_not_e &= ~TypeParam::kE;
  EXPECT_EQ(static_cast<uint8_t>(b_and_not_e), 2u /* 2 & 6*/);

  TypeParam b_xor_not_e = TypeParam::kB;
  b_xor_not_e ^= ~TypeParam::kE;
  EXPECT_EQ(static_cast<uint8_t>(b_xor_not_e), 4u /* 4 ^ 6*/);

  EXPECT_EQ(static_cast<uint8_t>(TypeParam::kB), 2u);
  EXPECT_EQ(static_cast<uint8_t>(TypeParam::kD), 4u);
  EXPECT_EQ(static_cast<uint8_t>(TypeParam::kE), 8u);
}

TYPED_TEST_P(Bits, IsConstexpr) {
  static constexpr auto this_should_compile = TypeParam::kB | TypeParam::kD | TypeParam::kE;
  EXPECT_EQ(this_should_compile, TypeParam::kMask);
}

TYPED_TEST_P(Bits, CanConvertToNumberButMustBeExplicit) {
  uint8_t r8 = static_cast<uint8_t>(TypeParam::kB);
  EXPECT_EQ(r8, 2u);
  uint16_t r16 = static_cast<uint8_t>(TypeParam::kB);
  EXPECT_EQ(r16, 2u);
}

TYPED_TEST_P(Bits, CanConvertToBool) {
  bool result = static_cast<bool>(TypeParam::kB);
  EXPECT_TRUE(result);
}

TYPED_TEST_P(Bits, TruncatingUnknown) {
  // The bits type only has 2, 4, and 8 defined.
  auto bits = TypeParam::TruncatingUnknown(1);
  EXPECT_EQ(static_cast<uint8_t>(bits), 0);
}

TYPED_TEST_P(Bits, TryFrom) {
  // The bits type only has 2, 4, and 8 defined.
  auto result = TypeParam::TryFrom(1);
  EXPECT_EQ(result, cpp17::nullopt);

  auto result_ok = TypeParam::TryFrom(2);
  EXPECT_EQ(result_ok, cpp17::optional<TypeParam>(TypeParam::kB));
}

TYPED_TEST_P(Bits, AllowingUnknownThroughStaticCast) {
  // The bits type only has 2, 4, and 8 defined.
  auto bits = static_cast<TypeParam>(1);
  EXPECT_EQ(static_cast<uint8_t>(bits), 1);
}

REGISTER_TYPED_TEST_SUITE_P(Bits, BitwiseOperators, BitwiseAssignOperators, IsConstexpr,
                            CanConvertToNumberButMustBeExplicit, CanConvertToBool,
                            TruncatingUnknown, TryFrom, AllowingUnknownThroughStaticCast);

using BitsTypesToTest =
    ::testing::Types<test_types::wire::StrictBits, test_types::wire::FlexibleBits>;
INSTANTIATE_TYPED_TEST_SUITE_P(BitsTests, Bits, BitsTypesToTest);

// The following APIs tested are only available on flexible bits.

TEST(Bits, QueryingUnknown) {
  using BitsType = test_types::wire::FlexibleBits;
  // The bits type only has 2, 4, and 8 defined.
  auto bits = static_cast<BitsType>(2 | 1);
  EXPECT_TRUE(bits.has_unknown_bits());
  EXPECT_EQ(static_cast<uint8_t>(bits.unknown_bits()), 1);

  bits = BitsType::TruncatingUnknown(2 | 1);
  EXPECT_FALSE(bits.has_unknown_bits());
  EXPECT_EQ(static_cast<uint8_t>(bits.unknown_bits()), 0);
}
