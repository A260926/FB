// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#include "fj_volume.h"
#include "fj_numeric.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace fj {

namespace {

// At most this many voxel cells per leaf block along each axis.
const int LEAF_BLOCK = 4;

bool finite_real(Real x)
{
  return std::isfinite(x);
}

bool finite_color(const Color &c)
{
  return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b);
}

// Slab test that stays finite when a ray-direction component is zero.
bool ray_box_slabs(const Box &box, const Vector &orig, const Vector &dir,
    Real ray_tmin, Real ray_tmax, Real *hit_tmin, Real *hit_tmax)
{
  Real t0 = ray_tmin;
  Real t1 = ray_tmax;

  for (int axis = 0; axis < 3; axis++) {
    const Real d = dir[axis];
    const Real o = orig[axis];
    if (d == 0) {
      if (o < box.min[axis] || box.max[axis] < o) {
        return false;
      }
      continue;
    }
    Real ta = (box.min[axis] - o) / d;
    Real tb = (box.max[axis] - o) / d;
    if (ta > tb) {
      std::swap(ta, tb);
    }
    t0 = Max(t0, ta);
    t1 = Min(t1, tb);
    if (t0 > t1) {
      return false;
    }
  }

  *hit_tmin = t0;
  *hit_tmax = t1;
  return true;
}

} // anonymous namespace

// Density hierarchy node. Leaves have num_children == 0; the block range is
// stored in block units (one block == LEAF_BLOCK voxel cells).
struct Volume::TransferNode {
  TransferNode() :
    parent(-1), first_child(-1), num_children(0),
    min_density(0), max_density(0)
  {
    block_min[0] = block_min[1] = block_min[2] = 0;
    block_max[0] = block_max[1] = block_max[2] = 0;
  }

  int parent;
  int first_child;
  int num_children;
  int block_min[3];
  int block_max[3];
  Real min_density;
  Real max_density;
};

VoxelBuffer::VoxelBuffer() : data_(), res_()
{
}

VoxelBuffer::~VoxelBuffer()
{
}

void VoxelBuffer::Resize(int xres, int yres, int zres)
{
  data_.resize(xres * yres * zres, 0);
  res_ = Resolution(xres, yres, zres);
}

const Resolution &VoxelBuffer::GetResolution() const
{
  return res_;
}

bool VoxelBuffer::IsEmpty() const
{
  return data_.empty();
}

void VoxelBuffer::SetValue(int x, int y, int z, float value)
{
  if (x < 0 || res_.x <= x)
    return;
  if (y < 0 || res_.y <= y)
    return;
  if (z < 0 || res_.z <= z)
    return;

  const int64_t index = (z * res_.x * res_.y) + (y * res_.x) + x;
  data_[index] = value;
}

float VoxelBuffer::GetValue(int x, int y, int z) const
{
  if (x < 0 || res_.x <= x)
    return 0;
  if (y < 0 || res_.y <= y)
    return 0;
  if (z < 0 || res_.z <= z)
    return 0;

  const int64_t index = (z * res_.x * res_.y) + (y * res_.x) + x;
  return data_[index];
}

static float trilinear_buffer_value(const VoxelBuffer &buffer, const Vector &P);
static float nearest_buffer_value(const VoxelBuffer &buffer, const Vector &P);

Volume::Volume() :
  buffer_(),
  bounds_(),
  size_(),
  filtersize_(0),
  transfer_knots_(),
  transfer_ready_(false),
  nodes_(),
  node_root_(-1),
  tree_height_(0),
  last_refit_nodes_(0)
{
  block_count_[0] = block_count_[1] = block_count_[2] = 0;
  compute_filter_size();
}

Volume::~Volume()
{
}

void Volume::Resize(int xres, int yres, int zres)
{
  if (xres < 2 || yres < 2 || zres < 2) {
    return;
  }

  buffer_.Resize(xres, yres, zres);
  compute_filter_size();

  // The density tree follows the voxel grid, so a resize rebuilds it.
  // The transfer function itself is retained.
  if (transfer_ready_) {
    rebuild_density_hierarchy();
  } else {
    nodes_.clear();
    node_root_ = -1;
    tree_height_ = 0;
    block_count_[0] = block_count_[1] = block_count_[2] = 0;
    last_refit_nodes_ = 0;
  }
}

