// task_periodic_texture.cpp
//
// Tests for noise::utils::BuildPeriodicTextureRegion().  The reference
// implementation in this file is written independently from the library code:
// it always builds whole mip levels and uses its own quintic blending and
// rectangle-overlap formulas.
//
// This file defines no main(); task_support/driver.c supplies one.

#include <cmath>
#include <cstddef>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include "task_support/check.h"

#include <noise/noise.h>
#include "noiseutils.h"

using namespace noise;

namespace
{

  // Non-flat source with no inherent periodicity.  The periodicity comes from
  // the four-point blending performed by the baking interface.  Every call is
  // counted and its (x, z) coordinates are recorded.
  class CountingModule: public module::Module
  {
    public:

      CountingModule ():
        module::Module (0),
        calls (0)
      {
      }

      virtual int GetSourceModuleCount () const
      {
        return 0;
      }

      virtual double GetValue (double x, double y, double z) const
      {
        (void)y;
        ++calls;
        footprint.insert (std::make_pair (x, z));
        return std::sin (0.73 * x + 1.31) * std::cos (0.91 * z - 0.43)
          + 0.31 * x - 0.23 * z + 0.17 * x * z;
      }

      mutable long calls;
      mutable std::set<std::pair<double, double> > footprint;
  };

  // A source that always returns a non-finite value.
  class NaNModule: public module::Module
  {
    public:

      NaNModule ():
        module::Module (0)
      {
      }

      virtual int GetSourceModuleCount () const
      {
        return 0;
      }

      virtual double GetValue (double x, double y, double z) const
      {
        (void)x;
        (void)y;
        (void)z;
        return std::sqrt (-1.0);
      }
  };

  // A source that throws an exception of its own type.
  class TeapotException
  {
  };

  class ThrowingModule: public module::Module
  {
    public:

      ThrowingModule ():
        module::Module (0)
      {
      }

      virtual int GetSourceModuleCount () const
      {
        return 0;
      }

      virtual double GetValue (double x, double y, double z) const
      {
        (void)x;
        (void)y;
        (void)z;
        throw TeapotException ();
      }
  };

  // An affine source.  Any weights summing to one reproduce it exactly, so
  // the physical slopes of the baked texture are known constants.
  class LinearModule: public module::Module
  {
    public:

      LinearModule ():
        module::Module (0)
      {
      }

      virtual int GetSourceModuleCount () const
      {
        return 0;
      }

      virtual double GetValue (double x, double y, double z) const
      {
        (void)y;
        return 2.0 * x + 3.0 * z + 0.5;
      }
  };

  // A source that is itself periodic over the texture's physical extents.
  class PeriodicSourceModule: public module::Module
  {
    public:

      PeriodicSourceModule (double extentX, double extentZ):
        module::Module (0),
        m_extentX (extentX),
        m_extentZ (extentZ)
      {
      }

      virtual int GetSourceModuleCount () const
      {
        return 0;
      }

      virtual double GetValue (double x, double y, double z) const
      {
        (void)y;
        const double pi = 3.14159265358979323846;
        return std::sin (2.0 * pi * x / m_extentX)
          * std::cos (2.0 * pi * z / m_extentZ);
      }

    private:

      double m_extentX;
      double m_extentZ;
  };

  double ReferenceQuintic (double t)
  {
    double t3 = t * t * t;
    return t3 * (t * t * 6.0 - t * 15.0 + 10.0);
  }

  // Independently written full-image base level.  Like the interface, each
  // layer stores float values; the blending itself is done in double.
  template <typename SourceType>
  std::vector<float> ReferenceBase (const SourceType& source,
    const utils::PeriodicTextureSpec& spec)
  {
    SourceType local;
    (void)source;
    double extentX = spec.upperX - spec.lowerX;
    double extentZ = spec.upperZ - spec.lowerZ;
    std::vector<float> out (
      static_cast<size_t>(spec.width) * spec.height);
    for (int j = 0; j < spec.height; j++) {
      for (int i = 0; i < spec.width; i++) {
        double u = (i + 0.5) / spec.width;
        double v = (j + 0.5) / spec.height;
        double px = spec.lowerX + u * extentX;
        double pz = spec.lowerZ + v * extentZ;
        double a = local.GetValue (px, 0.0, pz);
        double b = local.GetValue (px + extentX, 0.0, pz);
        double c = local.GetValue (px, 0.0, pz + extentZ);
        double d = local.GetValue (px + extentX, 0.0,
          pz + extentZ);
        double wx = 1.0 - ReferenceQuintic (u);
        double wz = 1.0 - ReferenceQuintic (v);
        // Same operation order as the specified blend: interpolate along X
        // first, then Z, in double precision before rounding to float.
        double row0 = a + (b - a) * wx;
        double row1 = c + (d - c) * wx;
        double blended = row0 + (row1 - row0) * wz;
        out[static_cast<size_t>(j) * spec.width + i] =
          static_cast<float>(blended);
      }
    }
    return out;
  }

