// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "spinel/ext/svg2spinel/svg2spinel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "spinel/spinel_opcodes.h"
#include "tests/common/spinel/spinel_test_utils.h"
#include "tests/common/svg/scoped_svg.h"
#include "tests/common/utils.h"
#include "tests/mock_spinel/mock_spinel_test_utils.h"

namespace {

class Svg2SpinelTest : public mock_spinel::Test {
};

}  // namespace

//
//
//

TEST_F(Svg2SpinelTest, polyline)
{
  char const doc[] = { "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
                       "  <g style = \"fill: #FF0000\">\n"
                       "    <polyline points = \"0,0 16,0 16,16 0,16 0,0\"/>\n"
                       "  </g>\n"
                       "</svg>\n" };

  ScopedSvg svg = ScopedSvg::parseString(doc);
  ASSERT_TRUE(svg.get());

  uint32_t const layer_count = svg_layer_count(svg.get());
  EXPECT_EQ(layer_count, 1u);

  // Verify path coordinates.
  spn_path_t * paths = spn_svg_paths_decode(svg.get(), path_builder_);
  ASSERT_TRUE(paths);
  {
    const auto & paths = mock_context()->paths();
    ASSERT_EQ(paths.size(), 1u);
    const auto & path = paths[0];
    // clang-format off
    static const float kExpectedPath[] = {
      MOCK_SPINEL_PATH_MOVE_TO_LITERAL(0,   0),
      MOCK_SPINEL_PATH_LINE_TO_LITERAL(16,  0),
      MOCK_SPINEL_PATH_LINE_TO_LITERAL(16, 16),
      MOCK_SPINEL_PATH_LINE_TO_LITERAL(0,  16),
      MOCK_SPINEL_PATH_LINE_TO_LITERAL(0,   0),
    };
    // clang-format on
    ASSERT_THAT(path.data, ::testing::ElementsAreArray(kExpectedPath));
  }
  // Verify raster stack.
  transform_stack * ts = transform_stack_create(32);
  transform_stack_push_identity(ts);

  spn_raster_t * rasters = spn_svg_rasters_decode(svg.get(), raster_builder_, paths, ts);
  ASSERT_TRUE(rasters);

  {
    const auto & rasters = mock_context()->rasters();
    ASSERT_EQ(rasters.size(), 1u);
    const auto & raster = rasters[0];

    ASSERT_EQ(raster.size(), 1u);
    const auto & raster_path = raster[0];
    EXPECT_EQ(raster_path.path_id, paths[0].handle);
    EXPECT_SPN_TRANSFORM_IS_IDENTITY(raster_path.transform);
  }

  // Verify composition and layers
  spn_svg_layers_decode(svg.get(), rasters, composition_, styling_, true);

  {
    const auto & prints = mock_composition()->prints();
    ASSERT_EQ(prints.size(), 1u);
    const auto & print = prints[0];
    EXPECT_EQ(print.raster_id, rasters[0].handle);
    EXPECT_EQ(print.layer_id, 0u);
    const spn_txty_t zero_translation = { 0, 0 };
    EXPECT_SPN_TXTY_EQ(print.translation, zero_translation);

    const auto layerMap = mock_composition()->computeLayerMap();
    ASSERT_EQ(layerMap.size(), 1u);
    auto const it = layerMap.find(0u);
    ASSERT_TRUE(it != layerMap.end());
    EXPECT_EQ(it->first, 0u);
    EXPECT_EQ(it->second.size(), 1u);
    EXPECT_EQ(it->second[0], &print);
  }

  {
    const auto & groups = mock_styling()->groups();
    ASSERT_EQ(groups.size(), 1u);
    const auto & group = groups[0];
    const auto   it    = group.layer_commands.find(0u);
    ASSERT_TRUE(it != group.layer_commands.end());
    const auto & commands = it->second;

    uint32_t kExpectedCommands[] = {
      static_cast<uint32_t>(SPN_STYLING_OPCODE_COVER_NONZERO),
      static_cast<uint32_t>(SPN_STYLING_OPCODE_COLOR_FILL_SOLID),
      0,  // filled later.
      0,  // filled later.
      static_cast<uint32_t>(SPN_STYLING_OPCODE_BLEND_OVER),
    };
    static const float kRedRgba[4] = { 1., 0., 0., 1. };
    mock_spinel::Spinel::rgbaToCmds(kRedRgba, kExpectedCommands + 2);

    ASSERT_THAT(commands, ::testing::ElementsAreArray(kExpectedCommands));
  }

  transform_stack_release(ts);

  spn_svg_rasters_release(svg.get(), context_, rasters);
  spn_svg_paths_release(svg.get(), context_, paths);
}