void Volume::SetBounds(const Box &bounds)
{
  bounds_ = bounds;
  size_ = bounds_.Diagonal();

  compute_filter_size();
  // Geometry mapping only: block indices and density ranges stay valid.
}

const Box &Volume::GetBounds() const
{
  return bounds_;
}

void Volume::GetResolution(int *i, int *j, int *k) const
{
  const Resolution &res = buffer_.GetResolution();
  *i = res.x;
  *j = res.y;
  *k = res.z;
}

Real Volume::GetFilterSize() const
{
  return filtersize_;
}

Vector Volume::IndexToPoint(int i, int j, int k) const
{
  const Resolution &res = buffer_.GetResolution();
  return Vector(
      (i + .5) / res.x * size_.x + bounds_.min.x,
      (j + .5) / res.y * size_.y + bounds_.min.y,
      (k + .5) / res.z * size_.z + bounds_.min.z);
}

void Volume::PointToIndex(const Vector &point, int *i, int *j, int *k) const
{
  const Resolution &res = buffer_.GetResolution();
  *i = (int) ((point.x - bounds_.min.x) / size_.x * res.x);
  *j = (int) ((point.y - bounds_.min.y) / size_.y * res.y);
  *k = (int) ((point.z - bounds_.min.z) / size_.z * res.z);

  if (point.x == res.x)
    *i -= 1;
  if (point.y == res.y)
    *j -= 1;
  if (point.z == res.z)
    *k -= 1;
}

void Volume::SetValue(int x, int y, int z, float value)
{
  if (buffer_.IsEmpty()) {
    return;
  }

  const Resolution &res = buffer_.GetResolution();
  if (x < 0 || res.x <= x || y < 0 || res.y <= y || z < 0 || res.z <= z) {
    last_refit_nodes_ = 0;
    return;
  }
  // Density data is contracted to be finite and non-negative; a non-finite
  // value would corrupt hierarchy min/max comparisons, so reject it.
  if (!std::isfinite(value)) {
    last_refit_nodes_ = 0;
    return;
  }

  buffer_.SetValue(x, y, z, value);

  if (transfer_ready_) {
    refit_after_write(x, y, z);
  } else {
    last_refit_nodes_ = 0;
  }
}

float Volume::GetValue(int x, int y, int z) const
{
  return buffer_.GetValue(x, y, z);
}

bool Volume::GetSample(const Vector &point, VolumeSample *sample) const
{
  if (buffer_.IsEmpty()) {
    return false;
  }

  const bool hit = bounds_.ContainsPoint(point);
  if (!hit) {
    return false;
  }

  const Resolution &res = buffer_.GetResolution();
  const Vector P(
      (point.x - bounds_.min.x) / size_.x * res.x,
      (point.y - bounds_.min.y) / size_.y * res.y,
      (point.z - bounds_.min.z) / size_.z * res.z);

  if (1) {
    sample->density = trilinear_buffer_value(buffer_, P);
  } else {
    sample->density = nearest_buffer_value(buffer_, P);
  }

  return true;
}

void Volume::compute_filter_size()
{
  if (buffer_.IsEmpty()) {
    filtersize_ = 0;
    return;
  }

  const Resolution &res = buffer_.GetResolution();
  const Vector voxelsize(
      size_.x / res.x,
      size_.y / res.y,
      size_.z / res.z);

  filtersize_ = Length(voxelsize);
}

void VolGetIndexRange(const Volume *volume,
    const Vector *center, double radius,
    int *xmin, int *ymin, int *zmin,
    int *xmax, int *ymax, int *zmax)
{
  const Vector P_min(
      center->x - radius,
      center->y - radius,
      center->z - radius);

  const Vector P_max(
      center->x + radius,
      center->y + radius,
      center->z + radius);

  volume->PointToIndex(P_min, xmin, ymin, zmin);
  volume->PointToIndex(P_max, xmax, ymax, zmax);
}

