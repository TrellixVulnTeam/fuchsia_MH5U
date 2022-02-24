// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/image-format-llcpp/image-format-llcpp.h>
#include <lib/image-format/image_format.h>
#include <lib/sysmem-version/sysmem-version.h>

#include <fbl/array.h>
#include <zxtest/zxtest.h>

#include "fidl/fuchsia.sysmem/cpp/wire.h"
#include "fidl/fuchsia.sysmem2/cpp/wire.h"
#include "fuchsia/sysmem/c/fidl.h"

namespace sysmem_v1 = fuchsia_sysmem;
namespace sysmem_v2 = fuchsia_sysmem2;

TEST(ImageFormat, LinearComparison_V2_LLCPP) {
  fidl::Arena allocator;
  sysmem_v2::wire::PixelFormat plain(allocator);
  plain.set_type(sysmem_v2::wire::PixelFormatType::kBgra32);

  sysmem_v2::wire::PixelFormat linear(allocator);
  linear.set_type(sysmem_v2::wire::PixelFormatType::kBgra32);
  linear.set_format_modifier_value(allocator, sysmem_v2::wire::kFormatModifierLinear);

  sysmem_v2::wire::PixelFormat x_tiled(allocator);
  x_tiled.set_type(sysmem_v2::wire::PixelFormatType::kBgra32);
  x_tiled.set_format_modifier_value(allocator, sysmem_v2::wire::kFormatModifierIntelI915XTiled);

  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(plain, plain));
  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(linear, linear));

  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(plain, linear));
  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(linear, plain));

  EXPECT_FALSE(ImageFormatIsPixelFormatEqual(linear, x_tiled));
  EXPECT_FALSE(ImageFormatIsPixelFormatEqual(plain, x_tiled));
}

TEST(ImageFormat, LinearComparison_V1_LLCPP) {
  sysmem_v1::wire::PixelFormat plain = {
      .type = sysmem_v1::wire::PixelFormatType::kBgra32,
      .has_format_modifier = false,
  };

  sysmem_v1::wire::PixelFormat linear = {
      .type = sysmem_v1::wire::PixelFormatType::kBgra32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = sysmem_v1::wire::kFormatModifierLinear,
          },
  };

  sysmem_v1::wire::PixelFormat x_tiled = {
      .type = sysmem_v1::wire::PixelFormatType::kBgra32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = sysmem_v1::wire::kFormatModifierIntelI915XTiled,
          },
  };

  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(plain, plain));
  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(linear, linear));

  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(plain, linear));
  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(linear, plain));

  EXPECT_FALSE(ImageFormatIsPixelFormatEqual(linear, x_tiled));
  EXPECT_FALSE(ImageFormatIsPixelFormatEqual(plain, x_tiled));
}

TEST(ImageFormat, LinearComparison_V1_C) {
  fuchsia_sysmem_PixelFormat plain = {
      .type = fuchsia_sysmem_PixelFormatType_BGRA32,
      .has_format_modifier = false,
  };

  fuchsia_sysmem_PixelFormat linear = {
      .type = fuchsia_sysmem_PixelFormatType_BGRA32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = fuchsia_sysmem_FORMAT_MODIFIER_LINEAR,
          },
  };

  fuchsia_sysmem_PixelFormat x_tiled = {
      .type = fuchsia_sysmem_PixelFormatType_BGRA32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = fuchsia_sysmem_FORMAT_MODIFIER_INTEL_I915_X_TILED,
          },
  };

  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(plain, plain));
  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(linear, linear));

  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(plain, linear));
  EXPECT_TRUE(ImageFormatIsPixelFormatEqual(linear, plain));

  EXPECT_FALSE(ImageFormatIsPixelFormatEqual(linear, x_tiled));
  EXPECT_FALSE(ImageFormatIsPixelFormatEqual(plain, x_tiled));
}

TEST(ImageFormat, LinearRowBytes_V2_LLCPP) {
  fidl::Arena allocator;
  sysmem_v2::wire::PixelFormat linear(allocator);
  linear.set_type(sysmem_v2::wire::PixelFormatType::kBgra32);
  linear.set_format_modifier_value(allocator, sysmem_v2::wire::kFormatModifierLinear);
  sysmem_v2::wire::ImageFormatConstraints constraints(allocator);
  constraints.set_pixel_format(allocator, std::move(linear));
  constraints.set_min_coded_width(12u);
  constraints.set_max_coded_width(100u);
  constraints.set_bytes_per_row_divisor(4u * 8u);
  constraints.set_max_bytes_per_row(100000u);

  uint32_t row_bytes;
  EXPECT_TRUE(ImageFormatMinimumRowBytes(constraints, 17, &row_bytes));
  EXPECT_EQ(row_bytes, 4 * 24);

  EXPECT_FALSE(ImageFormatMinimumRowBytes(constraints, 11, &row_bytes));
  EXPECT_FALSE(ImageFormatMinimumRowBytes(constraints, 101, &row_bytes));
}

TEST(ImageFormat, LinearRowBytes_V1_LLCPP) {
  sysmem_v1::wire::PixelFormat linear = {
      .type = sysmem_v1::wire::PixelFormatType::kBgra32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = sysmem_v1::wire::kFormatModifierLinear,
          },
  };
  sysmem_v1::wire::ImageFormatConstraints constraints = {
      .pixel_format = linear,
      .min_coded_width = 12,
      .max_coded_width = 100,
      .max_bytes_per_row = 100000,
      .bytes_per_row_divisor = 4 * 8,
  };

  uint32_t row_bytes;
  EXPECT_TRUE(ImageFormatMinimumRowBytes(constraints, 17, &row_bytes));
  EXPECT_EQ(row_bytes, 4 * 24);

  EXPECT_FALSE(ImageFormatMinimumRowBytes(constraints, 11, &row_bytes));
  EXPECT_FALSE(ImageFormatMinimumRowBytes(constraints, 101, &row_bytes));
}