  int LevelDim (int base, unsigned int level)
  {
    int value = base;
    for (unsigned int i = 0; i < level; i++) {
      value = (value > 1)? value / 2: 1;
    }
    return value;
  }

  // Overlap length between a child texel and a parent texel expressed on the
  // unit interval, then scaled back to parent-texel units.
  double ReferenceOverlap (int childIndex, int parentIndex, int childSize,
    int parentSize)
  {
    double c0 = static_cast<double>(childIndex) / childSize;
    double c1 = static_cast<double>(childIndex + 1) / childSize;
    double p0 = static_cast<double>(parentIndex) / parentSize;
    double p1 = static_cast<double>(parentIndex + 1) / parentSize;
    double lo = c0 > p0? c0: p0;
    double hi = c1 < p1? c1: p1;
    if (hi <= lo) {
      return 0.0;
    }
    return (hi - lo) * parentSize;
  }

  // Builds one full reference level from the previous full level.  The
  // previous layer is float; overlap accumulation stays in double and the
  // result is rounded to float, matching the interface's storage rule.
  std::vector<float> ReferenceDownsample (const std::vector<float>& parent,
    int parentWidth, int parentHeight, int childWidth, int childHeight)
  {
    std::vector<float> out (
      static_cast<size_t>(childWidth) * childHeight);
    for (int cy = 0; cy < childHeight; cy++) {
      for (int cx = 0; cx < childWidth; cx++) {
        double accum = 0.0;
        double totalWeight = 0.0;
        for (int py = 0; py < parentHeight; py++) {
          double wy = ReferenceOverlap (cy, py, childHeight, parentHeight);
          if (wy == 0.0) {
            continue;
          }
          for (int px = 0; px < parentWidth; px++) {
            double wx = ReferenceOverlap (cx, px, childWidth, parentWidth);
            if (wx == 0.0) {
              continue;
            }
            double weight = wx * wy;
            accum += static_cast<double>(
              parent[static_cast<size_t>(py) * parentWidth + px]) * weight;
            totalWeight += weight;
          }
        }
        out[static_cast<size_t>(cy) * childWidth + cx] =
          static_cast<float>(accum / totalWeight);
      }
    }
    return out;
  }

  // Independently encoded normal for one texel of a full reference level.
  utils::Color ReferenceNormal (const std::vector<float>& level, int width,
    int height, int x, int y, double spacingX, double spacingZ,
    double bumpHeight)
  {
    double slopeX = 0.0;
    double slopeZ = 0.0;
    if (width > 1) {
      int left = (x + width - 1) % width;
      int right = (x + 1) % width;
      slopeX = (static_cast<double>(
        level[static_cast<size_t>(y) * width + right])
        - level[static_cast<size_t>(y) * width + left])
        / (2.0 * spacingX);
    }
    if (height > 1) {
      int down = (y + height - 1) % height;
      int up = (y + 1) % height;
      slopeZ = (static_cast<double>(
        level[static_cast<size_t>(up) * width + x])
        - level[static_cast<size_t>(down) * width + x])
        / (2.0 * spacingZ);
    }
    double nx = -slopeX * bumpHeight;
    double nz = -slopeZ * bumpHeight;
    double length = std::sqrt (nx * nx + nz * nz + 1.0);
    double unitX = nx / length;
    double unitZ = nz / length;
    double unitY = 1.0 / length;
    utils::Color color;
    color.red = static_cast<uint8>(static_cast<uint>(
      std::floor ((unitX + 1.0) * 127.5)) & 0xff);
    color.green = static_cast<uint8>(static_cast<uint>(
      std::floor ((unitZ + 1.0) * 127.5)) & 0xff);
    color.blue = static_cast<uint8>(static_cast<uint>(
      std::floor ((unitY + 1.0) * 127.5)) & 0xff);
    color.alpha = 255;
    return color;
  }

  utils::PeriodicTextureSpec MakeSpec ()
  {
    utils::PeriodicTextureSpec spec;
    spec.width = 7;
    spec.height = 5;
    spec.lowerX = -1.2;
    spec.upperX = 3.4;
    spec.lowerZ = 0.5;
    spec.upperZ = 2.0;
    spec.bumpHeight = 0.37;
    return spec;
  }

}

// ---------------------------------------------------------------------------
// periodic group
// ---------------------------------------------------------------------------