static float trilinear_buffer_value(const VoxelBuffer &buffer, const Vector &P)
{
  const Vector P_sample(
      P.x - .5,
      P.y - .5,
      P.z - .5);

  const int lowest_corner[3] = {
      (int) P_sample[0],
      (int) P_sample[1],
      (int) P_sample[2]};

  int x, y, z;
  float weight[3];
  float value = 0;

  for (int i = 0; i < 2; i++) {
    x = lowest_corner[0] + i;
    weight[0] = 1 - fabs(P_sample[0] - x);

    for (int j = 0; j < 2; j++) {
      y = lowest_corner[1] + j;
      weight[1] = 1 - fabs(P_sample[1] - y);

      for (int k = 0; k < 2; k++) {
        z = lowest_corner[2] + k;
        weight[2] = 1 - fabs(P_sample[2] - z);

        value += weight[0] * weight[1] * weight[2] * buffer.GetValue(x, y, z);
      }
    }
  }

  return value;
}

static float nearest_buffer_value(const VoxelBuffer &buffer, const Vector &P)
{
  const int x = (int) P.x;
  const int y = (int) P.y;
  const int z = (int) P.z;

  return buffer.GetValue(x, y, z);
}

/* -------------------------------------------------------------------------- */
/* Transfer-function preview path */

void Volume::SetTransferFunction(const std::vector<TransferKnot> &knots)
{
  if (knots.size() < 2) {
    throw std::invalid_argument("transfer function needs at least two knots");
  }

  for (size_t i = 0; i < knots.size(); i++) {
    const TransferKnot &knot = knots[i];
    if (!finite_real(knot.density) ||
        !finite_real(knot.extinction) ||
        !finite_color(knot.emission)) {
      throw std::invalid_argument("transfer function contains non-finite values");
    }
    if (knot.extinction < 0 ||
        knot.emission.r < 0 || knot.emission.g < 0 || knot.emission.b < 0) {
      throw std::invalid_argument("transfer function contains negative values");
    }
    if (i > 0 && !(knot.density > knots[i - 1].density)) {
      throw std::invalid_argument("transfer function densities must be strictly increasing");
    }
  }

  // The first valid function builds the density hierarchy; later functions
  // only replace the knots and never rescan the voxels or rebuild the tree.
  if (!transfer_ready_) {
    build_density_hierarchy();
    transfer_ready_ = true;
  }

  transfer_knots_ = knots;
}

size_t Volume::GetLastTransferRefitNodes() const
{
  return last_refit_nodes_;
}

bool Volume::transfer_active_on_range(Real dmin, Real dmax) const
{
  if (transfer_knots_.empty() || dmax < dmin) {
    return false;
  }

  // Piecewise-linear, non-negative data: extrema on the clipped density range
  // occur at the range boundaries and at knots inside the range. Looking only
  // at the interval endpoints would miss narrow peaks between them.
  Real sigma = 0;
  Color emission;

  evaluate_transfer(dmin, &sigma, &emission);
  if (sigma > 0 || emission.r > 0 || emission.g > 0 || emission.b > 0) {
    return true;
  }
  evaluate_transfer(dmax, &sigma, &emission);
  if (sigma > 0 || emission.r > 0 || emission.g > 0 || emission.b > 0) {
    return true;
  }

  for (size_t i = 0; i < transfer_knots_.size(); i++) {
    const Real d = transfer_knots_[i].density;
    if (d < dmin || dmax < d) {
      continue;
    }
    evaluate_transfer(d, &sigma, &emission);
    if (sigma > 0 || emission.r > 0 || emission.g > 0 || emission.b > 0) {
      return true;
    }
  }

  return false;
}

void Volume::evaluate_transfer(Real density, Real *sigma, Color *emission) const
{
  const std::vector<TransferKnot> &knots = transfer_knots_;
  const size_t n = knots.size();

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
  size_t hi = n - 1;
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    if (knots[mid].density <= density) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  const Real t = (density - knots[lo].density) /
      (knots[hi].density - knots[lo].density);

  *sigma = Lerp(knots[lo].extinction, knots[hi].extinction, t);
  emission->r = static_cast<float>(
      Lerp(Real(knots[lo].emission.r), Real(knots[hi].emission.r), t));
  emission->g = static_cast<float>(
      Lerp(Real(knots[lo].emission.g), Real(knots[hi].emission.g), t));
  emission->b = static_cast<float>(
      Lerp(Real(knots[lo].emission.b), Real(knots[hi].emission.b), t));
}

