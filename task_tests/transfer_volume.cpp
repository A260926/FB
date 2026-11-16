// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

// Task tests for the transfer-function CPU volume preview path.
// The reference path is an independent naive voxel-center trilinear sampler
// plus a phase-identical integrator written only for these tests.

#include "task_support/check.h"

#include "fj_volume.h"
#include "fj_shading.h"
#include "fj_object_group.h"
#include "fj_object_instance.h"
#include "fj_bvh_accelerator.h"
#include "fj_volume_accelerator.h"
#include "fj_accelerator.h"
#include "fj_mesh.h"
#include "fj_shader.h"
#include "fj_ray.h"
#include "fj_transform.h"
#include "fj_vector.h"
#include "fj_box.h"
#include "fj_color.h"
#include "fj_types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace fj;

namespace {

const double EPS = 1e-7;

bool near_value(double a, double b, double eps = EPS)
{
  return std::fabs(a - b) <= eps * (1 + std::fabs(a) + std::fabs(b));
}

bool near_color(const Color4 &a, const Color4 &b, double eps = EPS)
{
  return near_value(a.r, b.r, eps) && near_value(a.g, b.g, eps) &&
      near_value(a.b, b.b, eps) && near_value(a.a, b.a, eps);
}

// Independent plain voxel grid; never touches the density hierarchy.
struct NaiveGrid {
  int res[3];
  Box bounds;
  std::vector<float> voxels;

  NaiveGrid(int rx, int ry, int rz, const Box &b)
    : bounds(b), voxels(static_cast<size_t>(rx) * ry * rz, 0.f)
  {
    res[0] = rx; res[1] = ry; res[2] = rz;
  }

  float get(int x, int y, int z) const
  {
    if (x < 0 || res[0] <= x || y < 0 || res[1] <= y ||
        z < 0 || res[2] <= z) {
      return 0.f;  // out-of-range neighbours and box halo are zero
    }
    return voxels[(static_cast<size_t>(z) * res[1] + y) * res[0] + x];
  }

  double density_at(const Vector &p) const
  {
    if (!bounds.ContainsPoint(p)) {
      return 0.;
    }
    const Vector size = bounds.Diagonal();
    const Vector q(
        (p.x - bounds.min.x) / size.x * res[0] - .5,
        (p.y - bounds.min.y) / size.y * res[1] - .5,
        (p.z - bounds.min.z) / size.z * res[2] - .5);
    const int base[3] = {
        static_cast<int>(std::floor(q.x)),
        static_cast<int>(std::floor(q.y)),
        static_cast<int>(std::floor(q.z))};
    const double w[3] = {q.x - base[0], q.y - base[1], q.z - base[2]};

    double value = 0;
    for (int dz = 0; dz < 2; dz++) {
      for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
          const double wx = dx ? w[0] : 1 - w[0];
          const double wy = dy ? w[1] : 1 - w[1];
          const double wz = dz ? w[2] : 1 - w[2];
          value += wx * wy * wz * get(base[0] + dx, base[1] + dy, base[2] + dz);
        }
      }
    }
    return value;
  }
};

void copy_to_naive(const Volume &volume, NaiveGrid *naive)
{
  int res[3];
  volume.GetResolution(&res[0], &res[1], &res[2]);
  for (int z = 0; z < res[2]; z++) {
    for (int y = 0; y < res[1]; y++) {
      for (int x = 0; x < res[0]; x++) {
        naive->voxels[(static_cast<size_t>(z) * res[1] + y) * res[0] + x] =
            volume.GetValue(x, y, z);
      }
    }
  }
  naive->bounds = volume.GetBounds();
}

bool naive_box_clip(const Box &box, const Ray &ray, double *t0, double *t1)
{
  double lo = ray.tmin;
  double hi = ray.tmax;
  for (int axis = 0; axis < 3; axis++) {
    const double d = ray.dir[axis];
    const double o = ray.orig[axis];
    if (d == 0) {
      if (o < box.min[axis] || box.max[axis] < o) {
        return false;
      }
      continue;
    }
    double a = (box.min[axis] - o) / d;
    double b = (box.max[axis] - o) / d;
    if (a > b) std::swap(a, b);
    lo = Max(lo, a);
    hi = Min(hi, b);
    if (lo > hi) return false;
  }
  *t0 = lo;
  *t1 = hi;
  return hi > lo;
}

void naive_transfer(const std::vector<TransferKnot> &knots, double density,
    double *sigma, Color *emission)
{
  if (density <= knots.front().density) {
    *sigma = knots.front().extinction;
    *emission = knots.front().emission;
    return;
  }
  if (density >= knots.back().density) {
    *sigma = knots.back().extinction;
    *emission = knots.back().emission;
    return;
  }
  size_t lo = 0;
  size_t hi = knots.size() - 1;
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    if (knots[mid].density <= density) lo = mid;
    else hi = mid;
  }
  const double t = (density - knots[lo].density) /
      (knots[hi].density - knots[lo].density);
  *sigma = (1 - t) * knots[lo].extinction + t * knots[hi].extinction;
  emission->r = static_cast<float>(
      (1 - t) * knots[lo].emission.r + t * knots[hi].emission.r);
  emission->g = static_cast<float>(
      (1 - t) * knots[lo].emission.g + t * knots[hi].emission.g);
  emission->b = static_cast<float>(
      (1 - t) * knots[lo].emission.b + t * knots[hi].emission.b);
}