TEST(ImageFormat, LinearRowBytes_V1_C) {
  fuchsia_sysmem_PixelFormat linear = {
      .type = fuchsia_sysmem_PixelFormatType_BGRA32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = fuchsia_sysmem_FORMAT_MODIFIER_LINEAR,
          },
  };
  fuchsia_sysmem_ImageFormatConstraints constraints = {
      .pixel_format = linear,
      .min_coded_width = 12,
      .max_coded_width = 100,
      .max_bytes_per_row = 100000,
      .bytes_per_row_divisor = 4 * 8,
  };

  uint32_t row_bytes;
  EXPECT_TRUE(ImageFormatMinimumRowBytes(&constraints, 17, &row_bytes));
  EXPECT_EQ(row_bytes, 4 * 24);

  EXPECT_FALSE(ImageFormatMinimumRowBytes(&constraints, 11, &row_bytes));
  EXPECT_FALSE(ImageFormatMinimumRowBytes(&constraints, 101, &row_bytes));
}

TEST(ImageFormat, InvalidColorSpace_V1_LLCPP) {
  fidl::Arena allocator;
  auto sysmem_format_result = ImageFormatConvertZxToSysmem_v1(allocator, ZX_PIXEL_FORMAT_RGB_565);
  EXPECT_TRUE(sysmem_format_result.is_ok());
  auto sysmem_format = sysmem_format_result.take_value();

  sysmem_v1::wire::ColorSpace color_space{sysmem_v1::wire::ColorSpaceType::kInvalid};
  // Shouldn't crash.
  EXPECT_FALSE(ImageFormatIsSupportedColorSpaceForPixelFormat(color_space, sysmem_format));
}

TEST(ImageFormat, PassThroughColorSpace_V1_LLCPP) {
  fidl::Arena allocator;
  fuchsia_sysmem::wire::PixelFormat linear_bgra = {
      .type = fuchsia_sysmem::wire::PixelFormatType::kBgra32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = fuchsia_sysmem_FORMAT_MODIFIER_LINEAR,
          },
  };

  sysmem_v1::wire::ColorSpace color_space{sysmem_v1::wire::ColorSpaceType::kPassThrough};
  EXPECT_TRUE(ImageFormatIsSupportedColorSpaceForPixelFormat(color_space, linear_bgra));

  fuchsia_sysmem::wire::PixelFormat linear_nv12 = {
      .type = fuchsia_sysmem::wire::PixelFormatType::kNv12,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = fuchsia_sysmem_FORMAT_MODIFIER_LINEAR,
          },
  };

  EXPECT_TRUE(ImageFormatIsSupportedColorSpaceForPixelFormat(color_space, linear_nv12));
}

TEST(ImageFormat, ZxPixelFormat_V2_LLCPP) {
  fidl::Arena allocator;
  zx_pixel_format_t pixel_formats[] = {
      ZX_PIXEL_FORMAT_RGB_565,   ZX_PIXEL_FORMAT_RGB_332,  ZX_PIXEL_FORMAT_RGB_2220,
      ZX_PIXEL_FORMAT_ARGB_8888, ZX_PIXEL_FORMAT_RGB_x888, ZX_PIXEL_FORMAT_MONO_8,
      ZX_PIXEL_FORMAT_GRAY_8,    ZX_PIXEL_FORMAT_NV12,     ZX_PIXEL_FORMAT_RGB_888,
  };
  for (zx_pixel_format_t format : pixel_formats) {
    fprintf(stderr, "Format %x\n", format);
    auto sysmem_format_result = ImageFormatConvertZxToSysmem_v2(allocator, format);
    EXPECT_TRUE(sysmem_format_result.is_ok());
    sysmem_v2::wire::PixelFormat sysmem_format = sysmem_format_result.take_value();
    zx_pixel_format_t back_format;
    EXPECT_TRUE(ImageFormatConvertSysmemToZx(sysmem_format, &back_format));
    if (format == ZX_PIXEL_FORMAT_RGB_x888) {
      EXPECT_EQ(ZX_PIXEL_FORMAT_ARGB_8888, back_format);
    } else {
      EXPECT_EQ(back_format, format);
    }
    EXPECT_TRUE(sysmem_format.has_format_modifier_value());
    EXPECT_EQ(sysmem_v2::wire::kFormatModifierLinear,
              static_cast<uint64_t>(sysmem_format.format_modifier_value()));

    sysmem_v2::wire::ColorSpace color_space(allocator);
    if (format == ZX_PIXEL_FORMAT_NV12) {
      color_space.set_type(sysmem_v2::wire::ColorSpaceType::kRec601Ntsc);
    } else {
      color_space.set_type(sysmem_v2::wire::ColorSpaceType::kSrgb);
    }
    EXPECT_TRUE(ImageFormatIsSupportedColorSpaceForPixelFormat(color_space, sysmem_format));

    EXPECT_EQ(ZX_PIXEL_FORMAT_BYTES(format), ImageFormatStrideBytesPerWidthPixel(sysmem_format));
    EXPECT_TRUE(ImageFormatIsSupported(sysmem_format));
    EXPECT_LT(0u, ImageFormatBitsPerPixel(sysmem_format));
  }

  sysmem_v2::wire::PixelFormat other_format(allocator);
  other_format.set_type(sysmem_v2::wire::PixelFormatType::kBgra32);
  other_format.set_format_modifier_value(allocator,
                                         sysmem_v2::wire::kFormatModifierIntelI915XTiled);

  zx_pixel_format_t back_format;
  EXPECT_FALSE(ImageFormatConvertSysmemToZx(other_format, &back_format));
  // Treat as linear.
  auto other_format2 = sysmem::V2ClonePixelFormat(allocator, other_format);
  other_format2.set_format_modifier_value(nullptr);
  EXPECT_TRUE(ImageFormatConvertSysmemToZx(other_format2, &back_format));
}

