// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//
// COLOR UTILITIES
//

#ifndef SRC_GRAPHICS_LIB_COMPUTE_SPINEL_EXT_COLOR_COLOR_H_
#define SRC_GRAPHICS_LIB_COMPUTE_SPINEL_EXT_COLOR_COLOR_H_

//
//
//

#include <stdint.h>

//
//
//

#ifdef __cplusplus
extern "C" {
#endif

//
// CONVERT FROM 0xAARRGGBB WORD ORDER INTO f32[4]
//

void
color_rgb32_to_rgba_f32(float rgba[4], const uint32_t rgb, const float opacity);

void
color_argb32_to_rgba_f32(float rgba[4], const uint32_t argb);

//
// PREMULTIPLY
//
// { r, g, b, a } ==> { r*a, g*a, b*a, a }
//

void
color_premultiply_rgba_f32(float rgba[4]);

//
// SRGB<>LINEAR
//
// EXT_framebuffer_sRGB:
// https://www.opengl.org/registry/specs/EXT/framebuffer_sRGB.txt
//
//        {  cs / 12.92,                 cs <= 0.04045
//   cl = {
//        {  ((cs + 0.055)/1.055)^2.4,   cs >  0.04045
//
//
//        {  0.0,                          0         <= cl
//        {  12.92 * cl,                   0         <  cl <  0.0031308
//   cs = {  1.055 * cl^0.41666 - 0.055,   0.0031308 <= cl <  1
//        {  1.0,                                       cl >= 1
//

void
color_srgb_to_linear_rgb_f32(float rgb[3]);

void
color_linear_to_srgb_rgb_f32(float rgb[3]);

//
//
//

void
color_linear_lerp_rgba_f32(float       rgba_c[4],
                           float const rgba_a[4],
                           float const rgba_b[4],
                           float const t);

//
//
//

#ifdef __cplusplus
}
#endif

//
//
//

#endif  // SRC_GRAPHICS_LIB_COMPUTE_SPINEL_EXT_COLOR_COLOR_H_