//
//
//

TEST_F(Svg2SpinelTest, skewX)
{
  char const doc[] = { "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
                       "<rect width=\"16\" height=\"16\" transform=\"skewX(45)\"/>\n"
                       "</svg>\n" };

  // expect skewX transform
  spn_transform_t const skewX = { 1.0f, 1.0f, 0.0f,  // shx = tan(45º) = 1
                                  0.0f, 1.0f, 0.0f,  //
                                  0.0f, 0.0f };

  ScopedSvg svg = ScopedSvg::parseString(doc);
  ASSERT_TRUE(svg.get());

  // create paths
  spn_path_t * paths = spn_svg_paths_decode(svg.get(), path_builder_);
  ASSERT_TRUE(paths);

  // create transform stack
  transform_stack * ts = transform_stack_create(32);
  transform_stack_push_identity(ts);

  // decode rasters
  spn_raster_t * rasters = spn_svg_rasters_decode(svg.get(), raster_builder_, paths, ts);
  ASSERT_TRUE(rasters);

  // inspect transform
  ASSERT_TRUE(rasters);
  {
    const auto & mock_rasters = mock_context()->rasters();
    ASSERT_EQ(mock_rasters.size(), 1u);

    const auto & raster = mock_rasters[0][0];
    EXPECT_SPN_TRANSFORM_EQ(raster.transform, skewX);
  }

  transform_stack_release(ts);

  spn_svg_rasters_release(svg.get(), context_, rasters);

  spn_svg_paths_release(svg.get(), context_, paths);
}

//
//
//

TEST_F(Svg2SpinelTest, skewY)
{
  char const doc[] = { "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
                       "<rect width=\"16\" height=\"16\" transform=\"skewY(45)\"/>\n"
                       "</svg>\n" };

  // expect skewY transform
  spn_transform_t const skewY = { 1.0f, 0.0f, 0.0f,  //
                                  1.0f, 1.0f, 0.0f,  // shy = tan(45º) = 1
                                  0.0f, 0.0f };

  ScopedSvg svg = ScopedSvg::parseString(doc);
  ASSERT_TRUE(svg.get());

  // create paths
  spn_path_t * paths = spn_svg_paths_decode(svg.get(), path_builder_);
  ASSERT_TRUE(paths);

  // create transform stack
  transform_stack * ts = transform_stack_create(32);
  transform_stack_push_identity(ts);

  // decode rasters
  spn_raster_t * rasters = spn_svg_rasters_decode(svg.get(), raster_builder_, paths, ts);
  ASSERT_TRUE(rasters);

  // inspect transform
  ASSERT_TRUE(rasters);
  {
    const auto & mock_rasters = mock_context()->rasters();
    ASSERT_EQ(mock_rasters.size(), 1u);

    const auto & raster = mock_rasters[0][0];
    EXPECT_SPN_TRANSFORM_EQ(raster.transform, skewY);
  }

  transform_stack_release(ts);

  spn_svg_rasters_release(svg.get(), context_, rasters);

  spn_svg_paths_release(svg.get(), context_, paths);
}

//
//
//