TEST(ImageFormat, ZxPixelFormat_V1_LLCPP) {
  fidl::Arena allocator;
  zx_pixel_format_t pixel_formats[] = {
      ZX_PIXEL_FORMAT_RGB_565,   ZX_PIXEL_FORMAT_RGB_332,  ZX_PIXEL_FORMAT_RGB_2220,
      ZX_PIXEL_FORMAT_ARGB_8888, ZX_PIXEL_FORMAT_RGB_x888, ZX_PIXEL_FORMAT_MONO_8,
      ZX_PIXEL_FORMAT_GRAY_8,    ZX_PIXEL_FORMAT_NV12,     ZX_PIXEL_FORMAT_RGB_888,
      ZX_PIXEL_FORMAT_ABGR_8888, ZX_PIXEL_FORMAT_BGR_888x,
  };
  for (zx_pixel_format_t format : pixel_formats) {
    printf("Format %x\n", format);
    auto sysmem_format_result = ImageFormatConvertZxToSysmem_v1(allocator, format);
    EXPECT_TRUE(sysmem_format_result.is_ok());
    auto sysmem_format = sysmem_format_result.take_value();
    zx_pixel_format_t back_format;
    EXPECT_TRUE(ImageFormatConvertSysmemToZx(sysmem_format, &back_format));
    if (format == ZX_PIXEL_FORMAT_RGB_x888) {
      EXPECT_EQ(ZX_PIXEL_FORMAT_ARGB_8888, back_format);
    } else if (format == ZX_PIXEL_FORMAT_BGR_888x) {
      EXPECT_EQ(ZX_PIXEL_FORMAT_ABGR_8888, back_format);
    } else {
      EXPECT_EQ(back_format, format);
    }
    EXPECT_TRUE(sysmem_format.has_format_modifier);
    EXPECT_EQ(fuchsia_sysmem_FORMAT_MODIFIER_LINEAR,
              static_cast<uint64_t>(sysmem_format.format_modifier.value));

    sysmem_v1::wire::ColorSpace color_space;
    if (format == ZX_PIXEL_FORMAT_NV12) {
      color_space.type = sysmem_v1::wire::ColorSpaceType::kRec601Ntsc;
    } else {
      color_space.type = sysmem_v1::wire::ColorSpaceType::kSrgb;
    }
    EXPECT_TRUE(ImageFormatIsSupportedColorSpaceForPixelFormat(color_space, sysmem_format));

    EXPECT_EQ(ZX_PIXEL_FORMAT_BYTES(format), ImageFormatStrideBytesPerWidthPixel(sysmem_format));
    EXPECT_TRUE(ImageFormatIsSupported(sysmem_format));
    EXPECT_LT(0u, ImageFormatBitsPerPixel(sysmem_format));
  }

  sysmem_v1::wire::PixelFormat other_format = {
      .type = sysmem_v1::wire::PixelFormatType::kBgra32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = sysmem_v1::wire::kFormatModifierIntelI915XTiled,
          },
  };

  zx_pixel_format_t back_format;
  EXPECT_FALSE(ImageFormatConvertSysmemToZx(other_format, &back_format));
  // Treat as linear.
  other_format.has_format_modifier = false;
  EXPECT_TRUE(ImageFormatConvertSysmemToZx(other_format, &back_format));
}

TEST(ImageFormat, ZxPixelFormat_V1_C) {
  zx_pixel_format_t pixel_formats[] = {
      ZX_PIXEL_FORMAT_RGB_565,   ZX_PIXEL_FORMAT_RGB_332,  ZX_PIXEL_FORMAT_RGB_2220,
      ZX_PIXEL_FORMAT_ARGB_8888, ZX_PIXEL_FORMAT_RGB_x888, ZX_PIXEL_FORMAT_MONO_8,
      ZX_PIXEL_FORMAT_GRAY_8,    ZX_PIXEL_FORMAT_NV12,     ZX_PIXEL_FORMAT_RGB_888,
      ZX_PIXEL_FORMAT_ABGR_8888, ZX_PIXEL_FORMAT_BGR_888x,
  };
  for (zx_pixel_format_t format : pixel_formats) {
    printf("Format %x\n", format);
    fuchsia_sysmem_PixelFormat sysmem_format;
    EXPECT_TRUE(ImageFormatConvertZxToSysmem(format, &sysmem_format));
    zx_pixel_format_t back_format;
    EXPECT_TRUE(ImageFormatConvertSysmemToZx(&sysmem_format, &back_format));
    if (format == ZX_PIXEL_FORMAT_RGB_x888) {
      EXPECT_EQ(ZX_PIXEL_FORMAT_ARGB_8888, back_format);
    } else if (format == ZX_PIXEL_FORMAT_BGR_888x) {
      EXPECT_EQ(ZX_PIXEL_FORMAT_ABGR_8888, back_format);
    } else {
      EXPECT_EQ(back_format, format);
    }
    EXPECT_TRUE(sysmem_format.has_format_modifier);
    EXPECT_EQ(fuchsia_sysmem_FORMAT_MODIFIER_LINEAR,
              static_cast<uint64_t>(sysmem_format.format_modifier.value));

    fuchsia_sysmem_ColorSpace color_space;
    if (format == ZX_PIXEL_FORMAT_NV12) {
      color_space.type = fuchsia_sysmem_ColorSpaceType_REC601_NTSC;
    } else {
      color_space.type = fuchsia_sysmem_ColorSpaceType_SRGB;
    }
    EXPECT_TRUE(ImageFormatIsSupportedColorSpaceForPixelFormat(color_space, sysmem_format));

    EXPECT_EQ(ZX_PIXEL_FORMAT_BYTES(format), ImageFormatStrideBytesPerWidthPixel(&sysmem_format));
    EXPECT_TRUE(ImageFormatIsSupported(&sysmem_format));
    EXPECT_LT(0u, ImageFormatBitsPerPixel(&sysmem_format));
  }

  fuchsia_sysmem_PixelFormat other_format = {
      .type = fuchsia_sysmem_PixelFormatType_BGRA32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = fuchsia_sysmem_FORMAT_MODIFIER_INTEL_I915_X_TILED,
          },
  };

  zx_pixel_format_t back_format;
  EXPECT_FALSE(ImageFormatConvertSysmemToZx(&other_format, &back_format));
  // Treat as linear.
  other_format.has_format_modifier = false;
  EXPECT_TRUE(ImageFormatConvertSysmemToZx(&other_format, &back_format));
}

