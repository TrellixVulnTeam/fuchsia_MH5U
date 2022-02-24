// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <math.h>
#include <stdio.h>

#include <vector>

#if USE_MOLD
#define DEMO_CLASS_HEADER "common/demo_app_mold.h"
#define DEMO_CLASS_NAME DemoAppMold
#define ONLY_IF_MOLD(...) __VA_ARGS__
#define ONLY_IF_SPINEL(...) /* nothing */
#else                       // !USE_MOLD
#define DEMO_CLASS_HEADER "common/demo_app_spinel.h"
#define DEMO_CLASS_NAME DemoAppSpinel
#define ONLY_IF_MOLD(...) /* nothing */
#define ONLY_IF_SPINEL(...) __VA_ARGS__
#endif  // !USE_MOLD

// This weird include is to work-around the fact that gn --check doesn't
// seem to properly understand conditional includes when they appear in
// #ifdef .. #endif blocks. Note that this is standard cpp behaviour!
#include DEMO_CLASS_HEADER

#include "common/demo_image.h"
#include "common/demo_image_group.h"
#include "common/demo_utils.h"
#include "tests/common/affine_transform.h"
#include "tests/common/argparse.h"
#include "tests/common/spinel/svg_spinel_image.h"
#include "tests/common/svg/scoped_svg.h"
#include "tests/common/svg/svg_bounds.h"
#include "tests/common/utils.h"

#define DEMO_SURFACE_WIDTH 1024
#define DEMO_SURFACE_HEIGHT 1024

#ifndef PROGRAM_NAME
#define PROGRAM_NAME "svg_demo"
#endif

//
//
//

namespace {

// A DemoImage derived class to display a single SVG document.
//
// TODO(digit): For simplicity, each instance has its own set of path handles.
// It might be useful to share these between several instances, but this requires
// non-trivial changes to the SvgSpinelImage class.
class SvgDemoImage : public DemoImage {
  // Type of a callback used to compute a transform to apply to a given frame
  // based on its counter value.
  using FrameTransformFunc = std::function<spn_transform_t(uint32_t)>;

 public:
  SvgDemoImage(const DemoImage::Config & config, const svg * svg, FrameTransformFunc transform_func)
      : transform_func_(transform_func)
  {
    svg_image_.init(svg, config.context, config.surface_width, config.surface_height);
    svg_image_.setupPaths();
  }

  ~SvgDemoImage()
  {
    svg_image_.reset();
  }

  void
  setup(uint32_t frame_counter) override
  {
    spn_transform_t transform = { .sx = 1., .sy = 1. };
    if (transform_func_)
      transform = transform_func_(frame_counter);

    svg_image_.setupRasters(&transform);
    svg_image_.setupLayers();
  }

  void
  render(void * submit_ext, uint32_t clip_width, uint32_t clip_height) override
  {
    svg_image_.render(submit_ext, clip_width, clip_height);
    svg_image_.resetRasters();
  }

  void
  flush() override
  {
    svg_image_.resetLayers();
  }

 protected:
  SvgSpinelImage     svg_image_ = {};
  FrameTransformFunc transform_func_;
};

}  // namespace

//
//
//