static int PeriodicBasePixels ()
{
  utils::PeriodicTextureSpec spec = MakeSpec ();
  CountingModule source;
  utils::NoiseMap heights;
  utils::Image normals;
  utils::BuildPeriodicTextureRegion (source, spec, 0, 0, 0,
    spec.width, spec.height, heights, normals);

  std::vector<float> reference = ReferenceBase (source, spec);

  TASK_CHECK (heights.GetWidth () == spec.width);
  TASK_CHECK (heights.GetHeight () == spec.height);
  TASK_CHECK (normals.GetWidth () == spec.width);
  TASK_CHECK (normals.GetHeight () == spec.height);

  double lo = 1e30;
  double hi = -1e30;
  for (int j = 0; j < spec.height; j++) {
    for (int i = 0; i < spec.width; i++) {
      float expected = reference[
        static_cast<size_t>(j) * spec.width + i];
      lo = std::fmin (lo, expected);
      hi = std::fmax (hi, expected);
      TASK_CHECK (heights.GetValue (i, j) == expected);
      utils::Color color = normals.GetValue (i, j);
      double spacingX = (spec.upperX - spec.lowerX) / spec.width;
      double spacingZ = (spec.upperZ - spec.lowerZ) / spec.height;
      utils::Color expectedColor = ReferenceNormal (reference, spec.width,
        spec.height, i, j, spacingX, spacingZ, spec.bumpHeight);
      TASK_CHECK (color.red == expectedColor.red);
      TASK_CHECK (color.green == expectedColor.green);
      TASK_CHECK (color.blue == expectedColor.blue);
      TASK_CHECK (color.alpha == 255);
    }
  }
  // The counting source must produce a genuinely non-flat base texture.
  TASK_CHECK (hi > lo);

  // One bake of the whole base level evaluates exactly four samples per base
  // texel, with no duplicated evaluations.
  TASK_CHECK (source.calls == 4L * spec.width * spec.height);
  TASK_CHECK (static_cast<int>(source.footprint.size ())
    == 4 * spec.width * spec.height);

  // Every recorded coordinate must be one of the four periodic sample points.
  double extentX = spec.upperX - spec.lowerX;
  double extentZ = spec.upperZ - spec.lowerZ;
  std::set<std::pair<double, double> > expectedPoints;
  for (int j = 0; j < spec.height; j++) {
    for (int i = 0; i < spec.width; i++) {
      double u = (i + 0.5) / spec.width;
      double v = (j + 0.5) / spec.height;
      double px = spec.lowerX + u * extentX;
      double pz = spec.lowerZ + v * extentZ;
      expectedPoints.insert (std::make_pair (px, pz));
      expectedPoints.insert (std::make_pair (px + extentX, pz));
      expectedPoints.insert (std::make_pair (px, pz + extentZ));
      expectedPoints.insert (std::make_pair (px + extentX, pz + extentZ));
    }
  }
  TASK_CHECK (source.footprint == expectedPoints);
  return 0;
}

static int PeriodicWrapping ()
{
  utils::PeriodicTextureSpec spec = MakeSpec ();
  CountingModule source;
  utils::NoiseMap whole;
  utils::Image wholeNormals;
  utils::BuildPeriodicTextureRegion (source, spec, 0, 0, 0,
    spec.width, spec.height, whole, wholeNormals);

  // A region one period away and a negatively offseted region must reproduce
  // the same physical texels.
  utils::NoiseMap shifted;
  utils::Image shiftedNormals;
  utils::BuildPeriodicTextureRegion (source, spec, 0, -spec.width,
    -spec.height, spec.width, spec.height, shifted, shiftedNormals);
  utils::NoiseMap negative;
  utils::Image negativeNormals;
  utils::BuildPeriodicTextureRegion (source, spec, 0, -1, -1, 1, 1,
    negative, negativeNormals);

  for (int j = 0; j < spec.height; j++) {
    for (int i = 0; i < spec.width; i++) {
      TASK_CHECK (shifted.GetValue (i, j) == whole.GetValue (i, j));
      TASK_CHECK (shiftedNormals.GetValue (i, j).red
        == wholeNormals.GetValue (i, j).red);
      TASK_CHECK (shiftedNormals.GetValue (i, j).green
        == wholeNormals.GetValue (i, j).green);
      TASK_CHECK (shiftedNormals.GetValue (i, j).blue
        == wholeNormals.GetValue (i, j).blue);
    }
  }
  TASK_CHECK (negative.GetValue (0, 0)
    == whole.GetValue (spec.width - 1, spec.height - 1));
  TASK_CHECK (negativeNormals.GetValue (0, 0).blue
    == wholeNormals.GetValue (spec.width - 1, spec.height - 1).blue);
  return 0;
}

// ---------------------------------------------------------------------------
// pyramid group
// ---------------------------------------------------------------------------