Real Volume::sample_density_center(const Vector &point) const
{
  const Resolution &res = buffer_.GetResolution();

  // Continuous voxel coordinates, shifted so integer values sit on voxel
  // centers. Out-of-range neighbours read as zero (box halo and edges).
  const Vector p(
      (point.x - bounds_.min.x) / size_.x * res.x - .5,
      (point.y - bounds_.min.y) / size_.y * res.y - .5,
      (point.z - bounds_.min.z) / size_.z * res.z - .5);

  const int base[3] = {
      static_cast<int>(std::floor(p.x)),
      static_cast<int>(std::floor(p.y)),
      static_cast<int>(std::floor(p.z))};
  const Real w[3] = {p.x - base[0], p.y - base[1], p.z - base[2]};

  Real value = 0;
  for (int dz = 0; dz < 2; dz++) {
    const Real wz = dz ? w[2] : 1 - w[2];
    for (int dy = 0; dy < 2; dy++) {
      const Real wy = dy ? w[1] : 1 - w[1];
      for (int dx = 0; dx < 2; dx++) {
        const Real wx = dx ? w[0] : 1 - w[0];
        value += wx * wy * wz *
            buffer_.GetValue(base[0] + dx, base[1] + dy, base[2] + dz);
      }
    }
  }
  return value;
}

void Volume::build_density_hierarchy()
{
  nodes_.clear();
  child_pool_.clear();
  node_root_ = -1;
  tree_height_ = 0;
  last_refit_nodes_ = 0;
  block_count_[0] = block_count_[1] = block_count_[2] = 0;

  if (buffer_.IsEmpty()) {
    return;
  }

  int res[3];
  GetResolution(&res[0], &res[1], &res[2]);

  int dims[3];
  for (int axis = 0; axis < 3; axis++) {
    dims[axis] = (res[axis] + LEAF_BLOCK - 1) / LEAF_BLOCK;
    block_count_[axis] = dims[axis];
  }

  // Leaves, in lexicographic block order (x, then y, then z).
  std::vector<int> level;
  level.reserve(static_cast<size_t>(dims[0]) * dims[1] * dims[2]);

  for (int bz = 0; bz < dims[2]; bz++) {
    for (int by = 0; by < dims[1]; by++) {
      for (int bx = 0; bx < dims[0]; bx++) {
        TransferNode node;
        node.parent = -1;
        node.first_child = -1;
        node.num_children = 0;
        node.block_min[0] = bx * LEAF_BLOCK;
        node.block_min[1] = by * LEAF_BLOCK;
        node.block_min[2] = bz * LEAF_BLOCK;
        node.block_max[0] = Min(node.block_min[0] + LEAF_BLOCK, res[0]);
        node.block_max[1] = Min(node.block_min[1] + LEAF_BLOCK, res[1]);
        node.block_max[2] = Min(node.block_min[2] + LEAF_BLOCK, res[2]);

        // Include the one-voxel halo and the zero values outside the grid,
        // which is the actual trilinear interpolation support of the block.
        Real mn = 0;
        Real mx = 0;
        for (int z = node.block_min[2] - 1; z <= node.block_max[2]; z++) {
          for (int y = node.block_min[1] - 1; y <= node.block_max[1]; y++) {
            for (int x = node.block_min[0] - 1; x <= node.block_max[0]; x++) {
              const Real v = buffer_.GetValue(x, y, z);
              mn = Min(mn, v);
              mx = Max(mx, v);
            }
          }
        }
        node.min_density = mn;
        node.max_density = mx;

        level.push_back(static_cast<int>(nodes_.size()));
        nodes_.push_back(node);
      }
    }
  }
  tree_height_ = 1;

  // Internal levels: groups of up to eight children, stored contiguously.
  while (level.size() > 1) {
    int parent_dims[3];
    for (int axis = 0; axis < 3; axis++) {
      parent_dims[axis] = (dims[axis] + 1) / 2;
    }

    std::vector<int> parent_level;
    parent_level.reserve(
        static_cast<size_t>(parent_dims[0]) * parent_dims[1] * parent_dims[2]);

    for (int pz = 0; pz < parent_dims[2]; pz++) {
      for (int py = 0; py < parent_dims[1]; py++) {
        for (int px = 0; px < parent_dims[0]; px++) {
          const int parent_index = static_cast<int>(nodes_.size());
          const int first_child = static_cast<int>(child_pool_.size());
          int child_count = 0;

          for (int cz = 0; cz < 2; cz++) {
            const int child_z = pz * 2 + cz;
            if (child_z >= dims[2]) {
              continue;
            }
            for (int cy = 0; cy < 2; cy++) {
              const int child_y = py * 2 + cy;
              if (child_y >= dims[1]) {
                continue;
              }
              for (int cx = 0; cx < 2; cx++) {
                const int child_x = px * 2 + cx;
                if (child_x >= dims[0]) {
                  continue;
                }
                const int child_index = level[
                    child_z * dims[1] * dims[0] +
                    child_y * dims[0] + child_x];
                child_pool_.push_back(child_index);
                child_count++;
              }
            }
          }

          TransferNode node;
          node.parent = -1;
          node.first_child = first_child;
          node.num_children = child_count;
          node.block_min[0] = node.block_min[1] = node.block_min[2] =
              std::numeric_limits<int>::max();
          node.block_max[0] = node.block_max[1] = node.block_max[2] =
              -std::numeric_limits<int>::max();

          Real mn = 0;
          Real mx = 0;
          for (int c = 0; c < child_count; c++) {
            const TransferNode &child = nodes_[child_pool_[first_child + c]];
            nodes_[child_pool_[first_child + c]].parent = parent_index;

            for (int axis = 0; axis < 3; axis++) {
              node.block_min[axis] = Min(node.block_min[axis],
                  child.block_min[axis]);
              node.block_max[axis] = Max(node.block_max[axis],
                  child.block_max[axis]);
            }
            mn = Min(mn, child.min_density);
            mx = Max(mx, child.max_density);
          }
          node.min_density = mn;
          node.max_density = mx;

          parent_level.push_back(parent_index);
          nodes_.push_back(node);
        }
      }
    }

    level.swap(parent_level);
    for (int axis = 0; axis < 3; axis++) {
      dims[axis] = parent_dims[axis];
    }
    tree_height_++;
  }

  node_root_ = level.empty() ? -1 : level[0];
}