// Same parameter grid and same per-portion formulas as the production path.
Color4 naive_integrate(const NaiveGrid &grid,
    const std::vector<TransferKnot> &knots, const Ray &ray,
    double world_direction_length, double world_step,
    bool shadow_only, size_t *sample_count)
{
  *sample_count = 0;
  Color4 color;
  double t_enter = 0;
  double t_exit = 0;
  if (!naive_box_clip(grid.bounds, ray, &t_enter, &t_exit)) {
    return color;
  }

  const double t_step = world_step / world_direction_length;
  const double grid_left = ray.tmin +
      std::floor((t_enter - ray.tmin) / t_step) * t_step;
  double first_right = grid_left + t_step;

  double transmittance = 1;
  auto march = [&](double ta, double tb) {
    if (!(tb > ta)) return;
    const double t_mid = .5 * (ta + tb);
    const double density = grid.density_at(RayPointAt(ray, t_mid));
    (*sample_count)++;

    double sigma = 0;
    Color emission;
    naive_transfer(knots, density, &sigma, &emission);

    const double ds = (tb - ta) * world_direction_length;
    double portion_t = 1;
    double emission_factor = ds;
    if (sigma > 0) {
      portion_t = std::exp(-sigma * ds);
      emission_factor = (1 - portion_t) / sigma;
    }
    if (!shadow_only) {
      color.r += static_cast<float>(transmittance * emission.r * emission_factor);
      color.g += static_cast<float>(transmittance * emission.g * emission_factor);
      color.b += static_cast<float>(transmittance * emission.b * emission_factor);
    }
    transmittance *= portion_t;
  };

  if (first_right >= t_exit) {
    march(t_enter, t_exit);
  } else {
    if (first_right > t_enter) march(t_enter, first_right);
    double t = first_right;
    while (t + t_step <= t_exit) {
      march(t, t + t_step);
      t += t_step;
    }
    if (t < t_exit) march(t, t_exit);
  }

  color.a = static_cast<float>(1 - transmittance);
  return color;
}

std::vector<TransferKnot> make_basic_tf()
{
  // Two knots: density 0 -> nothing, density 1 -> sigma 1.5, warm emission.
  std::vector<TransferKnot> knots;
  knots.push_back(TransferKnot(0., 0., Color(0, 0, 0)));
  knots.push_back(TransferKnot(1., 1.5, Color(.8f, .3f, .1f)));
  return knots;
}

void fill_pattern(Volume *volume, int seed_span)
{
  int res[3];
  volume->GetResolution(&res[0], &res[1], &res[2]);
  for (int z = 0; z < res[2]; z++) {
    for (int y = 0; y < res[1]; y++) {
      for (int x = 0; x < res[0]; x++) {
        // Deterministic non-uniform pattern with several empty bands.
        const int m = (x * 7 + y * 13 + z * 29) % seed_span;
        const float value = (m < 3) ? 0.f : .15f + .07f * (m % 6);
        volume->SetValue(x, y, z, value);
      }
    }
  }
}

const Ray make_ray(const Vector &o, const Vector &d, double tmin, double tmax)
{
  Ray ray;
  ray.orig = o;
  ray.dir = d;
  ray.tmin = tmin;
  ray.tmax = tmax;
  return ray;
}

} // anonymous namespace

/* ========================================================================== */
/* range group */

static int range_validation_keeps_old_function(void)
{
  Volume volume;
  volume.Resize(8, 8, 8);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  fill_pattern(&volume, 11);
  volume.SetTransferFunction(make_basic_tf());

  const Ray ray = make_ray(Vector(.5, .5, -1), Vector(0, 0, 1), .1, 10);
  const Color4 reference = volume.IntegrateTransfer(ray, 1., .07);

  auto expect_throw = [&](const std::vector<TransferKnot> &bad) -> bool {
    bool threw = false;
    try {
      volume.SetTransferFunction(bad);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    return threw;
  };

  std::vector<TransferKnot> too_few;
  too_few.push_back(TransferKnot(0., 1., Color(1, 1, 1)));
  TASK_CHECK(expect_throw(too_few));

  std::vector<TransferKnot> not_increasing;
  not_increasing.push_back(TransferKnot(.5, 1., Color(1, 1, 1)));
  not_increasing.push_back(TransferKnot(.5, 1., Color(1, 1, 1)));
  TASK_CHECK(expect_throw(not_increasing));

  std::vector<TransferKnot> non_finite;
  non_finite.push_back(TransferKnot(0., 1., Color(1, 1, 1)));
  non_finite.push_back(TransferKnot(
      std::numeric_limits<double>::infinity(), 1., Color(1, 1, 1)));
  TASK_CHECK(expect_throw(non_finite));

  std::vector<TransferKnot> negative_sigma;
  negative_sigma.push_back(TransferKnot(0., -.1, Color(1, 1, 1)));
  negative_sigma.push_back(TransferKnot(1., 1., Color(1, 1, 1)));
  TASK_CHECK(expect_throw(negative_sigma));

  std::vector<TransferKnot> negative_emission;
  negative_emission.push_back(TransferKnot(0., 1., Color(-1, 1, 1)));
  negative_emission.push_back(TransferKnot(1., 1., Color(1, 1, 1)));
  TASK_CHECK(expect_throw(negative_emission));

  // Every rejection left the original transfer function in place.
  const Color4 after = volume.IntegrateTransfer(ray, 1., .07);
  TASK_CHECK(near_color(reference, after));

  // Non-finite voxel data is rejected and never reaches the buffer or tree.
  const float before = volume.GetValue(2, 3, 4);
  volume.SetValue(2, 3, 4, std::numeric_limits<float>::quiet_NaN());
  TASK_CHECK(volume.GetValue(2, 3, 4) == before);
  TASK_CHECK(volume.GetLastTransferRefitNodes() == 0);
  return 0;
}

static int range_clamps_outside_knots(void)
{
  Volume volume;
  volume.Resize(6, 6, 6);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));

  std::vector<TransferKnot> knots;
  knots.push_back(TransferKnot(.2, .5, Color(.2f, .2f, .2f)));
  knots.push_back(TransferKnot(.8, 2., Color(.9f, .7f, .4f)));
  volume.SetTransferFunction(knots);

  NaiveGrid naive(6, 6, 6, volume.GetBounds());
  std::vector<Ray> rays;

  // Uniform density below the first knot: clamps to the first knot.
  for (int i = 0; i < 6 * 6 * 6; i++) volume.SetValue(i % 6, (i / 6) % 6, i / 36, .1f);
  copy_to_naive(volume, &naive);
  rays.push_back(make_ray(Vector(.5, .5, -1), Vector(0, 0, 1), .2, 5));
  rays.push_back(make_ray(Vector(-1, .5, .5), Vector(1, 0, 0), .3, 5));

  // Uniform density above the last knot: clamps to the last knot.
  for (int i = 0; i < 6 * 6 * 6; i++) volume.SetValue(i % 6, (i / 6) % 6, i / 36, .95f);
  copy_to_naive(volume, &naive);
  rays.push_back(make_ray(Vector(.5, .5, 2), Vector(0, 0, -1), .4, 5));

  for (size_t i = 0; i < rays.size(); i++) {
    VolumeTransferStats stats;
    const Color4 got = volume.IntegrateTransfer(
        rays[i], Length(rays[i].dir), .05, &stats);
    size_t naive_samples = 0;
    const Color4 expect = naive_integrate(naive, knots, rays[i],
        Length(rays[i].dir), .05, false, &naive_samples);
    TASK_CHECK(near_color(got, expect));
    TASK_CHECK(stats.density_samples == naive_samples);
  }
  return 0;
}