static int PyramidAreaLevels ()
{
  utils::PeriodicTextureSpec spec = MakeSpec ();
  TASK_CHECK (LevelDim (spec.width, 1) == 3);
  TASK_CHECK (LevelDim (spec.height, 1) == 2);
  TASK_CHECK (LevelDim (spec.width, 2) == 1);
  TASK_CHECK (LevelDim (spec.height, 2) == 1);

  std::vector<float> base = ReferenceBase (CountingModule (), spec);
  std::vector<float> level1 = ReferenceDownsample (base, 7, 5, 3, 2);
  std::vector<float> level2 = ReferenceDownsample (level1, 3, 2, 1, 1);

  // The odd first level must keep the last row and column instead of dropping
  // them, and the overlap weights must partition the whole parent axis: each
  // child texel covers parentSize / childSize parent-texel widths.
  double xWeightSum = 0.0;
  for (int px = 0; px < 7; px++) {
    xWeightSum += ReferenceOverlap (0, px, 3, 7);
  }
  double yWeightSum = 0.0;
  for (int py = 0; py < 5; py++) {
    yWeightSum += ReferenceOverlap (0, py, 2, 5);
  }
  TASK_CHECK (std::fabs (xWeightSum - 7.0 / 3.0) < 1e-12);
  TASK_CHECK (std::fabs (yWeightSum - 5.0 / 2.0) < 1e-12);
  double firstWeightSum = xWeightSum * yWeightSum;
  TASK_CHECK (std::fabs (firstWeightSum - 35.0 / 6.0) < 1e-12);

  CountingModule source;
  for (unsigned int level = 0; level <= 2; level++) {
    int width = LevelDim (spec.width, level);
    int height = LevelDim (spec.height, level);
    std::vector<float> reference =
      (level == 0)? base: (level == 1)? level1: level2;

    utils::NoiseMap heights;
    utils::Image normals;
    utils::BuildPeriodicTextureRegion (source, spec, level, 0, 0,
      width, height, heights, normals);
    TASK_CHECK (heights.GetWidth () == width);
    TASK_CHECK (heights.GetHeight () == height);

    double spacingX = (spec.upperX - spec.lowerX) / width;
    double spacingZ = (spec.upperZ - spec.lowerZ) / height;
    for (int j = 0; j < height; j++) {
      for (int i = 0; i < width; i++) {
        float expected = reference[
          static_cast<size_t>(j) * width + i];
        TASK_CHECK (heights.GetValue (i, j) == expected);
        utils::Color expectedColor = ReferenceNormal (reference, width,
          height, i, j, spacingX, spacingZ, spec.bumpHeight);
        utils::Color actual = normals.GetValue (i, j);
        TASK_CHECK (actual.red == expectedColor.red);
        TASK_CHECK (actual.green == expectedColor.green);
        TASK_CHECK (actual.blue == expectedColor.blue);
        TASK_CHECK (actual.alpha == 255);
      }
    }
  }

  // The 1 x 1 terminal level has zero derivatives: its normal points straight
  // up and is fully opaque.
  utils::NoiseMap terminalHeights;
  utils::Image terminalNormals;
  utils::BuildPeriodicTextureRegion (source, spec, 2, 0, 0, 1, 1,
    terminalHeights, terminalNormals);
  utils::Color terminal = terminalNormals.GetValue (0, 0);
  TASK_CHECK (terminal.red == 127);
  TASK_CHECK (terminal.green == 127);
  TASK_CHECK (terminal.blue == 255);
  TASK_CHECK (terminal.alpha == 255);
  return 0;
}

