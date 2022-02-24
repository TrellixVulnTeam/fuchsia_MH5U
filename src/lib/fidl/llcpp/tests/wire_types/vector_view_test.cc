// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/fidl/llcpp/vector_view.h>

#include <gtest/gtest.h>

TEST(VectorView, DefaultConstructor) {
  fidl::VectorView<int32_t> vv;
  EXPECT_EQ(vv.count(), 0ULL);
  EXPECT_TRUE(vv.empty());
  EXPECT_EQ(vv.data(), nullptr);
}

struct DestructionState {
  bool destructor_called = false;
};
struct DestructableObject {
  DestructableObject() : ds(nullptr) {}
  DestructableObject(DestructionState* ds) : ds(ds) {}

  ~DestructableObject() { ds->destructor_called = true; }

  DestructionState* ds;
};

TEST(VectorView, PointerConstructor) {
  DestructionState ds[3] = {};
  DestructableObject arr[3] = {&ds[0], &ds[1], &ds[2]};
  {
    auto vv = fidl::VectorView<DestructableObject>::FromExternal(arr, 2);
    EXPECT_EQ(vv.count(), 2ULL);
    EXPECT_FALSE(vv.empty());
    EXPECT_EQ(vv.data(), arr);
  }
  EXPECT_FALSE(ds[0].destructor_called);
  EXPECT_FALSE(ds[1].destructor_called);
  EXPECT_FALSE(ds[1].destructor_called);
}

TEST(VectorView, MoveConstructorUnowned) {
  std::vector<int32_t> vec{1, 2, 3};
  auto vv = fidl::VectorView<int32_t>::FromExternal(vec);
  fidl::VectorView<int32_t> moved_vv(std::move(vv));
  EXPECT_EQ(vv.count(), 3ULL);
  EXPECT_EQ(vv.data(), vec.data());
  EXPECT_EQ(moved_vv.count(), 3ULL);
  EXPECT_EQ(moved_vv.data(), vec.data());
}

TEST(VectorView, MoveAssigmentUnowned) {
  std::vector<int32_t> vec{1, 2, 3};
  auto vv = fidl::VectorView<int32_t>::FromExternal(vec);
  fidl::VectorView<int32_t> moved_vv;
  moved_vv = std::move(vv);
  EXPECT_EQ(vv.count(), 3ULL);
  EXPECT_EQ(vv.data(), vec.data());
  EXPECT_EQ(moved_vv.count(), 3ULL);
  EXPECT_EQ(moved_vv.data(), vec.data());
}

TEST(VectorView, Iteration) {
  std::vector<int32_t> vec{1, 2, 3};
  auto vv = fidl::VectorView<int32_t>::FromExternal(vec);
  int32_t i = 1;
  for (auto& val : vv) {
    EXPECT_EQ(&val, &vec.at(i - 1));
    ++i;
  }
  EXPECT_EQ(i, 4);
}

TEST(VectorView, Indexing) {
  std::vector<int32_t> vec{1, 2, 3};
  auto vv = fidl::VectorView<int32_t>::FromExternal(vec);
  for (uint64_t i = 0; i < vv.count(); i++) {
    EXPECT_EQ(&vv[i], &vec.at(i));
  }
}

TEST(VectorView, Mutations) {
  std::vector<int32_t> vec{1, 2, 3};
  auto vv = fidl::VectorView<int32_t>::FromExternal(vec);
  vv.set_count(2);
  *vv.mutable_data() = 4;
  vv[1] = 5;
  EXPECT_EQ(vv.count(), 2ULL);
  EXPECT_EQ(vv.data(), vec.data());
  EXPECT_EQ(vv.data(), vv.mutable_data());
  EXPECT_EQ(vv[0], 4);
  EXPECT_EQ(vv[1], 5);
  EXPECT_EQ(vec[0], 4);
  EXPECT_EQ(vec[1], 5);
}