static int range_transparent_interval_is_skipped(void)
{
  Volume volume;
  volume.Resize(16, 16, 16);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  for (int i = 0; i < 16 * 16 * 16; i++) {
    volume.SetValue(i % 16, (i / 16) % 16, i / 256, .3f);
  }

  // Active band around density .7 only; the whole volume sits at .3.
  std::vector<TransferKnot> knots;
  knots.push_back(TransferKnot(0., 0., Color(0, 0, 0)));
  knots.push_back(TransferKnot(.45, 0., Color(0, 0, 0)));
  knots.push_back(TransferKnot(.55, 1., Color(.6f, .2f, .5f)));
  knots.push_back(TransferKnot(1., 0., Color(0, 0, 0)));
  volume.SetTransferFunction(knots);

  const Ray ray = make_ray(Vector(.5, .5, -1), Vector(0, 0, 1), .1, 5);
  VolumeTransferStats stats;
  Color4 color = volume.IntegrateTransfer(ray, 1., .05, &stats);
  TASK_CHECK(color.r == 0 && color.g == 0 && color.b == 0 && color.a == 0);
  TASK_CHECK(stats.density_samples == 0);  // pruned at the hierarchy level

  // Editing a few voxels into the active band makes the volume visible again.
  volume.SetValue(8, 8, 8, .7f);
  volume.SetValue(8, 8, 9, .7f);
  stats = VolumeTransferStats();
  color = volume.IntegrateTransfer(
      make_ray(Vector(.53125, .53125, -1), Vector(0, 0, 1), .1, 5),
      1., .02, &stats);
  TASK_CHECK(color.a > 0);
  TASK_CHECK(stats.density_samples > 0);
  return 0;
}

static int range_narrow_peak_between_endpoints(void)
{
  Volume volume;
  volume.Resize(12, 12, 12);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));

  // Mix densities 0 and .7: node min/max spans [0, .7] while the transfer
  // function is nonzero only on a narrow interior band around .7.
  for (int z = 0; z < 12; z++) {
    for (int y = 0; y < 12; y++) {
      for (int x = 0; x < 12; x++) {
        volume.SetValue(x, y, z, ((x + y + z) % 4 == 0) ? .7f : 0.f);
      }
    }
  }

  std::vector<TransferKnot> knots;
  knots.push_back(TransferKnot(0., 0., Color(0, 0, 0)));
  knots.push_back(TransferKnot(.6, 0., Color(0, 0, 0)));
  knots.push_back(TransferKnot(.7, 1.2, Color(.3f, .8f, .2f)));
  knots.push_back(TransferKnot(.8, 0., Color(0, 0, 0)));
  knots.push_back(TransferKnot(1., 0., Color(0, 0, 0)));
  volume.SetTransferFunction(knots);

  NaiveGrid naive(12, 12, 12, volume.GetBounds());
  copy_to_naive(volume, &naive);

  const Ray rays[] = {
      make_ray(Vector(.5, .5, -1), Vector(0, 0, 1), .1, 5),
      make_ray(Vector(-1, -1, .5), Vector(1, 1, 0), .2, 5),
      make_ray(Vector(2, .5, .5), Vector(-1, 0, 0), .15, 5)};

  for (const Ray &ray : rays) {
    VolumeTransferStats stats;
    const Color4 got = volume.IntegrateTransfer(
        ray, Length(ray.dir), .04, &stats);
    size_t naive_samples = 0;
    const Color4 expect = naive_integrate(naive, knots, ray,
        Length(ray.dir), .04, false, &naive_samples);
    TASK_CHECK(near_color(got, expect));
    TASK_CHECK(stats.density_samples > 0);
    TASK_CHECK(stats.density_samples <= naive_samples);
  }
  return 0;
}