static int PyramidValidation ()
{
  utils::PeriodicTextureSpec spec = MakeSpec ();
  CountingModule source;
  utils::NoiseMap heights (2, 2);
  utils::Image normals (2, 2);
  heights.Clear (123.25f);
  normals.Clear (utils::Color (7, 8, 9, 10));

  // Requesting a level below the terminal 1 x 1 level is invalid.
  bool threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, spec, 3, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  // Regions larger than their level are invalid.
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, spec, 1, 0, 0, 4, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  // Non-positive region sizes are invalid.
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, spec, 0, 0, 0, 0, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  // Bad specs are rejected for every documented reason.
  utils::PeriodicTextureSpec bad = spec;
  bad.width = 0;
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, bad, 0, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  bad = spec;
  bad.upperX = bad.lowerX;
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, bad, 0, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  bad = spec;
  bad.bumpHeight = std::sqrt (-1.0);
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, bad, 0, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  // Non-finite source values are rejected.
  NaNModule nanSource;
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (nanSource, spec, 0, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  // None of the failed calls may have modified the two output objects.
  TASK_CHECK (heights.GetWidth () == 2);
  TASK_CHECK (heights.GetHeight () == 2);
  TASK_CHECK (heights.GetValue (0, 0) == 123.25f);
  TASK_CHECK (heights.GetValue (1, 1) == 123.25f);
  utils::Color untouched = normals.GetValue (0, 0);
  TASK_CHECK (untouched.red == 7 && untouched.green == 8
    && untouched.blue == 9 && untouched.alpha == 10);
  return 0;
}

static int PyramidValidationEdges ()
{
  CountingModule source;
  utils::NoiseMap heights;
  utils::Image normals;

  // Size cap: 4096 is accepted, 4097 is rejected.
  utils::PeriodicTextureSpec big = MakeSpec ();
  big.width = 4096;
  big.height = 4096;
  bool threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, big, 0, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (!threw);

  big.width = 4097;
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, big, 0, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  // Non-finite bounds and non-finite span are rejected.
  utils::PeriodicTextureSpec bad = MakeSpec ();
  bad.lowerZ = std::sqrt (-1.0);
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, bad, 0, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  bad = MakeSpec ();
  bad.upperX = std::numeric_limits<double>::infinity ();
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, bad, 0, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  // A negative but finite bump height is legal; only finiteness is required.
  utils::PeriodicTextureSpec negativeBump = MakeSpec ();
  negativeBump.bumpHeight = -1.5;
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, negativeBump, 0, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (!threw);

  // A 1 x 1 base texture is itself the terminal level; level one is rejected.
  utils::PeriodicTextureSpec single = MakeSpec ();
  single.width = 1;
  single.height = 1;
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, single, 0, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (!threw);
  TASK_CHECK (heights.GetWidth () == 1 && heights.GetHeight () == 1);
  utils::Color flat = normals.GetValue (0, 0);
  TASK_CHECK (flat.red == 127 && flat.green == 127 && flat.blue == 255
    && flat.alpha == 255);

  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, single, 1, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  // When one axis reaches one first, halving continues on the other axis
  // until the whole level is 1 x 1; one level deeper is rejected.
  utils::PeriodicTextureSpec narrow = MakeSpec ();
  narrow.width = 2;
  narrow.height = 5;
  TASK_CHECK (LevelDim (narrow.width, 1) == 1);
  TASK_CHECK (LevelDim (narrow.height, 1) == 2);
  TASK_CHECK (LevelDim (narrow.height, 2) == 1);
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, narrow, 2, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (!threw);
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, narrow, 3, 0, 0, 1, 1,
      heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  // An absurdly large level is rejected during validation, before any
  // mip-chain storage is allocated.
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (source, MakeSpec (),
      0xffffffffU, 0, 0, 1, 1, heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);

  // An infinite source value is rejected like any other non-finite result.
  class InfinityModule: public module::Module
  {
    public:
      InfinityModule (): module::Module (0) {}
      virtual int GetSourceModuleCount () const { return 0; }
      virtual double GetValue (double, double, double) const
      {
        return std::numeric_limits<double>::infinity ();
      }
  };
  InfinityModule infinitySource;
  threw = false;
  try {
    utils::BuildPeriodicTextureRegion (infinitySource, MakeSpec (), 0, 0, 0,
      1, 1, heights, normals);
  } catch (const noise::ExceptionInvalidParam&) {
    threw = true;
  }
  TASK_CHECK (threw);
  return 0;
}

// ---------------------------------------------------------------------------
// regions group
// ---------------------------------------------------------------------------

static int RegionsStitchEquivalence ()
{
  utils::PeriodicTextureSpec spec = MakeSpec ();
  CountingModule source;

  for (unsigned int level = 0; level <= 2; level++) {
    int width = LevelDim (spec.width, level);
    int height = LevelDim (spec.height, level);

    utils::NoiseMap wholeHeights;
    utils::Image wholeNormals;
    utils::BuildPeriodicTextureRegion (source, spec, level, 0, 0,
      width, height, wholeHeights, wholeNormals);

    utils::NoiseMap stitchedHeights (width, height);
    utils::Image stitchedNormals (width, height);

    // Deliberately irregular, out-of-order pieces with negative and
    // beyond-period offsets.  Together they cover every physical texel (a few
    // seam texels are written twice, which is only sound because the values
    // are required to be identical).
    struct Piece
    {
      int x;
      int y;
      int w;
      int h;
    };
    std::vector<Piece> pieces;
    if (width >= 2 && height >= 2) {
      pieces.push_back (Piece { 1, -1, width - 1, 1 });
      pieces.push_back (Piece { 0, 0, width - 1, height - 1 });
      pieces.push_back (Piece { -1, -1, 2, 2 });
      pieces.push_back (Piece { -1, 1, 1, height - 1 });
      pieces.push_back (Piece { width, height, 1, 1 });
    } else {
      pieces.push_back (Piece { -3, 7, 1, 1 });
    }

    for (size_t p = 0; p < pieces.size (); p++) {
      const Piece& piece = pieces[p];
      utils::NoiseMap pieceHeights;
      utils::Image pieceNormals;
      utils::BuildPeriodicTextureRegion (source, spec, level, piece.x,
        piece.y, piece.w, piece.h, pieceHeights, pieceNormals);
      for (int ly = 0; ly < piece.h; ly++) {
        for (int lx = 0; lx < piece.w; lx++) {
          int physicalX = (piece.x + lx) % width;
          int physicalY = (piece.y + ly) % height;
          if (physicalX < 0) {
            physicalX += width;
          }
          if (physicalY < 0) {
            physicalY += height;
          }
          stitchedHeights.SetValue (physicalX, physicalY,
            pieceHeights.GetValue (lx, ly));
          stitchedNormals.SetValue (physicalX, physicalY,
            pieceNormals.GetValue (lx, ly));
        }
      }
    }

    for (int j = 0; j < height; j++) {
      for (int i = 0; i < width; i++) {
        TASK_CHECK (stitchedHeights.GetValue (i, j)
          == wholeHeights.GetValue (i, j));
        utils::Color a = stitchedNormals.GetValue (i, j);
        utils::Color b = wholeNormals.GetValue (i, j);
        TASK_CHECK (a.red == b.red && a.green == b.green
          && a.blue == b.blue && a.alpha == b.alpha);
      }
    }
  }
  return 0;
}

static int RegionsSparseFootprint ()
{
  utils::PeriodicTextureSpec spec = MakeSpec ();

  // A single interior level-zero texel only needs its 3 x 3 base window, so
  // the source must see far fewer calls than a full bake.
  CountingModule smallSource;
  utils::NoiseMap smallHeights;
  utils::Image smallNormals;
  utils::BuildPeriodicTextureRegion (smallSource, spec, 0, 3, 2, 1, 1,
    smallHeights, smallNormals);
  TASK_CHECK (smallSource.calls == 4L * 3 * 3);
  TASK_CHECK (smallSource.calls < 4L * spec.width * spec.height);

  // A region touching a seam still resolves to nine distinct physical base
  // texels rather than building the whole level.
  CountingModule seamSource;
  utils::BuildPeriodicTextureRegion (seamSource, spec, 0, 0, 0, 1, 1,
    smallHeights, smallNormals);
  TASK_CHECK (seamSource.calls == 4L * 3 * 3);

  // At level one a single texel's normal border covers the whole 3 x 2 level,
  // which in turn reaches a set of base texels.  Derive that expected base
  // footprint independently as the union of the bordered child window's
  // overlaps rather than assuming the whole base is rebuilt.
  CountingModule mipSource;
  utils::BuildPeriodicTextureRegion (mipSource, spec, 1, 1, 1, 1, 1,
    smallHeights, smallNormals);
  std::set<int> touchedX;
  std::set<int> touchedY;
  int level1Width = LevelDim (spec.width, 1);
  int level1Height = LevelDim (spec.height, 1);
  int childMinX = (level1Width == 1)? 1: 0;
  int childMaxX = (level1Width == 1)? 1: 2;
  int childMinY = (level1Height == 1)? 1: 0;
  int childMaxY = (level1Height == 1)? 1: 2;
  for (int cx = childMinX; cx <= childMaxX; cx++) {
    for (int px = 0; px < spec.width; px++) {
      if (ReferenceOverlap (cx, px, level1Width, spec.width) > 0.0) {
        touchedX.insert (px);
      }
    }
  }
  for (int cy = childMinY; cy <= childMaxY; cy++) {
    for (int py = 0; py < spec.height; py++) {
      if (ReferenceOverlap (cy, py, level1Height, spec.height) > 0.0) {
        touchedY.insert (py);
      }
    }
  }
  long expectedMipCalls =
    4L * static_cast<long>(touchedX.size ()) * touchedY.size ();
  TASK_CHECK (mipSource.calls == expectedMipCalls);
  TASK_CHECK (expectedMipCalls <= 4L * spec.width * spec.height);
  return 0;
}

static int RegionsSourceExceptionPassthrough ()
{
  utils::PeriodicTextureSpec spec = MakeSpec ();
  ThrowingModule source;
  utils::NoiseMap heights (2, 2);
  utils::Image normals (2, 2);
  heights.Clear (-5.0f);
  normals.Clear (utils::Color (1, 2, 3, 4));

  bool passedThrough = false;
  try {
    utils::BuildPeriodicTextureRegion (source, spec, 0, 0, 0, 1, 1,
      heights, normals);
  } catch (const TeapotException&) {
    passedThrough = true;
  } catch (...) {
    passedThrough = false;
  }
  TASK_CHECK (passedThrough);

  // The source's failure must not have rewritten the outputs.
  TASK_CHECK (heights.GetWidth () == 2);
  TASK_CHECK (heights.GetValue (0, 0) == -5.0f);
  utils::Color color = normals.GetValue (0, 0);
  TASK_CHECK (color.red == 1 && color.green == 2 && color.blue == 3
    && color.alpha == 4);
  return 0;
}

static int PeriodicPhysicalNormals ()
{
  // An affine source.  Unit-sum weights reproduce it exactly, but the quintic
  // blend shifts the effective sample position, so the physical slope at a
  // texel is the source slope scaled by one minus the quintic derivative.
  // Those slopes are still known analytically, so the normal encoding can be
  // checked against closed-form values.
  utils::PeriodicTextureSpec spec = MakeSpec ();
  LinearModule source;
  double extentX = spec.upperX - spec.lowerX;
  double extentZ = spec.upperZ - spec.lowerZ;

  // Level zero: bit-exact heights against the independent full-image blend.
  // Normals are checked against a closed-form expression of the blended
  // affine composite, evaluated with wrapped neighbor texels.
  std::vector<float> referenceBase = ReferenceBase (source, spec);
  utils::NoiseMap heights;
  utils::Image normals;
  utils::BuildPeriodicTextureRegion (source, spec, 0, 0, 0,
    spec.width, spec.height, heights, normals);
  double spacingX = extentX / spec.width;
  double spacingZ = extentZ / spec.height;

  // Closed form of the blended composite for f(x, z) = 2x + 3z + 0.5:
  // g(u, v) = const + 2 Lx (u + 1 - q(u)) + 3 Lz (v + 1 - q(v)).
  double affineConstant = 2.0 * spec.lowerX + 3.0 * spec.lowerZ + 0.5;
  struct AffineComposite
  {
    static double At (double u, double v, double constant, double lx,
      double lz)
    {
      return constant + 2.0 * lx * (u + 1.0 - ReferenceQuintic (u))
        + 3.0 * lz * (v + 1.0 - ReferenceQuintic (v));
    }
  };
  for (int j = 0; j < spec.height; j++) {
    for (int i = 0; i < spec.width; i++) {
      TASK_CHECK (heights.GetValue (i, j)
        == referenceBase[static_cast<size_t>(j) * spec.width + i]);

      int leftIndex = (i + spec.width - 1) % spec.width;
      int rightIndex = (i + 1) % spec.width;
      int downIndex = (j + spec.height - 1) % spec.height;
      int upIndex = (j + 1) % spec.height;
      double uLeft = (leftIndex + 0.5) / spec.width;
      double uRight = (rightIndex + 0.5) / spec.width;
      double vDown = (downIndex + 0.5) / spec.height;
      double vUp = (upIndex + 0.5) / spec.height;
      double v = (j + 0.5) / spec.height;
      double u = (i + 0.5) / spec.width;
      double slopeX = (AffineComposite::At (uRight, v, affineConstant,
          extentX, extentZ)
        - AffineComposite::At (uLeft, v, affineConstant, extentX, extentZ))
        / (2.0 * spacingX);
      double slopeZ = (AffineComposite::At (u, vUp, affineConstant,
          extentX, extentZ)
        - AffineComposite::At (u, vDown, affineConstant, extentX, extentZ))
        / (2.0 * spacingZ);
      double nx = -slopeX * spec.bumpHeight;
      double nz = -slopeZ * spec.bumpHeight;
      double length = std::sqrt (nx * nx + nz * nz + 1.0);
      uint8 expectedRed = static_cast<uint8>(static_cast<uint>(
        std::floor ((nx / length + 1.0) * 127.5)) & 0xff);
      uint8 expectedGreen = static_cast<uint8>(static_cast<uint>(
        std::floor ((nz / length + 1.0) * 127.5)) & 0xff);
      uint8 expectedBlue = static_cast<uint8>(static_cast<uint>(
        std::floor ((1.0 / length + 1.0) * 127.5)) & 0xff);
      utils::Color color = normals.GetValue (i, j);
      TASK_CHECK (color.red == expectedRed);
      TASK_CHECK (color.green == expectedGreen);
      TASK_CHECK (color.blue == expectedBlue);
      TASK_CHECK (color.alpha == 255);
    }
  }

  // Higher levels: heights and normals must match the independent full-image
  // area-downsample and normal chains value for value.
  std::vector<float> level1 = ReferenceDownsample (referenceBase,
    spec.width, spec.height, LevelDim (spec.width, 1),
    LevelDim (spec.height, 1));
  std::vector<float> level2 = ReferenceDownsample (level1,
    LevelDim (spec.width, 1), LevelDim (spec.height, 1),
    LevelDim (spec.width, 2), LevelDim (spec.height, 2));
  const std::vector<float>* references[3] =
    { &referenceBase, &level1, &level2 };
  for (unsigned int level = 1; level <= 2; level++) {
    int width = LevelDim (spec.width, level);
    int height = LevelDim (spec.height, level);
    utils::NoiseMap levelHeights;
    utils::Image levelNormals;
    utils::BuildPeriodicTextureRegion (source, spec, level, 0, 0,
      width, height, levelHeights, levelNormals);
    double levelSpacingX = extentX / width;
    double levelSpacingZ = extentZ / height;
    for (int j = 0; j < height; j++) {
      for (int i = 0; i < width; i++) {
        TASK_CHECK (levelHeights.GetValue (i, j)
          == (*references[level])[
            static_cast<size_t>(j) * width + i]);
        utils::Color expected = ReferenceNormal (*references[level], width,
          height, i, j, levelSpacingX, levelSpacingZ, spec.bumpHeight);
        utils::Color actual = levelNormals.GetValue (i, j);
        TASK_CHECK (actual.red == expected.red);
        TASK_CHECK (actual.green == expected.green);
        TASK_CHECK (actual.blue == expected.blue);
        TASK_CHECK (actual.alpha == expected.alpha);
      }
    }
  }
  return 0;
}

static int PeriodicSeamDerivativeContinuity ()
{
  // With a source that is periodic over the texture extents, the four blended
  // samples are equal and the baked field equals the source.  The periodic
  // central differences across the seam must therefore match the differences
  // between interior neighbors, proving no first-order jump at the seam.
  utils::PeriodicTextureSpec spec = MakeSpec ();
  double extentX = spec.upperX - spec.lowerX;
  double extentZ = spec.upperZ - spec.lowerZ;
  PeriodicSourceModule source (extentX, extentZ);

  utils::NoiseMap heights;
  utils::Image normals;
  utils::BuildPeriodicTextureRegion (source, spec, 0, 0, 0,
    spec.width, spec.height, heights, normals);

  double spacingX = extentX / spec.width;
  double spacingZ = extentZ / spec.height;
  for (int j = 0; j < spec.height; j++) {
    for (int i = 0; i < spec.width; i++) {
      int left = (i + spec.width - 1) % spec.width;
      int right = (i + 1) % spec.width;
      int down = (j + spec.height - 1) % spec.height;
      int up = (j + 1) % spec.height;

      // First differences across the periodic seam.
      double slopeX = (heights.GetValue (right, j)
        - heights.GetValue (left, j)) / (2.0 * spacingX);
      double slopeZ = (heights.GetValue (i, up)
        - heights.GetValue (i, down)) / (2.0 * spacingZ);

      // The same physical neighbor points evaluated straight from the
      // periodic source.  The blend introduces no jump because all four
      // periodic samples of this source are equal.
      double u = (i + 0.5) / spec.width;
      double v = (j + 0.5) / spec.height;
      double px = spec.lowerX + u * extentX;
      double pz = spec.lowerZ + v * extentZ;
      double sourceSlopeX = (source.GetValue (px + spacingX, 0.0, pz)
        - source.GetValue (px - spacingX, 0.0, pz))
        / (2.0 * spacingX);
      double sourceSlopeZ = (source.GetValue (px, 0.0, pz + spacingZ)
        - source.GetValue (px, 0.0, pz - spacingZ))
        / (2.0 * spacingZ);
      TASK_CHECK (std::fabs (slopeX - sourceSlopeX) < 1e-5);
      TASK_CHECK (std::fabs (slopeZ - sourceSlopeZ) < 1e-5);

      // Second differences must also be continuous across the seam: compare
      // the wrapped second difference with one taken from source samples.
      double secondX = (heights.GetValue (right, j)
        - 2.0 * heights.GetValue (i, j) + heights.GetValue (left, j))
        / (spacingX * spacingX);
      double sourceSecondX = (source.GetValue (px + spacingX, 0.0, pz)
        - 2.0 * source.GetValue (px, 0.0, pz)
        + source.GetValue (px - spacingX, 0.0, pz))
        / (spacingX * spacingX);
      TASK_CHECK (std::fabs (secondX - sourceSecondX) < 1e-4);
    }
  }

  // A chunk containing only seam texels (negative offsets) must encode the
  // same normals as the whole bake at those physical texels.
  utils::NoiseMap chunkHeights;
  utils::Image chunkNormals;
  utils::BuildPeriodicTextureRegion (source, spec, 0, -2, -1, 2, 1,
    chunkHeights, chunkNormals);
  for (int lx = 0; lx < 2; lx++) {
    int physicalX = spec.width - 2 + lx;
    int physicalY = spec.height - 1;
    utils::Color a = chunkNormals.GetValue (lx, 0);
    utils::Color b = normals.GetValue (physicalX, physicalY);
    TASK_CHECK (a.red == b.red && a.green == b.green
      && a.blue == b.blue && a.alpha == b.alpha);
  }
  return 0;
}

static int RegionsPartialMipSubset ()
{
  // On a larger pyramid, a single interior level-one texel plus its border
  // leaves part of the base level untouched, so its call count must be a
  // strict subset of a full level-one bake.
  utils::PeriodicTextureSpec spec = MakeSpec ();
  spec.width = 16;
  spec.height = 12;

  CountingModule partialSource;
  utils::NoiseMap heights;
  utils::Image normals;
  utils::BuildPeriodicTextureRegion (partialSource, spec, 1, 4, 3, 1, 1,
    heights, normals);

  CountingModule fullSource;
  utils::NoiseMap fullHeights;
  utils::Image fullNormals;
  utils::BuildPeriodicTextureRegion (fullSource, spec, 1, 0, 0,
    LevelDim (spec.width, 1), LevelDim (spec.height, 1),
    fullHeights, fullNormals);

  TASK_CHECK (partialSource.calls > 0);
  TASK_CHECK (partialSource.calls < fullSource.calls);
  TASK_CHECK (fullSource.calls
    == 4L * spec.width * spec.height);
  return 0;
}

// Registration consumed by task_support/driver.c.
const struct task_case task_cases[] = {
  { "periodic", "base_pixels_and_footprint", PeriodicBasePixels },
  { "periodic", "periodic_wrapping", PeriodicWrapping },
  { "periodic", "physical_normals", PeriodicPhysicalNormals },
  { "periodic", "seam_derivative_continuity",
    PeriodicSeamDerivativeContinuity },
  { "pyramid", "area_weighted_levels", PyramidAreaLevels },
  { "pyramid", "validation_and_untouched_outputs", PyramidValidation },
  { "pyramid", "validation_edges", PyramidValidationEdges },
  { "regions", "stitch_equivalence", RegionsStitchEquivalence },
  { "regions", "sparse_call_footprint", RegionsSparseFootprint },
  { "regions", "partial_mip_subset", RegionsPartialMipSubset },
  { "regions", "source_exception_passthrough",
    RegionsSourceExceptionPassthrough },
};

const size_t task_case_count =
  sizeof (task_cases) / sizeof (task_cases[0]);