TEST(ImageFormat, PlaneByteOffset_V2_LLCPP) {
  fidl::Arena allocator;
  sysmem_v2::wire::PixelFormat linear(allocator);
  linear.set_type(sysmem_v2::wire::PixelFormatType::kBgra32);
  linear.set_format_modifier_value(allocator, sysmem_v2::wire::kFormatModifierLinear);
  sysmem_v2::wire::ImageFormatConstraints constraints(allocator);
  constraints.set_pixel_format(allocator, std::move(linear));
  constraints.set_min_coded_width(12u);
  constraints.set_max_coded_width(100u);
  constraints.set_min_coded_height(12u);
  constraints.set_max_coded_height(100u);
  constraints.set_bytes_per_row_divisor(4u * 8u);
  constraints.set_max_bytes_per_row(100000u);

  auto image_format_result = ImageConstraintsToFormat(allocator, constraints, 18, 17);
  EXPECT_TRUE(image_format_result.is_ok());
  auto image_format = image_format_result.take_value();
  // The raw size would be 72 without bytes_per_row_divisor of 32.
  EXPECT_EQ(96u, image_format.bytes_per_row());

  uint64_t byte_offset;
  EXPECT_TRUE(ImageFormatPlaneByteOffset(image_format, 0, &byte_offset));
  EXPECT_EQ(0u, byte_offset);
  EXPECT_FALSE(ImageFormatPlaneByteOffset(image_format, 1, &byte_offset));

  auto constraints2 = sysmem::V2CloneImageFormatConstraints(allocator, constraints);
  constraints2.pixel_format().set_type(sysmem_v2::wire::PixelFormatType::kI420);

  constexpr uint32_t kBytesPerRow = 32;
  image_format_result = ImageConstraintsToFormat(allocator, constraints2, 18, 20);
  EXPECT_TRUE(image_format_result.is_ok());
  image_format = image_format_result.take_value();
  EXPECT_EQ(kBytesPerRow, image_format.bytes_per_row());
  EXPECT_TRUE(ImageFormatPlaneByteOffset(image_format, 0, &byte_offset));
  EXPECT_EQ(0u, byte_offset);
  EXPECT_TRUE(ImageFormatPlaneByteOffset(image_format, 1, &byte_offset));
  EXPECT_EQ(kBytesPerRow * 20, byte_offset);
  EXPECT_TRUE(ImageFormatPlaneByteOffset(image_format, 2, &byte_offset));
  EXPECT_EQ(kBytesPerRow * 20 + kBytesPerRow / 2 * 20 / 2, byte_offset);
  EXPECT_FALSE(ImageFormatPlaneByteOffset(image_format, 3, &byte_offset));

  uint32_t row_bytes;
  EXPECT_TRUE(ImageFormatPlaneRowBytes(image_format, 0, &row_bytes));
  EXPECT_EQ(kBytesPerRow, row_bytes);
  EXPECT_TRUE(ImageFormatPlaneRowBytes(image_format, 1, &row_bytes));
  EXPECT_EQ(kBytesPerRow / 2, row_bytes);
  EXPECT_TRUE(ImageFormatPlaneRowBytes(image_format, 2, &row_bytes));
  EXPECT_EQ(kBytesPerRow / 2, row_bytes);
  EXPECT_FALSE(ImageFormatPlaneRowBytes(image_format, 3, &row_bytes));
}

TEST(ImageFormat, PlaneByteOffset_V1_LLCPP) {
  sysmem_v1::wire::PixelFormat linear = {
      .type = sysmem_v1::wire::PixelFormatType::kBgra32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = sysmem_v1::wire::kFormatModifierLinear,
          },
  };
  sysmem_v1::wire::ImageFormatConstraints constraints = {
      .pixel_format = linear,
      .min_coded_width = 12,
      .max_coded_width = 100,
      .min_coded_height = 12,
      .max_coded_height = 100,
      .max_bytes_per_row = 100000,
      .bytes_per_row_divisor = 4 * 8,
  };

  auto image_format_result = ImageConstraintsToFormat(constraints, 18, 17);
  EXPECT_TRUE(image_format_result.is_ok());
  auto image_format = image_format_result.take_value();
  // The raw size would be 72 without bytes_per_row_divisor of 32.
  EXPECT_EQ(96u, image_format.bytes_per_row);

  uint64_t byte_offset;
  EXPECT_TRUE(ImageFormatPlaneByteOffset(image_format, 0, &byte_offset));
  EXPECT_EQ(0u, byte_offset);
  EXPECT_FALSE(ImageFormatPlaneByteOffset(image_format, 1, &byte_offset));

  constraints.pixel_format.type = sysmem_v1::wire::PixelFormatType::kI420;

  constexpr uint32_t kBytesPerRow = 32;
  image_format_result = ImageConstraintsToFormat(constraints, 18, 20);
  EXPECT_TRUE(image_format_result.is_ok());
  image_format = image_format_result.take_value();
  EXPECT_EQ(kBytesPerRow, image_format.bytes_per_row);
  EXPECT_TRUE(ImageFormatPlaneByteOffset(image_format, 0, &byte_offset));
  EXPECT_EQ(0u, byte_offset);
  EXPECT_TRUE(ImageFormatPlaneByteOffset(image_format, 1, &byte_offset));
  EXPECT_EQ(kBytesPerRow * 20, byte_offset);
  EXPECT_TRUE(ImageFormatPlaneByteOffset(image_format, 2, &byte_offset));
  EXPECT_EQ(kBytesPerRow * 20 + kBytesPerRow / 2 * 20 / 2, byte_offset);
  EXPECT_FALSE(ImageFormatPlaneByteOffset(image_format, 3, &byte_offset));

  uint32_t row_bytes;
  EXPECT_TRUE(ImageFormatPlaneRowBytes(image_format, 0, &row_bytes));
  EXPECT_EQ(kBytesPerRow, row_bytes);
  EXPECT_TRUE(ImageFormatPlaneRowBytes(image_format, 1, &row_bytes));
  EXPECT_EQ(kBytesPerRow / 2, row_bytes);
  EXPECT_TRUE(ImageFormatPlaneRowBytes(image_format, 2, &row_bytes));
  EXPECT_EQ(kBytesPerRow / 2, row_bytes);
  EXPECT_FALSE(ImageFormatPlaneRowBytes(image_format, 3, &row_bytes));
}