TEST_F(Svg2SpinelTest, project)
{
  char const doc[] = { "<svg xmlns=\"http://www.w3.org/2000/svg\">\n"
                       "<g transform=\"project(1,2,3,4,5,6,7,8)\">\n"
                       "  <path d= \"M1,2 R 3,4 5,6 7\"/>\n"
                       "  <path d= \"M1,2 D 3,4 5,6 7,8, 9,10\"/>\n"
                       "</g>\n"
                       "</svg>\n" };

  // SVG:    sx shy shx sy  tx ty w0 w1
  // Spinel: sx shx tx  shy sy ty w0 w1
  spn_transform_t const project = { 1.0f, 3.0f, 5.0f,  //
                                    2.0f, 4.0f, 6.0f,  //
                                    7.0f, 8.0f };

  ScopedSvg svg = ScopedSvg::parseString(doc);
  ASSERT_TRUE(svg.get());

  // create paths
  spn_path_t * paths = spn_svg_paths_decode(svg.get(), path_builder_);
  ASSERT_TRUE(paths);

  {
    const auto & mock_paths = mock_context()->paths();
    ASSERT_EQ(mock_paths.size(), 2u);

    {
      const auto & path = mock_paths[0];

      static const float kExpectedPath[] = {

        MOCK_SPINEL_PATH_MOVE_TO_LITERAL(1, 2),               //
        MOCK_SPINEL_PATH_RAT_QUAD_TO_LITERAL(3, 4, 5, 6, 7),  //
        MOCK_SPINEL_PATH_LINE_TO_LITERAL(1, 2)
      };

      ASSERT_THAT(path.data, ::testing::ElementsAreArray(kExpectedPath));
    }

    {
      const auto & path = mock_paths[1];

      static const float kExpectedPath[] = {

        MOCK_SPINEL_PATH_MOVE_TO_LITERAL(1, 2),                          //
        MOCK_SPINEL_PATH_RAT_CUBIC_TO_LITERAL(3, 4, 5, 6, 7, 8, 9, 10),  //
        MOCK_SPINEL_PATH_LINE_TO_LITERAL(1, 2)
      };

      ASSERT_THAT(path.data, ::testing::ElementsAreArray(kExpectedPath));
    }
  }

  // create transform stack
  transform_stack * ts = transform_stack_create(32);
  transform_stack_push_identity(ts);

  // decode rasters
  spn_raster_t * rasters = spn_svg_rasters_decode(svg.get(), raster_builder_, paths, ts);
  ASSERT_TRUE(rasters);

  // inspect transform of first
  ASSERT_TRUE(rasters);
  {
    const auto & mock_rasters = mock_context()->rasters();
    ASSERT_EQ(mock_rasters.size(), 2u);

    {
      const auto & raster = mock_rasters[0][0];
      EXPECT_SPN_TRANSFORM_EQ(raster.transform, project);
    }
    {
      const auto & raster = mock_rasters[1][0];
      EXPECT_SPN_TRANSFORM_EQ(raster.transform, project);
    }
  }

  transform_stack_release(ts);

  spn_svg_rasters_release(svg.get(), context_, rasters);

  spn_svg_paths_release(svg.get(), context_, paths);
}

//
//
//

TEST_F(Svg2SpinelTest, circle)
{
  char const doc[] = { "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\">\n"
                       "  <circle cx=\"16\"  cy=\"512\" r=\"16\"/>\n"
                       "</svg>\n" };

  ScopedSvg svg = ScopedSvg::parseString(doc);
  ASSERT_TRUE(svg.get());

  // create paths
  spn_path_t * paths = spn_svg_paths_decode(svg.get(), path_builder_);
  ASSERT_TRUE(paths);

  {
    const auto & mock_paths = mock_context()->paths();
    ASSERT_EQ(mock_paths.size(), 1u);

    {
      const auto & path = mock_paths[0];
      // MOVE, QUAD, QUAD, QUAD
      ASSERT_EQ(path.data.size(), 3u + 6u + 6u + 6u);

      //
      // NOTE(allanmac): circles & ellipses are currently implemented
      // with 3 quads -- that may change in the future
      //
      // NOTE(allanmac): the values aren't integral so just check tags
      //
      ASSERT_EQ(path.data[0], ::mock_spinel::Path::MoveTo::kTag);
      ASSERT_EQ(path.data[3], ::mock_spinel::Path::RatQuadTo::kTag);
      ASSERT_EQ(path.data[9], ::mock_spinel::Path::RatQuadTo::kTag);
      ASSERT_EQ(path.data[15], ::mock_spinel::Path::RatQuadTo::kTag);
    }
  }

  spn_svg_paths_release(svg.get(), context_, paths);
}