static int range_edge_zero_extension(void)
{
  Volume volume;
  volume.Resize(8, 8, 8);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  fill_pattern(&volume, 9);
  volume.SetTransferFunction(make_basic_tf());

  NaiveGrid naive(8, 8, 8, volume.GetBounds());
  copy_to_naive(volume, &naive);

  // Rays start well outside the box and cross its edges; the half-voxel
  // border region must read zero on both paths.
  const Ray rays[] = {
      make_ray(Vector(.5, .5, -3), Vector(0, 0, 1), .5, 5),
      make_ray(Vector(3, .5, .5), Vector(-1, 0, 0), .1, 7),
      make_ray(Vector(-2, 2, -2), Vector(1, -1, 1), .3, 9)};

  for (const Ray &ray : rays) {
    const Color4 got = volume.IntegrateTransfer(ray, Length(ray.dir), .06);
    size_t naive_samples = 0;
    const Color4 expect = naive_integrate(naive, make_basic_tf(), ray,
        Length(ray.dir), .06, false, &naive_samples);
    TASK_CHECK(near_color(got, expect));
  }

  // A ray missing the box entirely, and a zero-length clipped segment,
  // return all zero.
  Color4 miss = volume.IntegrateTransfer(
      make_ray(Vector(5, 5, -1), Vector(0, 0, 1), .1, 2), 1., .05);
  TASK_CHECK(miss.r == 0 && miss.g == 0 && miss.b == 0 && miss.a == 0);
  miss = volume.IntegrateTransfer(
      make_ray(Vector(.5, .5, 2), Vector(0, 0, 1), .1, .5), 1., .05);
  TASK_CHECK(miss.r == 0 && miss.g == 0 && miss.b == 0 && miss.a == 0);
  return 0;
}

/* ========================================================================== */
/* phase group */

static int phase_matches_naive_all_directions(void)
{
  Volume volume;
  volume.Resize(18, 14, 10);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(2, 1.5, 1)));
  fill_pattern(&volume, 13);
  const std::vector<TransferKnot> knots = make_basic_tf();
  volume.SetTransferFunction(knots);

  NaiveGrid naive(18, 14, 10, volume.GetBounds());
  copy_to_naive(volume, &naive);

  const Vector directions[] = {
      Vector(0, 0, 1), Vector(1, 0, 0), Vector(0, -1, 0),
      Vector(1, 1, 1), Vector(-1, -1, -1),
      Vector(.3, -.7, 1.4), Vector(-2., .5, -.9)};

  for (const Vector &d : directions) {
    // Non-unit directions exercise worldDirectionLength; reversed rays too.
    const Ray ray = make_ray(Vector(-1, .75, .5), d, .13, 12);
    VolumeTransferStats stats;
    const Color4 got = volume.IntegrateTransfer(
        ray, Length(ray.dir), .08, &stats);
    size_t naive_samples = 0;
    const Color4 expect = naive_integrate(naive, knots, ray,
        Length(ray.dir), .08, false, &naive_samples);
    TASK_CHECK(near_color(got, expect));
    TASK_CHECK(stats.density_samples == naive_samples);

    const Ray reversed = make_ray(
        Vector(-1, .75, .5) + d * 12, d * -1, .13, 12);
    const Color4 got_rev = volume.IntegrateTransfer(
        reversed, Length(reversed.dir), .08);
    size_t rev_samples = 0;
    const Color4 expect_rev = naive_integrate(naive, knots, reversed,
        Length(reversed.dir), .08, false, &rev_samples);
    TASK_CHECK(near_color(got_rev, expect_rev));
  }
  return 0;
}

static int phase_analytic_uniform_segment(void)
{
  Volume volume;
  volume.Resize(10, 10, 10);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  for (int i = 0; i < 1000; i++) volume.SetValue(i % 10, (i / 10) % 10, i / 100, .4f);

  const double sigma = 1.25;
  const Color emission(.5f, .25f, .125f);
  std::vector<TransferKnot> knots;
  knots.push_back(TransferKnot(0., sigma, emission));
  knots.push_back(TransferKnot(1., sigma, emission));
  volume.SetTransferFunction(knots);

  // Ray segment fully inside the box along z from .2 to .8.
  const Ray ray = make_ray(Vector(.5, .5, 0), Vector(0, 0, 1), .2, .8);
  const double length = .6;
  const double transmittance = std::exp(-sigma * length);
  const double analytic_alpha = 1 - transmittance;

  VolumeTransferStats stats;
  const Color4 got = volume.IntegrateTransfer(ray, 1., .037, &stats);
  TASK_CHECK(near_value(got.a, analytic_alpha, 1e-6));
  TASK_CHECK(near_value(got.r, emission.r * (1 - transmittance) / sigma, 1e-6));
  TASK_CHECK(near_value(got.g, emission.g * (1 - transmittance) / sigma, 1e-6));
  TASK_CHECK(near_value(got.b, emission.b * (1 - transmittance) / sigma, 1e-6));

  // Zero-extinction region: alpha stays zero and emission is j * length.
  std::vector<TransferKnot> emission_only;
  emission_only.push_back(TransferKnot(0., 0., emission));
  emission_only.push_back(TransferKnot(1., 0., emission));
  volume.SetTransferFunction(emission_only);
  const Color4 no_sigma = volume.IntegrateTransfer(ray, 1., .041);
  TASK_CHECK(near_value(no_sigma.a, 0., 1e-9));
  TASK_CHECK(near_value(no_sigma.r, emission.r * length, 1e-6));
  TASK_CHECK(near_value(no_sigma.g, emission.g * length, 1e-6));
  TASK_CHECK(near_value(no_sigma.b, emission.b * length, 1e-6));
  return 0;
}