TEST(ImageFormat, PlaneByteOffset_V1_C) {
  fuchsia_sysmem_PixelFormat linear = {
      .type = fuchsia_sysmem_PixelFormatType_BGRA32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = fuchsia_sysmem_FORMAT_MODIFIER_LINEAR,
          },
  };
  fuchsia_sysmem_ImageFormatConstraints constraints = {
      .pixel_format = linear,
      .min_coded_width = 12,
      .max_coded_width = 100,
      .min_coded_height = 12,
      .max_coded_height = 100,
      .max_bytes_per_row = 100000,
      .bytes_per_row_divisor = 4 * 8,
  };

  fuchsia_sysmem_ImageFormat_2 image_format;
  EXPECT_TRUE(ImageConstraintsToFormat(&constraints, 18, 17, &image_format));
  // The raw size would be 72 without bytes_per_row_divisor of 32.
  EXPECT_EQ(96u, image_format.bytes_per_row);

  uint64_t byte_offset;
  EXPECT_TRUE(ImageFormatPlaneByteOffset(&image_format, 0, &byte_offset));
  EXPECT_EQ(0u, byte_offset);
  EXPECT_FALSE(ImageFormatPlaneByteOffset(&image_format, 1, &byte_offset));

  constraints.pixel_format.type = fuchsia_sysmem_PixelFormatType_I420;

  constexpr uint32_t kBytesPerRow = 32;
  EXPECT_TRUE(ImageConstraintsToFormat(&constraints, 18, 20, &image_format));
  EXPECT_EQ(kBytesPerRow, image_format.bytes_per_row);
  EXPECT_TRUE(ImageFormatPlaneByteOffset(&image_format, 0, &byte_offset));
  EXPECT_EQ(0u, byte_offset);
  EXPECT_TRUE(ImageFormatPlaneByteOffset(&image_format, 1, &byte_offset));
  EXPECT_EQ(kBytesPerRow * 20, byte_offset);
  EXPECT_TRUE(ImageFormatPlaneByteOffset(&image_format, 2, &byte_offset));
  EXPECT_EQ(kBytesPerRow * 20 + kBytesPerRow / 2 * 20 / 2, byte_offset);
  EXPECT_FALSE(ImageFormatPlaneByteOffset(&image_format, 3, &byte_offset));

  uint32_t row_bytes;
  EXPECT_TRUE(ImageFormatPlaneRowBytes(&image_format, 0, &row_bytes));
  EXPECT_EQ(kBytesPerRow, row_bytes);
  EXPECT_TRUE(ImageFormatPlaneRowBytes(&image_format, 1, &row_bytes));
  EXPECT_EQ(kBytesPerRow / 2, row_bytes);
  EXPECT_TRUE(ImageFormatPlaneRowBytes(&image_format, 2, &row_bytes));
  EXPECT_EQ(kBytesPerRow / 2, row_bytes);
  EXPECT_FALSE(ImageFormatPlaneRowBytes(&image_format, 3, &row_bytes));
}

TEST(ImageFormat, TransactionEliminationFormats_V2_LLCPP) {
  fidl::Arena allocator;
  sysmem_v2::wire::PixelFormat format(allocator);
  format.set_type(sysmem_v2::wire::PixelFormatType::kBgra32);
  format.set_format_modifier_value(allocator, sysmem_v2::wire::kFormatModifierLinear);

  EXPECT_TRUE(ImageFormatCompatibleWithProtectedMemory(format));

  auto format2 = sysmem::V2ClonePixelFormat(allocator, format);
  format2.set_format_modifier_value(allocator, sysmem_v2::wire::kFormatModifierArmLinearTe);

  EXPECT_FALSE(ImageFormatCompatibleWithProtectedMemory(format2));

  sysmem_v2::wire::ImageFormatConstraints constraints(allocator);
  constraints.set_pixel_format(allocator, std::move(format2));
  constraints.set_min_coded_width(12u);
  constraints.set_max_coded_width(100u);
  constraints.set_min_coded_height(12u);
  constraints.set_max_coded_height(100u);
  constraints.set_bytes_per_row_divisor(4u * 8u);
  constraints.set_max_bytes_per_row(100000u);

  auto image_format_result = ImageConstraintsToFormat(allocator, constraints, 18, 17);
  EXPECT_TRUE(image_format_result.is_ok());
  auto image_format = image_format_result.take_value();
  // The raw size would be 72 without bytes_per_row_divisor of 32.
  EXPECT_EQ(96u, image_format.bytes_per_row());

  // Check the color plane data.
  uint32_t row_bytes;
  uint64_t plane_offset;
  EXPECT_TRUE(ImageFormatPlaneByteOffset(image_format, 0, &plane_offset));
  EXPECT_EQ(0u, plane_offset);
  EXPECT_TRUE(ImageFormatPlaneRowBytes(image_format, 0, &row_bytes));
  EXPECT_EQ(image_format.bytes_per_row(), row_bytes);

  constexpr uint32_t kTePlane = 3;
  // Check the TE plane data.
  EXPECT_TRUE(ImageFormatPlaneByteOffset(image_format, kTePlane, &plane_offset));
  EXPECT_LE(image_format.bytes_per_row() * 17, plane_offset);
  EXPECT_TRUE(ImageFormatPlaneRowBytes(image_format, kTePlane, &row_bytes));

  // Row size should be rounded up to 64 bytes.
  EXPECT_EQ(64, row_bytes);
}