//
//
//

TEST_F(Svg2SpinelTest, ellipse)
{
  char const doc[] = { "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\">\n"
                       "  <ellipse cx=\"16\"  cy=\"512\" rx=\"16\"  ry=\"32\" />\n"
                       "</svg>\n" };

  ScopedSvg svg = ScopedSvg::parseString(doc);
  ASSERT_TRUE(svg.get());

  // create paths
  spn_path_t * paths = spn_svg_paths_decode(svg.get(), path_builder_);
  ASSERT_TRUE(paths);

  {
    const auto & mock_paths = mock_context()->paths();
    ASSERT_EQ(mock_paths.size(), 1u);

    {
      const auto & path = mock_paths[0];
      // MOVE, QUAD, QUAD, QUAD
      ASSERT_EQ(path.data.size(), 3u + 6u + 6u + 6u);

      //
      // NOTE(allanmac): circles & ellipses are currently implemented
      // with 3 quads -- that may change in the future
      //
      // NOTE(allanmac): the values aren't integral so just check tags
      //
      ASSERT_EQ(path.data[0], ::mock_spinel::Path::MoveTo::kTag);
      ASSERT_EQ(path.data[3], ::mock_spinel::Path::RatQuadTo::kTag);
      ASSERT_EQ(path.data[9], ::mock_spinel::Path::RatQuadTo::kTag);
      ASSERT_EQ(path.data[15], ::mock_spinel::Path::RatQuadTo::kTag);
    }
  }

  spn_svg_paths_release(svg.get(), context_, paths);
}

//
//
//

TEST_F(Svg2SpinelTest, arc)
{
  char const doc[] = { "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\">\n"
                       "  <path d=\"M80 80\n"
                       "           A 45 45, 0, 0, 0, 125 125\n"
                       "           L 125 80 Z\" fill=\"green\"/>\n"
                       "</svg>\n" };

  ScopedSvg svg = ScopedSvg::parseString(doc);
  ASSERT_TRUE(svg.get());

  // create paths
  spn_path_t * paths = spn_svg_paths_decode(svg.get(), path_builder_);
  ASSERT_TRUE(paths);

  {
    const auto & mock_paths = mock_context()->paths();
    ASSERT_EQ(mock_paths.size(), 1u);

    {
      const auto & path = mock_paths[0];
      // MOVE, ARC, LINE, LINE, MOVE
      ASSERT_EQ(path.data.size(), 3u + 6u + 3u + 3u + 3u);

      //
      // NOTE(allanmac): this arc is 90 degrees which is currently
      // represented with one quad -- this may change in the future.
      //
      // NOTE(allanmac): the values aren't integral so just check tags
      //
      ASSERT_EQ(path.data[0], ::mock_spinel::Path::MoveTo::kTag);
      ASSERT_EQ(path.data[3], ::mock_spinel::Path::RatQuadTo::kTag);
      ASSERT_EQ(path.data[9], ::mock_spinel::Path::LineTo::kTag);
      ASSERT_EQ(path.data[12], ::mock_spinel::Path::LineTo::kTag);
      ASSERT_EQ(path.data[15], ::mock_spinel::Path::MoveTo::kTag);
    }
  }

  spn_svg_paths_release(svg.get(), context_, paths);
}

//
//
//