static int phase_grid_anchored_at_tmin(void)
{
  Volume volume;
  volume.Resize(10, 10, 10);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  fill_pattern(&volume, 7);
  const std::vector<TransferKnot> knots = make_basic_tf();
  volume.SetTransferFunction(knots);

  NaiveGrid naive(10, 10, 10, volume.GetBounds());
  copy_to_naive(volume, &naive);

  // Awkward tmin produces partial head and tail cells; the production path
  // must keep the same phase as the naive grid and never restart at nodes.
  const double tmins[] = {.001, .017, .123, .31};
  for (double tmin : tmins) {
    const Ray ray = make_ray(Vector(.5, .5, -1), Vector(0, 0, 1), tmin, 4);
    VolumeTransferStats stats;
    const Color4 got = volume.IntegrateTransfer(ray, 1., .063, &stats);
    size_t naive_samples = 0;
    const Color4 expect = naive_integrate(naive, knots, ray, 1., .063,
        false, &naive_samples);
    TASK_CHECK(near_color(got, expect, 1e-6));
    TASK_CHECK(stats.density_samples == naive_samples);
  }

  // Stats reset on every integration.
  VolumeTransferStats stats;
  volume.IntegrateTransfer(
      make_ray(Vector(.5, .5, -1), Vector(0, 0, 1), .1, 4), 1., .1, &stats);
  const size_t first = stats.density_samples;
  TASK_CHECK(first > 0);
  volume.IntegrateTransfer(
      make_ray(Vector(5, 5, -1), Vector(0, 0, 1), .1, 4), 1., .1, &stats);
  TASK_CHECK(stats.density_samples == 0);
  return 0;
}

/* ========================================================================== */
/* refit group */

static int refit_single_write_grows_with_height(void)
{
  const int sizes[] = {16, 32, 64, 128};
  size_t counts[4] = {0, 0, 0, 0};
  const size_t leaf_counts[4] = {64, 512, 4096, 32768};
  // Tree levels (leaves -> root): dims 4,8,16,32 give heights 3,4,5,6.
  const size_t heights[4] = {3, 4, 5, 6};

  for (int s = 0; s < 4; s++) {
    Volume volume;
    volume.Resize(sizes[s], sizes[s], sizes[s]);
    volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
    volume.SetTransferFunction(make_basic_tf());  // builds the hierarchy

    const int interior = sizes[s] / 2;
    volume.SetValue(interior, interior, interior, .66f);
    counts[s] = volume.GetLastTransferRefitNodes();
    TASK_CHECK(counts[s] > 0);
    // O(log N): at most the eight halo leaves times the tree height.
    TASK_CHECK(counts[s] <= 8 * heights[s]);
    // And far below the leaf count once the tree has a few levels.
    if (s > 0) {
      TASK_CHECK(counts[s] * 8 < leaf_counts[s]);
    } else {
      TASK_CHECK(counts[s] < leaf_counts[s]);
    }
  }

  for (int s = 1; s < 4; s++) {
    TASK_CHECK(counts[s] > counts[s - 1]);          // grows with tree height
    TASK_CHECK(counts[s] <= counts[s - 1] + 8);    // one level adds <= 8 nodes
  }
  std::printf("REFIT refit_nodes 16^3=%zu 32^3=%zu 64^3=%zu 128^3=%zu\n",
      counts[0], counts[1], counts[2], counts[3]);
  return 0;
}

static int refit_matches_fresh_rebuild_and_naive(void)
{
  const int N = 12;
  Volume edited;
  edited.Resize(N, N, N);
  edited.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  fill_pattern(&edited, 11);
  edited.SetTransferFunction(make_basic_tf());

  // Edits on and around block boundaries (LEAF_BLOCK == 4): halo blocks on
  // both sides must pick the change up.
  const int edits[][3] = {
      {3, 3, 3}, {4, 4, 4}, {7, 2, 9}, {0, 0, 0}, {11, 11, 11}, {5, 8, 1}};
  for (const auto &e : edits) {
    edited.SetValue(e[0], e[1], e[2], .83f);
  }

  // A fresh volume with the same final voxel data rebuilds the whole tree.
  Volume rebuilt;
  rebuilt.Resize(N, N, N);
  rebuilt.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  for (int z = 0; z < N; z++) {
    for (int y = 0; y < N; y++) {
      for (int x = 0; x < N; x++) {
        rebuilt.SetValue(x, y, z, edited.GetValue(x, y, z));
      }
    }
  }
  rebuilt.SetTransferFunction(make_basic_tf());

  NaiveGrid naive(N, N, N, edited.GetBounds());
  copy_to_naive(edited, &naive);

  const Ray rays[] = {
      make_ray(Vector(.5, .5, -1), Vector(0, 0, 1), .1, 5),
      make_ray(Vector(-1, .3, .7), Vector(1, .2, .1), .2, 9),
      make_ray(Vector(1.2, 1.2, 1.2), Vector(-1, -1, -1), .05, 6),
      make_ray(Vector(.1, .9, -2), Vector(.05, -.1, 1), .3, 7)};

  for (const Ray &ray : rays) {
    VolumeTransferStats stats_edited;
    VolumeTransferStats stats_rebuilt;
    const Color4 got = edited.IntegrateTransfer(
        ray, Length(ray.dir), .05, &stats_edited);
    const Color4 fresh = rebuilt.IntegrateTransfer(
        ray, Length(ray.dir), .05, &stats_rebuilt);
    size_t naive_samples = 0;
    const Color4 expect = naive_integrate(naive, make_basic_tf(), ray,
        Length(ray.dir), .05, false, &naive_samples);
    TASK_CHECK(near_color(got, expect));
    TASK_CHECK(near_color(fresh, expect));
    TASK_CHECK(stats_edited.density_samples == stats_rebuilt.density_samples);
    TASK_CHECK(stats_edited.nodes_visited == stats_rebuilt.nodes_visited);
  }
  return 0;
}