TEST(ImageFormat, TransactionEliminationFormats_V1_LLCPP) {
  sysmem_v1::wire::PixelFormat format = {
      .type = sysmem_v1::wire::PixelFormatType::kBgra32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = sysmem_v1::wire::kFormatModifierLinear,
          },
  };
  EXPECT_TRUE(image_format::FormatCompatibleWithProtectedMemory(format));
  EXPECT_TRUE(ImageFormatCompatibleWithProtectedMemory(format));

  format.format_modifier.value = sysmem_v1::wire::kFormatModifierArmLinearTe;
  EXPECT_FALSE(image_format::FormatCompatibleWithProtectedMemory(format));
  EXPECT_FALSE(ImageFormatCompatibleWithProtectedMemory(format));

  sysmem_v1::wire::ImageFormatConstraints constraints = {
      .pixel_format = format,
      .min_coded_width = 12,
      .max_coded_width = 100,
      .min_coded_height = 12,
      .max_coded_height = 100,
      .max_bytes_per_row = 100000,
      .bytes_per_row_divisor = 4 * 8,
  };

  auto optional_format = image_format::ConstraintsToFormat(constraints, 18, 17);
  EXPECT_TRUE(optional_format);
  auto& image_format = *optional_format;
  // The raw size would be 72 without bytes_per_row_divisor of 32.
  EXPECT_EQ(96u, image_format.bytes_per_row);

  // Check the color plane data.
  uint32_t row_bytes;
  uint64_t plane_offset;
  EXPECT_TRUE(image_format::GetPlaneByteOffset(image_format, 0, &plane_offset));
  EXPECT_EQ(0u, plane_offset);
  EXPECT_TRUE(image_format::GetPlaneRowBytes(image_format, 0, &row_bytes));
  EXPECT_EQ(image_format.bytes_per_row, row_bytes);

  constexpr uint32_t kTePlane = 3;
  // Check the TE plane data.
  EXPECT_TRUE(image_format::GetPlaneByteOffset(image_format, kTePlane, &plane_offset));
  EXPECT_LE(image_format.bytes_per_row * 17, plane_offset);
  EXPECT_TRUE(image_format::GetPlaneRowBytes(image_format, kTePlane, &row_bytes));

  // Row size should be rounded up to 64 bytes.
  EXPECT_EQ(64, row_bytes);
}

TEST(ImageFormat, BasicSizes_V2_LLCPP) {
  fidl::Arena allocator;
  constexpr uint32_t kWidth = 64;
  constexpr uint32_t kHeight = 128;
  constexpr uint32_t kStride = kWidth * 6;

  sysmem_v2::wire::ImageFormat image_format_bgra32(allocator);
  {
    sysmem_v2::wire::PixelFormat pixel_format(allocator);
    pixel_format.set_type(sysmem_v2::wire::PixelFormatType::kBgra32);
    image_format_bgra32.set_pixel_format(allocator, std::move(pixel_format));
  }
  image_format_bgra32.set_coded_width(kWidth);
  image_format_bgra32.set_coded_height(kHeight);
  image_format_bgra32.set_bytes_per_row(kStride);
  EXPECT_EQ(kHeight * kStride, ImageFormatImageSize(image_format_bgra32));
  EXPECT_EQ(1, ImageFormatCodedWidthMinDivisor(image_format_bgra32.pixel_format()));
  EXPECT_EQ(1, ImageFormatCodedHeightMinDivisor(image_format_bgra32.pixel_format()));
  EXPECT_EQ(4, ImageFormatSampleAlignment(image_format_bgra32.pixel_format()));

  sysmem_v2::wire::ImageFormat image_format_nv12(allocator);
  {
    sysmem_v2::wire::PixelFormat pixel_format(allocator);
    pixel_format.set_type(sysmem_v2::wire::PixelFormatType::kNv12);
    image_format_nv12.set_pixel_format(allocator, std::move(pixel_format));
  }
  image_format_nv12.set_coded_width(kWidth);
  image_format_nv12.set_coded_height(kHeight);
  image_format_nv12.set_bytes_per_row(kStride);
  EXPECT_EQ(kHeight * kStride * 3 / 2, ImageFormatImageSize(image_format_nv12));
  EXPECT_EQ(2, ImageFormatCodedWidthMinDivisor(image_format_nv12.pixel_format()));
  EXPECT_EQ(2, ImageFormatCodedHeightMinDivisor(image_format_nv12.pixel_format()));
  EXPECT_EQ(2, ImageFormatSampleAlignment(image_format_nv12.pixel_format()));
}

TEST(ImageFormat, BasicSizes_V1_LLCPP) {
  constexpr uint32_t kWidth = 64;
  constexpr uint32_t kHeight = 128;
  constexpr uint32_t kStride = 256;

  sysmem_v1::wire::ImageFormat2 image_format_bgra32 = {
      .pixel_format =
          {
              .type = sysmem_v1::wire::PixelFormatType::kBgra32,
          },
      .coded_width = kWidth,
      .coded_height = kHeight,
      .bytes_per_row = kStride,
  };
  EXPECT_EQ(kHeight * kStride, ImageFormatImageSize(image_format_bgra32));
  EXPECT_EQ(1, ImageFormatCodedWidthMinDivisor(image_format_bgra32.pixel_format));
  EXPECT_EQ(1, ImageFormatCodedHeightMinDivisor(image_format_bgra32.pixel_format));
  EXPECT_EQ(4, ImageFormatSampleAlignment(image_format_bgra32.pixel_format));

  sysmem_v1::wire::ImageFormat2 image_format_nv12 = {
      .pixel_format =
          {
              .type = sysmem_v1::wire::PixelFormatType::kNv12,
          },
      .coded_width = kWidth,
      .coded_height = kHeight,
      .bytes_per_row = kStride,
  };
  EXPECT_EQ(kHeight * kStride * 3 / 2, ImageFormatImageSize(image_format_nv12));
  EXPECT_EQ(2, ImageFormatCodedWidthMinDivisor(image_format_nv12.pixel_format));
  EXPECT_EQ(2, ImageFormatCodedHeightMinDivisor(image_format_nv12.pixel_format));
  EXPECT_EQ(2, ImageFormatSampleAlignment(image_format_nv12.pixel_format));
}

