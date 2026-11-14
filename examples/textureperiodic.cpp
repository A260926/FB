// textureperiodic.cpp
//
// This program demonstrates noise::utils::BuildPeriodicTextureRegion(), the
// tileable mipmapped texture-baking interface.  A base texture is baked once
// as a whole and then baked again as independent rectangular chunks with
// negative and beyond-period offsets; the chunks are reassembled and compared
// value by value with the whole bake.  No image files are written.
//
// Copyright (C) 2024
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// (COPYING.txt) for more details.

#include <cassert>
#include <cstdio>

#include <noise/noise.h>

#include "noiseutils.h"

using namespace noise;

namespace
{

  // Reassembles periodic chunk coordinates into a physical texel index.
  int WrapIndex (int index, int size)
  {
    int wrapped = index % size;
    if (wrapped < 0) {
      wrapped += size;
    }
    return wrapped;
  }

}

int main ()
{
  module::Perlin source;
  source.SetSeed (31);
  source.SetOctaveCount (3);

  // A non-square base texture spanning an arbitrary physical area.
  utils::PeriodicTextureSpec spec;
  spec.width = 8;
  spec.height = 6;
  spec.lowerX = -2.0;
  spec.upperX = 2.0;
  spec.lowerZ = -1.0;
  spec.upperZ = 1.0;
  spec.bumpHeight = 0.5;

  // Reference bake: the whole base texture in one call.
  utils::NoiseMap wholeHeights;
  utils::Image wholeNormals;
  utils::BuildPeriodicTextureRegion (source, spec, 0, 0, 0,
    spec.width, spec.height, wholeHeights, wholeNormals);

  // Parallel-friendly bake: 3 x 2 chunks requested out of order, starting at
  // a negative offset so the first chunk crosses the period seam.
  const int chunkWidth = 3;
  const int chunkHeight = 2;
  utils::NoiseMap stitchedHeights (spec.width, spec.height);
  utils::Image stitchedNormals (spec.width, spec.height);

  for (int chunkY = spec.height - chunkHeight; chunkY >= -chunkHeight;
    chunkY -= chunkHeight) {
    for (int chunkX = -chunkWidth; chunkX < spec.width;
      chunkX += chunkWidth) {
      int width = chunkWidth;
      int height = chunkHeight;
      if (chunkX + width > spec.width) {
        width = spec.width - chunkX;
      }
      if (chunkY + height > spec.height) {
        height = spec.height - chunkY;
      }
      if (width <= 0 || height <= 0) {
        continue;
      }

      utils::NoiseMap chunkHeights;
      utils::Image chunkNormals;
      utils::BuildPeriodicTextureRegion (source, spec, 0, chunkX, chunkY,
        width, height, chunkHeights, chunkNormals);

      for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
          int physicalX = WrapIndex (chunkX + x, spec.width);
          int physicalY = WrapIndex (chunkY + y, spec.height);
          stitchedHeights.SetValue (physicalX, physicalY,
            chunkHeights.GetValue (x, y));
          stitchedNormals.SetValue (physicalX, physicalY,
            chunkNormals.GetValue (x, y));
        }
      }
    }
  }

  // The reassembled chunks must match the single whole bake exactly.
  for (int y = 0; y < spec.height; y++) {
    for (int x = 0; x < spec.width; x++) {
      assert (stitchedHeights.GetValue (x, y)
        == wholeHeights.GetValue (x, y));
      utils::Color a = stitchedNormals.GetValue (x, y);
      utils::Color b = wholeNormals.GetValue (x, y);
      assert (a.red == b.red && a.green == b.green
        && a.blue == b.blue && a.alpha == b.alpha);
    }
  }

  // The same region machinery produces the area-weighted mip levels.  Level
  // one of an 8 x 6 texture is 4 x 3; a small region still reads its real
  // periodic neighbors across the chunk boundary.
  utils::NoiseMap mipHeights;
  utils::Image mipNormals;
  utils::BuildPeriodicTextureRegion (source, spec, 1, 3, 2, 1, 1,
    mipHeights, mipNormals);
  assert (mipHeights.GetWidth () == 1 && mipHeights.GetHeight () == 1);
  assert (mipNormals.GetValue (0, 0).alpha == 255);

  std::printf ("textureperiodic: chunked bake matches whole bake, "
    "texels=%d normals=%d\n",
    spec.width * spec.height, spec.width * spec.height);
  return 0;
}
