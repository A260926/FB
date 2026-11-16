// Copyright (c) 2011-2020 Hiroshi Tsubokawa
// See LICENSE and README

#ifndef FJ_VOLUME_H
#define FJ_VOLUME_H

#include "fj_compatibility.h"
#include "fj_vector.h"
#include "fj_types.h"
#include "fj_box.h"
#include "fj_color.h"
#include "fj_ray.h"
#include <cstddef>
#include <vector>

namespace fj {

class FJ_API Resolution {
public:
  Resolution() : x(0), y(0), z(0) {}
  Resolution(int xx, int yy, int zz) : x(xx), y(yy), z(zz) {}
  ~Resolution() {}

  int x, y, z;
};

class FJ_API VoxelBuffer {
public:
  VoxelBuffer();
  ~VoxelBuffer();

  void Resize(int xres, int yres, int zres);
  const Resolution &GetResolution() const;
  bool IsEmpty() const;

  void SetValue(int x, int y, int z, float value);
  float GetValue(int x, int y, int z) const;

private:
  std::vector<float> data_;
  Resolution res_;
};

class FJ_API VolumeSample {
public:
  VolumeSample() : density(0) {}
  ~VolumeSample() {}

  float density;
};

// One control point of the piecewise-linear transfer function.
// density is strictly increasing across knots; extinction and the RGB
// emission are non-negative.
class FJ_API TransferKnot {
public:
  TransferKnot() : density(0), extinction(0), emission() {}
  TransferKnot(Real density_value, Real extinction_value, const Color &emission_value)
    : density(density_value), extinction(extinction_value), emission(emission_value) {}
  ~TransferKnot() {}

  Real density;
  Real extinction;
  Color emission;
};

// Per-integration statistics for the transfer-function preview path.
class FJ_API VolumeTransferStats {
public:
  VolumeTransferStats() : nodes_visited(0), density_samples(0) {}
  ~VolumeTransferStats() {}

  size_t nodes_visited;
  size_t density_samples;
};

class FJ_API Volume {
public:
  Volume();
  ~Volume();

  void Resize(int xres, int yres, int zres);

  void SetBounds(const Box &bounds);
  const Box &GetBounds() const;

  void GetResolution(int *i, int *j, int *k) const;
  Real GetFilterSize() const;

  Vector IndexToPoint(int i, int j, int k) const;
  void PointToIndex(const Vector &point, int *i, int *j, int *k) const;

  void SetValue(int x, int y, int z, float value);
  float GetValue(int x, int y, int z) const;

  bool GetSample(const Vector &point, VolumeSample *sample) const;

  // Transfer-function CPU preview path.
  void SetTransferFunction(const std::vector<TransferKnot> &knots);
  size_t GetLastTransferRefitNodes() const;
  Color4 IntegrateTransfer(const Ray &ray, Real worldDirectionLength,
      Real worldStep, VolumeTransferStats *stats = NULL) const;
  // Shadow variant: accumulates extinction only, no emission.
  Color4 IntegrateTransferShadow(const Ray &ray, Real worldDirectionLength,
      Real worldStep, VolumeTransferStats *stats = NULL) const;
  bool HasTransferFunction() const;

public:
  void compute_filter_size();

  VoxelBuffer buffer_;
  Box bounds_;
  Vector size_;

  Real filtersize_;

private:
  struct TransferNode;

  void build_density_hierarchy();
  void rebuild_density_hierarchy();
  void refit_after_write(int x, int y, int z);
  bool transfer_active_on_range(Real dmin, Real dmax) const;
  void evaluate_transfer(Real density, Real *sigma, Color *emission) const;
  struct TransferPortion;

  Real sample_density_center(const Vector &point) const;
  Color4 integrate_transfer(const Ray &ray, Real worldDirectionLength,
      Real worldStep, VolumeTransferStats *stats, bool shadow_only) const;
  void collect_node_portions(const TransferNode *node, const Ray &ray,
      Real t_enter, Real t_exit, Real root_enter, Real root_exit,
      Real grid_left, Real t_step,
      std::vector<TransferPortion> *portions,
      VolumeTransferStats *stats) const;

  // Piecewise-linear transfer function knots, always sorted by density.
  std::vector<TransferKnot> transfer_knots_;
  bool transfer_ready_;

  // Sparse density hierarchy. One leaf covers up to four voxel cells per
  // axis; internal nodes have up to eight children.
  std::vector<TransferNode> nodes_;
  std::vector<int> child_pool_;
  int node_root_;
  int block_count_[3];
  int tree_height_;

  // Number of tree nodes recomputed by the most recent SetValue call.
  size_t last_refit_nodes_;
};

FJ_API void VolGetIndexRange(const Volume *volume,
    const Vector *center, double radius,
    int *xmin, int *ymin, int *zmin,
    int *xmax, int *ymax, int *zmax);

} // namespace xxx

#endif // FJ_XXX_H