void Volume::rebuild_density_hierarchy()
{
  build_density_hierarchy();
}

void Volume::refit_after_write(int x, int y, int z)
{
  if (node_root_ < 0) {
    last_refit_nodes_ = 0;
    return;
  }

  // A voxel is part of the interpolation support of every leaf block whose
  // halo reaches it: at most the eight blocks adjacent to its owning block.
  // All of them, plus their ancestor chains, must be recomputed.
  std::vector<int> leaves;
  std::vector<int> touched;
  leaves.reserve(8);
  touched.reserve(64);

  auto mark_node = [&](int node_index) {
    for (size_t i = 0; i < touched.size(); i++) {
      if (touched[i] == node_index) {
        return;
      }
    }
    touched.push_back(node_index);
  };

  const int center_block[3] = {x / LEAF_BLOCK, y / LEAF_BLOCK, z / LEAF_BLOCK};
  for (int dz = -1; dz <= 0; dz++) {
    const int bz = center_block[2] + dz;
    if (bz < 0 || bz >= block_count_[2]) {
      continue;
    }
    for (int dy = -1; dy <= 0; dy++) {
      const int by = center_block[1] + dy;
      if (by < 0 || by >= block_count_[1]) {
        continue;
      }
      for (int dx = -1; dx <= 0; dx++) {
        const int bx = center_block[0] + dx;
        if (bx < 0 || bx >= block_count_[0]) {
          continue;
        }
        const TransferNode &candidate =
            nodes_[bz * block_count_[1] * block_count_[0] +
                    by * block_count_[0] + bx];
        if (candidate.block_min[0] - 1 <= x && x <= candidate.block_max[0] &&
            candidate.block_min[1] - 1 <= y && y <= candidate.block_max[1] &&
            candidate.block_min[2] - 1 <= z && z <= candidate.block_max[2]) {
          const int leaf_index =
              bz * block_count_[1] * block_count_[0] +
              by * block_count_[0] + bx;
          leaves.push_back(leaf_index);
          mark_node(leaf_index);
        }
      }
    }
  }

  // Recompute every affected leaf directly from its support.
  for (size_t i = 0; i < leaves.size(); i++) {
    TransferNode &leaf = nodes_[leaves[i]];
    Real mn = 0;
    Real mx = 0;
    for (int zz = leaf.block_min[2] - 1; zz <= leaf.block_max[2]; zz++) {
      for (int yy = leaf.block_min[1] - 1; yy <= leaf.block_max[1]; yy++) {
        for (int xx = leaf.block_min[0] - 1; xx <= leaf.block_max[0]; xx++) {
          const Real v = buffer_.GetValue(xx, yy, zz);
          mn = Min(mn, v);
          mx = Max(mx, v);
        }
      }
    }
    leaf.min_density = mn;
    leaf.max_density = mx;
  }

  // Mark every ancestor of every affected leaf.
  for (size_t i = 0; i < leaves.size(); i++) {
    int parent = nodes_[leaves[i]].parent;
    while (parent >= 0) {
      mark_node(parent);
      parent = nodes_[parent].parent;
    }
  }

  // Node indices increase bottom-up, so ascending index order is a valid
  // bottom-up order: every child is recomputed before its parent.
  std::sort(touched.begin(), touched.end());
  for (size_t i = 0; i < touched.size(); i++) {
    TransferNode &node = nodes_[touched[i]];
    if (node.num_children == 0) {
      continue;  // leaves were recomputed above
    }
    Real mn = 0;
    Real mx = 0;
    for (int c = 0; c < node.num_children; c++) {
      const TransferNode &child = nodes_[child_pool_[node.first_child + c]];
      mn = Min(mn, child.min_density);
      mx = Max(mx, child.max_density);
    }
    node.min_density = mn;
    node.max_density = mx;
  }

  last_refit_nodes_ = touched.size();
}