int
main(int argc, const char ** argv)
{
  // clang-format off

#define MY_OPTIONS_LIST(param)                                                                     \
  ARGPARSE_OPTION_DOUBLE(param, scale, 's', "scale", "Apply affine scale to the image.")           \
  ARGPARSE_OPTION_DOUBLE(param, fixed_scale, 'S', "fixed-scale", \
      "Fix animation scale to specific value. Useful for "  \
      "replicating rendering bugs. Implies --fixed-rotation=0 if that option is not used.") \
  ARGPARSE_OPTION_DOUBLE(param, fixed_rotation, 'R', "fixed-rotation", \
      "Fix animation rotation to specific angle value in degrees. Useful for " \
      "replicating rendering bugs. Implies --fixed-scale=1 if that option is not used.") \
  ARGPARSE_OPTION_FLAG(param, debug, 'D', "debug",                                                 \
      "Enable debug messages and Vulkan validation layers.")                                       \
  ARGPARSE_OPTION_STRING(param, window, '\0', "window", "Set window dimensions (e.g. 800x600).")   \
  ARGPARSE_OPTION_STRING(param, device, '\0', "device", "Device selection (vendor:device) IDs.")   \
  ARGPARSE_OPTION_STRING(param, format, '\0', "format", "Force pixel format [RGBA, BGRA].")        \
  ARGPARSE_OPTION_FLAG(param, fps, '\0', "fps", "Print frames per seconds to stdout.")             \
  ARGPARSE_OPTION_FLAG(param, no_vsync, '\0', "no-vsync",                                          \
      "Disable vsync synchronization. Useful for benchmarking. Note that this will disable "       \
      "presentation on Fuchsia as well.")                                                          \
  ARGPARSE_OPTION_FLAG(param, no_clear, '\0', "no-clear",                                          \
      "Disable image clear before rendering. Useful for benchmarking raw rendering performance.")

  // clang-format on
  ARGPARSE_DEFINE_OPTIONS_STRUCT(options, MY_OPTIONS_LIST);
  if (!ARGPARSE_PARSE_ARGS(argc, argv, options, MY_OPTIONS_LIST))
    {
      if (options.help_needed)
        ARGPARSE_PRINT_HELP(PROGRAM_NAME, "A short demo of Spinel rendering", MY_OPTIONS_LIST);
      exit(options.help_needed ? EXIT_SUCCESS : EXIT_FAILURE);
    }

  uint32_t vendor_id, device_id;
  if (!parseDeviceOption(options.device, &vendor_id, &device_id))
    return EXIT_FAILURE;

  uint32_t window_width, window_height;
  if (!parseWindowOption(options.window,
                         DEMO_SURFACE_WIDTH,
                         DEMO_SURFACE_HEIGHT,
                         &window_width,
                         &window_height))
    return EXIT_FAILURE;

  double svg_scale = 1.0;
  if (options.scale.used)
    {
      svg_scale = options.scale.value;
    }

  // Parse the SVG input document.
  if (argc < 2)
    {
      fprintf(stderr, "This program requires an input svg file path!\n");
      return EXIT_FAILURE;
    }
  ScopedSvg svg = ScopedSvg::parseFile(argv[1]);
  ASSERT_MSG(svg.get(), "Could not parse input SVG file: %s\n", argv[1]);

  VkOffset2D image_center = {};
  {
    double svg_xmin, svg_ymin, svg_xmax, svg_ymax;
    svg_estimate_bounds(svg.get(), nullptr, &svg_xmin, &svg_ymin, &svg_xmax, &svg_ymax);

    if (options.debug)
      {
        printf("Image bounds min=(%g,%g) max=(%g,%g) width=%g height=%g\n",
               svg_xmin,
               svg_ymin,
               svg_xmax,
               svg_ymax,
               svg_xmax - svg_xmin,
               svg_ymax - svg_ymin);
      }

    if (svg_xmin >= svg_xmax || svg_ymin >= svg_ymax)
      {
        fprintf(stderr, "WARNING: Could not compute bounds of SVG document!\n");
      }
    else
      {
        image_center.x = (int32_t)((svg_xmin + svg_xmax) / 2.);
        image_center.y = (int32_t)((svg_ymin + svg_ymax) / 2.);
      }
  }

  // Create the application window.
  DEMO_CLASS_NAME::Config config = {
    .app = {
      .app_name      = PROGRAM_NAME,
      .window_width  = window_width,
      .window_height = window_height,
      .verbose       = options.debug,
      .debug         = options.debug,
      .disable_vsync = options.no_vsync,
      .print_fps     = options.fps,
    },
    .no_clear = options.no_clear,
  };

  DEMO_CLASS_NAME demo(config);

  // Determine per-frame transform / animation.
  VkExtent2D swapchain_extent = demo.window_extent();

  auto per_frame_transform =
    [options, svg_scale, swapchain_extent, image_center](uint32_t frame_counter) {
      double angle;
      double scale;

      if (options.fixed_scale.used || options.fixed_rotation.used)
        {
          angle = options.fixed_rotation.used ? options.fixed_rotation.value * M_PI / 180. : 0.;
          scale = options.fixed_scale.used ? options.fixed_scale.value : 1.;
        }
      else
        {
          angle = (frame_counter / 60.) * M_PI;
          scale = (1. + 0.25 * (1. + sin(M_PI * frame_counter / 20.)));
        }
      scale *= svg_scale;

      affine_transform_t affine;

      affine = affine_transform_make_translation(-image_center.x, -image_center.y);
      affine = affine_transform_multiply_by_value(affine_transform_make_rotation(angle), affine);
      affine = affine_transform_multiply_by_value(affine_transform_make_scale(scale), affine);
      affine = affine_transform_multiply_by_value(
        affine_transform_make_translation(swapchain_extent.width / 2, swapchain_extent.height / 2),
        affine);

      return spn_transform_t{
        .sx  = (float)affine.sx,
        .shx = (float)affine.shx,
        .tx  = (float)affine.tx,
        .shy = (float)affine.shy,
        .sy  = (float)affine.sy,
        .ty  = (float)affine.ty,
      };
    };

  demo.setImageFactory([&](const DemoImage::Config & config) {
    return std::make_unique<SvgDemoImage>(config, svg.get(), per_frame_transform);
  });

  demo.run();

  return EXIT_SUCCESS;
}