static int refit_warm_hierarchy_switches_transfer_function(void)
{
  Volume volume;
  volume.Resize(10, 10, 10);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  fill_pattern(&volume, 9);
  volume.SetTransferFunction(make_basic_tf());

  const Ray ray = make_ray(Vector(.5, .5, -1), Vector(0, 0, 1), .1, 5);
  const Color4 first = volume.IntegrateTransfer(ray, 1., .05);
  TASK_CHECK(first.a > 0);

  // Switching functions never rescans voxels or rebuilds the tree; the refit
  // counter only moves on SetValue.
  const size_t counter_before = volume.GetLastTransferRefitNodes();
  std::vector<TransferKnot> shifted;
  shifted.push_back(TransferKnot(.4, .8, Color(.1f, .6f, .9f)));
  shifted.push_back(TransferKnot(.9, 2.4, Color(.7f, .2f, .8f)));
  volume.SetTransferFunction(shifted);
  TASK_CHECK(volume.GetLastTransferRefitNodes() == counter_before);

  NaiveGrid naive(10, 10, 10, volume.GetBounds());
  copy_to_naive(volume, &naive);
  size_t naive_samples = 0;
  Color4 expect = naive_integrate(naive, shifted, ray, 1., .05,
      false, &naive_samples);
  TASK_CHECK(near_color(volume.IntegrateTransfer(ray, 1., .05), expect));

  // Fully transparent replacement: whole-tree prune, no density samples.
  std::vector<TransferKnot> transparent;
  transparent.push_back(TransferKnot(0., 0., Color(0, 0, 0)));
  transparent.push_back(TransferKnot(1., 0., Color(0, 0, 0)));
  volume.SetTransferFunction(transparent);
  VolumeTransferStats stats;
  const Color4 invisible = volume.IntegrateTransfer(ray, 1., .05, &stats);
  TASK_CHECK(invisible.r == 0 && invisible.g == 0 && invisible.b == 0 &&
      invisible.a == 0);
  TASK_CHECK(stats.density_samples == 0);

  // And the same voxels become visible again with another function.
  volume.SetTransferFunction(make_basic_tf());
  const Color4 visible = volume.IntegrateTransfer(ray, 1., .05);
  TASK_CHECK(near_color(visible, first));
  return 0;
}

static int refit_resize_rebuilds_and_setbounds_remaps(void)
{
  Volume volume;
  volume.Resize(6, 6, 6);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  fill_pattern(&volume, 7);
  volume.SetTransferFunction(make_basic_tf());

  // Resize is allowed to rebuild; the transfer function is retained.
  volume.Resize(9, 9, 9);
  int res[3] = {0, 0, 0};
  volume.GetResolution(&res[0], &res[1], &res[2]);
  TASK_CHECK(res[0] == 9 && res[1] == 9 && res[2] == 9);
  for (int z = 0; z < 9; z++) {
    for (int y = 0; y < 9; y++) {
      for (int x = 0; x < 9; x++) {
        volume.SetValue(x, y, z, .25f + .05f * ((x + y + z) % 4));
      }
    }
  }

  // SetBounds only changes the geometric mapping; hierarchy indices survive.
  const Box new_bounds(Vector(-2, -1, 0), Vector(2, 1, 2));
  volume.SetBounds(new_bounds);
  NaiveGrid naive(9, 9, 9, new_bounds);
  copy_to_naive(volume, &naive);

  const Ray ray = make_ray(Vector(0, 0, -1), Vector(0, 0, 1), .2, 5);
  VolumeTransferStats stats;
  const Color4 got = volume.IntegrateTransfer(ray, 1., .11, &stats);
  size_t naive_samples = 0;
  const Color4 expect = naive_integrate(naive, make_basic_tf(), ray,
      1., .11, false, &naive_samples);
  TASK_CHECK(near_color(got, expect));
  TASK_CHECK(stats.density_samples == naive_samples);

  // Resolutions below two voxels per axis are rejected and keep the grid.
  volume.Resize(1, 9, 9);
  volume.GetResolution(&res[0], &res[1], &res[2]);
  TASK_CHECK(res[0] == 9);
  return 0;
}

/* ========================================================================== */
/* trace group */

class ConstantPreviewShader : public Shader {
  void evaluate(const TraceContext &, const SurfaceInput &,
      SurfaceOutput *out) const override
  {
    out->Cs = Color(.2f, .4f, .8f);
    out->Os = 1;
  }
};

static ObjectGroup *build_preview_group(Volume *volume,
    ObjectInstance *volume_object, BVHAccelerator *surface = NULL,
    ObjectInstance *surface_object = NULL)
{
  ObjectGroup *group = new ObjectGroup();
  volume_object->SetVolume(volume);
  volume_object->ComputeBounds();
  group->AddObject(volume_object);
  if (surface != NULL && surface_object != NULL) {
    surface_object->SetSurface(surface);
    surface_object->ComputeBounds();
    group->AddObject(surface_object);
  }
  group->ComputeBounds();
  const_cast<Accelerator *>(group->GetSurfaceAccelerator())->Build();
  VolumeAccBuild(const_cast<VolumeAccelerator *>(
      group->GetVolumeAccelerator()));
  return group;
}