// One clipped piece of the ray-global parameter grid. The cell index makes
// every piece unique even where neighbouring node boxes share a boundary.
struct Volume::TransferPortion {
  long long cell;
  Real ta;
  Real tb;
};

void Volume::collect_node_portions(const TransferNode *node, const Ray &ray,
    Real t_enter, Real t_exit, Real root_enter, Real root_exit,
    Real grid_left, Real t_step,
    std::vector<TransferPortion> *portions,
    VolumeTransferStats *stats) const
{
  stats->nodes_visited++;

  if (!transfer_active_on_range(node->min_density, node->max_density)) {
    return;
  }

  if (node->num_children == 0) {
    // Enumerate the global grid cells overlapping this leaf. Cell geometry is
    // defined once at the root clip, so crossing node boundaries never splits
    // a cell, restarts the phase or samples a point twice.
    const long long k_first =
        static_cast<long long>(std::floor((t_enter - grid_left) / t_step));
    const long long k_last =
        static_cast<long long>(std::ceil((t_exit - grid_left) / t_step)) - 1;

    for (long long k = k_first; k <= k_last; k++) {
      const Real cell_a = grid_left + k * t_step;
      const Real cell_b = cell_a + t_step;
      // Clip the cell to the root interval, not this leaf: leaves tile the
      // box, so every leaf that reaches the cell produces the same portion.
      const Real ta = Max(cell_a, root_enter);
      const Real tb = Min(cell_b, root_exit);
      if (!(tb > ta)) {
        continue;
      }
      TransferPortion portion;
      portion.cell = k;
      portion.ta = ta;
      portion.tb = tb;
      portions->push_back(portion);
    }
    return;
  }

  // Internal node: recurse into the children actually crossed by the ray.
  const Resolution &res = buffer_.GetResolution();
  const Vector voxel_size(
      size_.x / res.x, size_.y / res.y, size_.z / res.z);

  for (int c = 0; c < node->num_children; c++) {
    const TransferNode &child = nodes_[child_pool_[node->first_child + c]];
    Box child_box;
    for (int axis = 0; axis < 3; axis++) {
      child_box.min[axis] = bounds_.min[axis] +
          child.block_min[axis] * voxel_size[axis];
      child_box.max[axis] = bounds_.min[axis] +
          child.block_max[axis] * voxel_size[axis];
    }

    Real child_tmin = 0;
    Real child_tmax = 0;
    if (!ray_box_slabs(child_box, ray.orig, ray.dir,
        t_enter, t_exit, &child_tmin, &child_tmax)) {
      continue;
    }
    child_tmin = Max(child_tmin, t_enter);
    child_tmax = Min(child_tmax, t_exit);
    if (!(child_tmax > child_tmin)) {
      continue;
    }

    collect_node_portions(&child, ray, child_tmin, child_tmax,
        root_enter, root_exit, grid_left, t_step, portions, stats);
  }
}