TEST(ImageFormat, BasicSizes_V1_C) {
  constexpr uint32_t kWidth = 64;
  constexpr uint32_t kHeight = 128;
  constexpr uint32_t kStride = 256;

  fuchsia_sysmem_ImageFormat_2 image_format_bgra32 = {
      .pixel_format =
          {
              .type = fuchsia_sysmem_PixelFormatType_BGRA32,
          },
      .coded_width = kWidth,
      .coded_height = kHeight,
      .bytes_per_row = kStride,
  };
  EXPECT_EQ(kHeight * kStride, ImageFormatImageSize(&image_format_bgra32));
  EXPECT_EQ(1, ImageFormatCodedWidthMinDivisor(&image_format_bgra32.pixel_format));
  EXPECT_EQ(1, ImageFormatCodedHeightMinDivisor(&image_format_bgra32.pixel_format));
  EXPECT_EQ(4, ImageFormatSampleAlignment(&image_format_bgra32.pixel_format));

  fuchsia_sysmem_ImageFormat_2 image_format_nv12{
      .pixel_format =
          {
              .type = fuchsia_sysmem_PixelFormatType_NV12,
          },
      .coded_width = kWidth,
      .coded_height = kHeight,
      .bytes_per_row = kStride,
  };
  EXPECT_EQ(kHeight * kStride * 3 / 2, ImageFormatImageSize(&image_format_nv12));
  EXPECT_EQ(2, ImageFormatCodedWidthMinDivisor(&image_format_nv12.pixel_format));
  EXPECT_EQ(2, ImageFormatCodedHeightMinDivisor(&image_format_nv12.pixel_format));
  EXPECT_EQ(2, ImageFormatSampleAlignment(&image_format_nv12.pixel_format));
}

TEST(ImageFormat, AfbcFlagFormats_V1_LLCPP) {
  sysmem_v1::wire::PixelFormat format = {
      .type = sysmem_v1::wire::PixelFormatType::kBgra32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = sysmem_v1::wire::kFormatModifierArmAfbc16X16SplitBlockSparseYuvTe,
          },
  };

  EXPECT_FALSE(image_format::FormatCompatibleWithProtectedMemory(format));

  sysmem_v1::wire::ImageFormatConstraints constraints = {
      .pixel_format = format,
      .min_coded_width = 12,
      .max_coded_width = 100,
      .min_coded_height = 12,
      .max_coded_height = 100,
      .max_bytes_per_row = 100000,
      .bytes_per_row_divisor = 4 * 8,
  };

  auto optional_format = image_format::ConstraintsToFormat(constraints, 18, 17);
  EXPECT_TRUE(optional_format);

  sysmem_v1::wire::PixelFormat tiled_format = {
      .type = sysmem_v1::wire::PixelFormatType::kBgra32,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = sysmem_v1::wire::kFormatModifierArmAfbc16X16SplitBlockSparseYuvTiledHeader,
          },
  };

  constraints.pixel_format = tiled_format;

  optional_format = image_format::ConstraintsToFormat(constraints, 18, 17);
  EXPECT_TRUE(optional_format);
  auto& image_format = *optional_format;
  constexpr uint32_t kMinHeaderOffset = 4096u;
  constexpr uint32_t kMinWidth = 128;
  constexpr uint32_t kMinHeight = 128;
  EXPECT_EQ(kMinHeaderOffset + kMinWidth * kMinHeight * 4, ImageFormatImageSize(image_format));
}

TEST(ImageFormat, R8G8Formats_V1_LLCPP) {
  sysmem_v1::wire::PixelFormat format = {
      .type = sysmem_v1::wire::PixelFormatType::kR8G8,
      .has_format_modifier = true,
      .format_modifier =
          {
              .value = sysmem_v1::wire::kFormatModifierLinear,
          },
  };

  sysmem_v1::wire::ImageFormatConstraints constraints = {
      .pixel_format = format,
      .min_coded_width = 12,
      .max_coded_width = 100,
      .min_coded_height = 12,
      .max_coded_height = 100,
      .max_bytes_per_row = 100000,
      .bytes_per_row_divisor = 1,
  };

  auto optional_format = image_format::ConstraintsToFormat(constraints, 18, 17);
  EXPECT_TRUE(optional_format);
  EXPECT_EQ(18u * 2, optional_format->bytes_per_row);
  EXPECT_EQ(18u * 17u * 2, ImageFormatImageSize(*optional_format));

  constraints.pixel_format.type = sysmem_v1::wire::PixelFormatType::kR8;

  optional_format = image_format::ConstraintsToFormat(constraints, 18, 17);
  EXPECT_TRUE(optional_format);
  EXPECT_EQ(18u * 1, optional_format->bytes_per_row);
  EXPECT_EQ(18u * 17u * 1, ImageFormatImageSize(*optional_format));
}

TEST(ImageFormat, A2R10G10B10_Formats_V1_LLCPP) {
  for (const auto& pixel_format_type : {sysmem_v1::wire::PixelFormatType::kA2R10G10B10,
                                        sysmem_v1::wire::PixelFormatType::kA2B10G10R10}) {
    sysmem_v1::wire::PixelFormat format = {
        .type = pixel_format_type,
        .has_format_modifier = true,
        .format_modifier =
            {
                .value = sysmem_v1::wire::kFormatModifierLinear,
            },
    };

    sysmem_v1::wire::ImageFormatConstraints constraints = {
        .pixel_format = format,
        .min_coded_width = 12,
        .max_coded_width = 100,
        .min_coded_height = 12,
        .max_coded_height = 100,
        .max_bytes_per_row = 100000,
        .bytes_per_row_divisor = 1,
    };

    auto optional_format = image_format::ConstraintsToFormat(constraints, 18, 17);
    EXPECT_TRUE(optional_format);
    EXPECT_EQ(18u * 4, optional_format->bytes_per_row);
    EXPECT_EQ(18u * 17u * 4, ImageFormatImageSize(*optional_format));
    EXPECT_EQ(1, ImageFormatCodedWidthMinDivisor(optional_format->pixel_format));
    EXPECT_EQ(1, ImageFormatCodedHeightMinDivisor(optional_format->pixel_format));
    EXPECT_EQ(4, ImageFormatSampleAlignment(optional_format->pixel_format));
  }
}