static int trace_default_flag_keeps_legacy_mode(void)
{
  Volume volume;
  volume.Resize(8, 8, 8);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  for (int i = 0; i < 512; i++) volume.SetValue(i % 8, (i / 8) % 8, i / 64, .3f);
  volume.SetTransferFunction(make_basic_tf());

  ObjectInstance object;
  ConstantPreviewShader shader;
  object.SetShader(&shader, 0);
  ObjectGroup *group = build_preview_group(&volume, &object);
  const TraceContext camera = SlCameraContext(group);
  TASK_CHECK(camera.transfer_volume == false);

  // With the flag off, the legacy raymarcher still runs unchanged.
  TraceContext legacy = camera;
  legacy.raymarch_step = .1;
  legacy.raymarch_shadow_step = .1;
  legacy.opacity_threshold = 1;
  Color4 color;
  double hit_t = 0;
  const Vector legacy_o(.5, .5, -1);
  const Vector legacy_d(0, 0, 1);
  const int hit = SlTrace(&legacy,
      &legacy_o, &legacy_d, 1.2, 1.8, &color, &hit_t);
  TASK_CHECK(hit != 0);
  TASK_CHECK(color.a > 0 && color.a < 1);
  TASK_CHECK(near_value(color.r, .2 * color.a, 1e-6));
  delete group;
  return 0;
}

static int trace_matches_direct_integration_and_shadow(void)
{
  Volume volume;
  volume.Resize(12, 12, 12);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  fill_pattern(&volume, 11);
  volume.SetTransferFunction(make_basic_tf());

  ObjectInstance object;
  ObjectGroup *group = build_preview_group(&volume, &object);

  TraceContext cxt = SlCameraContext(group);
  cxt.transfer_volume = true;
  cxt.raymarch_step = .05;
  cxt.raymarch_shadow_step = .05;

  const Ray ray = make_ray(Vector(.5, .5, -1), Vector(.2, -.1, 1), .1, 6);
  Color4 traced;
  double hit_t = 0;
  TASK_CHECK(SlTrace(&cxt, &ray.orig, &ray.dir, ray.tmin, ray.tmax,
      &traced, &hit_t) != 0);

  VolumeTransferStats stats;
  const Color4 direct = volume.IntegrateTransfer(
      ray, Length(ray.dir), .05, &stats);
  TASK_CHECK(near_color(traced, direct));

  // Shadow context returns extinction alpha with no emission.
  TraceContext shadow = cxt;
  shadow.ray_context = CXT_SHADOW_RAY;
  Color4 shadow_color;
  TASK_CHECK(SlTrace(&shadow, &ray.orig, &ray.dir, ray.tmin, ray.tmax,
      &shadow_color, &hit_t) != 0);
  const Color4 direct_shadow = volume.IntegrateTransferShadow(
      ray, Length(ray.dir), .05);
  TASK_CHECK(near_value(shadow_color.a, direct_shadow.a));
  TASK_CHECK(shadow_color.r == 0 && shadow_color.g == 0 &&
      shadow_color.b == 0);
  delete group;
  return 0;
}

static int trace_surface_truncates_and_composites(void)
{
  Volume volume;
  volume.Resize(10, 10, 10);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  for (int i = 0; i < 1000; i++) {
    volume.SetValue(i % 10, (i / 10) % 10, i / 100, .4f);
  }
  volume.SetTransferFunction(make_basic_tf());

  Mesh mesh;
  mesh.SetPointCount(3);
  mesh.SetFaceCount(1);
  mesh.AddPointPosition();
  mesh.AddFaceIndices();
  mesh.SetPointPosition(0, Vector(-2, -2, .35));
  mesh.SetPointPosition(1, Vector(2, -2, .35));
  mesh.SetPointPosition(2, Vector(0, 2, .35));
  mesh.SetFaceIndices(0, Index3(0, 1, 2));
  mesh.ComputeNormals();
  mesh.ComputeBounds();
  BVHAccelerator surface;
  surface.SetPrimitiveSet(&mesh);
  TASK_CHECK(surface.Build() == 0);

  ConstantPreviewShader shader;
  ObjectInstance volume_object;
  ObjectInstance surface_object;
  surface_object.SetShader(&shader, 0);
  ObjectGroup *group = build_preview_group(
      &volume, &volume_object, &surface, &surface_object);

  TraceContext cxt = SlCameraContext(group);
  cxt.transfer_volume = true;
  cxt.raymarch_step = .04;
  cxt.opacity_threshold = 1;

  const Vector o(.5, .5, -1);
  const Vector d(0, 0, 1);
  Color4 traced;
  double hit_t = 0;
  TASK_CHECK(SlTrace(&cxt, &o, &d, .1, 4, &traced, &hit_t) != 0);
  TASK_CHECK(near_value(hit_t, 1.35, 1e-9));

  // Reference: integrate only up to the surface, then composite surface color.
  Ray clipped = make_ray(o, d, .1, hit_t);
  const Color4 volume_color = volume.IntegrateTransfer(clipped, 1., .04);
  TASK_CHECK(near_value(traced.a,
      volume_color.a + 1 * (1 - volume_color.a), 1e-6));
  TASK_CHECK(near_value(traced.r,
      volume_color.r + .2f * (1 - volume_color.a), 1e-6));
  TASK_CHECK(near_value(traced.g,
      volume_color.g + .4f * (1 - volume_color.a), 1e-6));
  TASK_CHECK(near_value(traced.b,
      volume_color.b + .8f * (1 - volume_color.a), 1e-6));
  delete group;
  return 0;
}