Color4 Volume::integrate_transfer(const Ray &ray, Real worldDirectionLength,
    Real worldStep, VolumeTransferStats *stats, bool shadow_only) const
{
  VolumeTransferStats local_stats;
  VolumeTransferStats &st = stats ? *stats : local_stats;
  st.nodes_visited = 0;
  st.density_samples = 0;

  if (!transfer_ready_ || node_root_ < 0) {
    return Color4();
  }
  if (!(worldDirectionLength > 0) || !(worldStep > 0)) {
    return Color4();
  }
  if (!(size_.x > 0 && size_.y > 0 && size_.z > 0)) {
    return Color4();
  }

  // Clip to the volume box first, then to the caller's ray range.
  Real root_enter = 0;
  Real root_exit = 0;
  if (!ray_box_slabs(bounds_, ray.orig, ray.dir,
      ray.tmin, ray.tmax, &root_enter, &root_exit)) {
    return Color4();
  }
  root_enter = Max(root_enter, ray.tmin);
  root_exit = Min(root_exit, ray.tmax);
  if (!(root_exit > root_enter)) {
    return Color4();
  }

  const Real t_step = worldStep / worldDirectionLength;
  if (!(t_step > 0)) {
    return Color4();
  }

  // The parameter grid is anchored at ray.tmin and shared by every node.
  const Real grid_left = ray.tmin +
      std::floor((root_enter - ray.tmin) / t_step) * t_step;

  std::vector<TransferPortion> portions;
  collect_node_portions(&nodes_[node_root_], ray, root_enter, root_exit,
      root_enter, root_exit, grid_left, t_step, &portions, &st);

  // Every grid cell belongs to exactly one leaf; sort by cell (ray order) and
  // drop any boundary duplicate so transmittance accumulates in ray order.
  std::sort(portions.begin(), portions.end(),
      [](const TransferPortion &a, const TransferPortion &b) {
        return a.cell < b.cell;
      });

  Real transmittance = 1;
  Color4 color;
  long long previous_cell = std::numeric_limits<long long>::min();

  for (size_t i = 0; i < portions.size(); i++) {
    const TransferPortion &portion = portions[i];
    if (portion.cell == previous_cell) {
      continue;
    }
    previous_cell = portion.cell;

    const Real t_mid = .5 * (portion.ta + portion.tb);
    const Vector point = RayPointAt(ray, t_mid);
    const Real density = sample_density_center(point);
    st.density_samples++;

    Real sigma = 0;
    Color emission;
    evaluate_transfer(density, &sigma, &emission);

    const Real ds = (portion.tb - portion.ta) * worldDirectionLength;
    Real portion_transmittance = 1;
    Real emission_factor = ds;
    if (sigma > 0) {
      portion_transmittance = std::exp(-sigma * ds);
      emission_factor = (1 - portion_transmittance) / sigma;
    }

    if (!shadow_only) {
      color.r += static_cast<float>(
          transmittance * Real(emission.r) * emission_factor);
      color.g += static_cast<float>(
          transmittance * Real(emission.g) * emission_factor);
      color.b += static_cast<float>(
          transmittance * Real(emission.b) * emission_factor);
    }
    transmittance *= portion_transmittance;
  }

  color.a = static_cast<float>(1 - transmittance);
  return color;
}

Color4 Volume::IntegrateTransfer(const Ray &ray, Real worldDirectionLength,
    Real worldStep, VolumeTransferStats *stats) const
{
  return integrate_transfer(ray, worldDirectionLength, worldStep, stats, false);
}

Color4 Volume::IntegrateTransferShadow(const Ray &ray, Real worldDirectionLength,
    Real worldStep, VolumeTransferStats *stats) const
{
  return integrate_transfer(ray, worldDirectionLength, worldStep, stats, true);
}

bool Volume::HasTransferFunction() const
{
  return transfer_ready_;
}

} // namespace xxx