TEST(ImageFormat, GoldfishOptimal_V2_LLCPP) {
  fidl::Arena allocator;
  constexpr uint32_t kWidth = 64;
  constexpr uint32_t kHeight = 128;
  constexpr uint32_t kStride = kWidth * 6;

  sysmem_v2::wire::ImageFormat linear_image_format_bgra32(allocator);
  {
    sysmem_v2::wire::PixelFormat pixel_format(allocator);
    pixel_format.set_type(sysmem_v2::wire::PixelFormatType::kBgra32);
    linear_image_format_bgra32.set_pixel_format(allocator, std::move(pixel_format));
  }
  linear_image_format_bgra32.set_coded_width(kWidth);
  linear_image_format_bgra32.set_coded_height(kHeight);
  linear_image_format_bgra32.set_bytes_per_row(kStride);

  sysmem_v2::wire::ImageFormat goldfish_optimal_image_format_bgra32(allocator);
  {
    sysmem_v2::wire::PixelFormat pixel_format(allocator);
    pixel_format.set_type(sysmem_v2::wire::PixelFormatType::kBgra32);
    pixel_format.set_format_modifier_value(allocator,
                                           sysmem_v2::wire::kFormatModifierGoogleGoldfishOptimal);
    goldfish_optimal_image_format_bgra32.set_pixel_format(allocator, std::move(pixel_format));
  }
  goldfish_optimal_image_format_bgra32.set_coded_width(kWidth);
  goldfish_optimal_image_format_bgra32.set_coded_height(kHeight);
  goldfish_optimal_image_format_bgra32.set_bytes_per_row(kStride);
  EXPECT_EQ(ImageFormatImageSize(linear_image_format_bgra32),
            ImageFormatImageSize(goldfish_optimal_image_format_bgra32));
  EXPECT_EQ(ImageFormatCodedWidthMinDivisor(linear_image_format_bgra32.pixel_format()),
            ImageFormatCodedWidthMinDivisor(goldfish_optimal_image_format_bgra32.pixel_format()));
  EXPECT_EQ(ImageFormatCodedHeightMinDivisor(linear_image_format_bgra32.pixel_format()),
            ImageFormatCodedHeightMinDivisor(goldfish_optimal_image_format_bgra32.pixel_format()));
  EXPECT_EQ(ImageFormatSampleAlignment(linear_image_format_bgra32.pixel_format()),
            ImageFormatSampleAlignment(goldfish_optimal_image_format_bgra32.pixel_format()));
}

TEST(ImageFormat, CorrectModifiers) {
  EXPECT_EQ(sysmem_v1::wire::kFormatModifierArmAfbc16X16YuvTiledHeader,
            sysmem_v1::wire::kFormatModifierArmAfbc16X16YuvTiledHeader);
  EXPECT_EQ(sysmem_v1::wire::kFormatModifierArmAfbc16X16YuvTiledHeader,
            sysmem_v1::wire::kFormatModifierArmAfbc16X16 |
                sysmem_v1::wire::kFormatModifierArmYuvBit |
                sysmem_v1::wire::kFormatModifierArmTiledHeaderBit);
  EXPECT_EQ(sysmem_v1::wire::kFormatModifierGoogleGoldfishOptimal,
            sysmem_v2::wire::kFormatModifierGoogleGoldfishOptimal);
}

TEST(ImageFormat, IntelCcsFormats_V1_LLCPP) {
  for (auto& format_modifier : {
           sysmem_v1::wire::kFormatModifierIntelI915YTiledCcs,
           sysmem_v1::wire::kFormatModifierIntelI915YfTiledCcs,
       }) {
    sysmem_v1::wire::PixelFormat format = {
        .type = sysmem_v1::wire::PixelFormatType::kBgra32,
        .has_format_modifier = true,
        .format_modifier =
            {
                .value = format_modifier,
            },
    };

    sysmem_v1::wire::ImageFormatConstraints constraints = {
        .pixel_format = format,
        .min_coded_width = 12,
        .max_coded_width = 100,
        .min_coded_height = 12,
        .max_coded_height = 100,
        .max_bytes_per_row = 100000,
        .bytes_per_row_divisor = 4 * 8,
    };

    auto optional_format = image_format::ConstraintsToFormat(constraints, 64, 63);
    EXPECT_TRUE(optional_format);

    auto& image_format = *optional_format;
    constexpr uint32_t kWidthInTiles = 2;
    constexpr uint32_t kHeightInTiles = 2;
    constexpr uint32_t kTileSize = 4096;
    constexpr uint32_t kMainPlaneSize = kWidthInTiles * kHeightInTiles * kTileSize;
    constexpr uint32_t kCcsWidthInTiles = 1;
    constexpr uint32_t kCcsHeightInTiles = 1;
    constexpr uint32_t kCcsPlane = 3;
    EXPECT_EQ(kMainPlaneSize + kCcsWidthInTiles * kCcsHeightInTiles * kTileSize,
              ImageFormatImageSize(image_format));
    uint64_t ccs_byte_offset;
    EXPECT_TRUE(ImageFormatPlaneByteOffset(image_format, kCcsPlane, &ccs_byte_offset));
    EXPECT_EQ(kMainPlaneSize, ccs_byte_offset);

    uint32_t main_plane_row_stride;
    EXPECT_TRUE(ImageFormatPlaneRowBytes(image_format, 0, &main_plane_row_stride));
    EXPECT_EQ(128u * kWidthInTiles, main_plane_row_stride);
    uint32_t ccs_row_stride;
    EXPECT_TRUE(ImageFormatPlaneRowBytes(image_format, kCcsPlane, &ccs_row_stride));
    EXPECT_EQ(ccs_row_stride, 128u * kCcsWidthInTiles);
  }
}