static int trace_non_uniform_transform_preserves_arc_length(void)
{
  Volume volume;
  volume.Resize(10, 10, 10);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  fill_pattern(&volume, 8);
  volume.SetTransferFunction(make_basic_tf());

  ObjectInstance object;
  object.SetScale(2., 1., .5, 0);  // non-uniform, non-unit world mapping
  ObjectGroup *group = build_preview_group(&volume, &object);

  TraceContext cxt = SlCameraContext(group);
  cxt.transfer_volume = true;
  cxt.raymarch_step = .07;

  const Ray world_ray = make_ray(
      Vector(-1, .5, .25), Vector(1.3, .4, .6), .2, 7);
  Color4 traced;
  double hit_t = 0;
  TASK_CHECK(SlTrace(&cxt, &world_ray.orig, &world_ray.dir,
      world_ray.tmin, world_ray.tmax, &traced, &hit_t) != 0);

  // Expected result: inverse-transform the ray at time 0 with the engine's
  // own transform API, keeping t and the world direction length.
  Ray object_ray = world_ray;
  {
    TransformSampleList list;
    XfmInitTransformSampleList(&list);
    XfmPushScaleSample(&list, 2., 1., .5, 0);
    Transform at_time;
    XfmLerpTransformSample(&list, 0, &at_time);
    XfmTransformPointInverse(&at_time, &object_ray.orig);
    XfmTransformVectorInverse(&at_time, &object_ray.dir);
  }
  const Color4 expect = volume.IntegrateTransfer(
      object_ray, Length(world_ray.dir), .07);
  TASK_CHECK(near_color(traced, expect));
  delete group;
  return 0;
}

static int trace_sparse_128_volume_prunes_most_samples(void)
{
  const int N = 128;
  Volume volume;
  volume.Resize(N, N, N);
  volume.SetBounds(Box(Vector(0, 0, 0), Vector(1, 1, 1)));
  // Mostly empty: only a 2^3 core carries density inside the 2-million grid.
  for (int z = 64; z < 66; z++) {
    for (int y = 64; y < 66; y++) {
      for (int x = 64; x < 66; x++) {
        volume.SetValue(x, y, z, .7f);
      }
    }
  }
  volume.SetTransferFunction(make_basic_tf());

  NaiveGrid naive(N, N, N, volume.GetBounds());
  copy_to_naive(volume, &naive);

  // Ray through empty space only: the hierarchy prunes whole subtrees.
  const Ray miss_ray = make_ray(
      Vector(.1, .1, -1), Vector(0, 0, 1), .1, 4);
  VolumeTransferStats stats;
  const Color4 missed = volume.IntegrateTransfer(
      miss_ray, 1., .01, &stats);
  size_t naive_miss_samples = 0;
  const Color4 naive_miss = naive_integrate(naive, make_basic_tf(), miss_ray,
      1., .01, false, &naive_miss_samples);
  TASK_CHECK(near_color(missed, naive_miss));
  TASK_CHECK(naive_miss_samples > 0);
  TASK_CHECK(stats.density_samples * 8 < naive_miss_samples);
  std::printf("SPARSE empty-ray density_samples=%zu naive=%zu ratio=1/%g\n",
      stats.density_samples, naive_miss_samples,
      naive_miss_samples / double(std::max<size_t>(1, stats.density_samples)));

  // Ray through the dense core: numeric agreement, still well under 1/8.
  const Ray hit_ray = make_ray(
      Vector(64.5 / N, 64.5 / N, -1), Vector(0, 0, 1), .1, 4);
  stats = VolumeTransferStats();
  const Color4 hit_color = volume.IntegrateTransfer(
      hit_ray, 1., .01, &stats);
  size_t naive_hit_samples = 0;
  const Color4 naive_hit = naive_integrate(naive, make_basic_tf(), hit_ray,
      1., .01, false, &naive_hit_samples);
  TASK_CHECK(near_color(hit_color, naive_hit, 1e-6));
  TASK_CHECK(stats.density_samples > 0);
  TASK_CHECK(stats.density_samples * 8 < naive_hit_samples);
  std::printf("SPARSE core-ray density_samples=%zu naive=%zu (1/%g of naive)\n",
      stats.density_samples, naive_hit_samples,
      naive_hit_samples / double(std::max<size_t>(1, stats.density_samples)));
  return 0;
}

/* ========================================================================== */

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
  {"range", "validation_keeps_old_function", range_validation_keeps_old_function},
  {"range", "clamps_outside_knots", range_clamps_outside_knots},
  {"range", "transparent_interval_is_skipped", range_transparent_interval_is_skipped},
  {"range", "narrow_peak_between_endpoints", range_narrow_peak_between_endpoints},
  {"range", "edge_zero_extension", range_edge_zero_extension},

  {"phase", "matches_naive_all_directions", phase_matches_naive_all_directions},
  {"phase", "analytic_uniform_segment", phase_analytic_uniform_segment},
  {"phase", "grid_anchored_at_tmin", phase_grid_anchored_at_tmin},

  {"refit", "single_write_grows_with_height", refit_single_write_grows_with_height},
  {"refit", "matches_fresh_rebuild_and_naive", refit_matches_fresh_rebuild_and_naive},
  {"refit", "warm_hierarchy_switch", refit_warm_hierarchy_switches_transfer_function},
  {"refit", "resize_rebuilds_setbounds_remaps", refit_resize_rebuilds_and_setbounds_remaps},

  {"trace", "default_flag_keeps_legacy_mode", trace_default_flag_keeps_legacy_mode},
  {"trace", "matches_direct_integration_and_shadow", trace_matches_direct_integration_and_shadow},
  {"trace", "surface_truncates_and_composites", trace_surface_truncates_and_composites},
  {"trace", "non_uniform_transform", trace_non_uniform_transform_preserves_arc_length},
  {"trace", "sparse_128_pruning", trace_sparse_128_volume_prunes_most_samples},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
