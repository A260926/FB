/* Copyright (c) 2024 BSplineLib contributors

Example: build a genuinely periodic NURBS surface, insert a knot near the
seam, move the parameter seam, and write a closed display mesh as OBJ.  The
mesh wraps around the periodic first parameter by sharing vertex indices at
the seam rather than duplicating positions. */

#include "BSplineLib/Splines/nurbs.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>

using namespace bsplinelib;
using parameter_spaces::KnotVector;
using parameter_spaces::ParameterSpace;
using parameter_spaces::PeriodicModes;
using parameter_spaces::Periodicity;
using splines::Nurbs;
using vector_spaces::WeightedVectorSpace;

namespace {

std::vector<double> FiniteKnots(const std::vector<double>& fundamental,
                                int degree,
                                double period_length) {
  int const m = static_cast<int>(fundamental.size());
  std::vector<double> finite;
  for (int i = -degree; i <= m + degree; ++i) {
    int local = i % m;
    if (local < 0)
      local += m;
    int block = (i - local) / m;
    finite.push_back(fundamental[local] + block * period_length);
  }
  return finite;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <output.obj>\n";
    return 2;
  }
  std::string const output_name{argv[1]};

  // Periodic in U (degree 2, period 2*pi), periodic in V (degree 3, period
  // 2*pi): a rational torus.
  constexpr int kDegreeU = 2, kDegreeV = 3;
  constexpr int kFundU = 6, kFundV = 8;
  constexpr double kPeriodU = 2.0 * M_PI;
  constexpr double kPeriodV = 2.0 * M_PI;
  constexpr double kMajor = 2.0, kMinor = 0.7;

  std::vector<double> fund_u, fund_v;
  for (int i = 0; i < kFundU; ++i)
    fund_u.push_back(kPeriodU * i / kFundU);
  for (int j = 0; j < kFundV; ++j)
    fund_v.push_back(kPeriodV * j / kFundV);

  std::vector<double> finite_u =
      FiniteKnots(fund_u, kDegreeU, kPeriodU);
  std::vector<double> finite_v =
      FiniteKnots(fund_v, kDegreeV, kPeriodV);
  ParameterSpace<2>::KnotVectors_ knot_vectors{
      std::make_shared<KnotVector>(
          KnotVector::Knots_(finite_u.begin(), finite_u.end())),
      std::make_shared<KnotVector>(
          KnotVector::Knots_(finite_v.begin(), finite_v.end()))};
  ParameterSpace<2>::Degrees_ degrees{kDegreeU, kDegreeV};
  PeriodicModes<2> modes{Periodicity::Periodic, Periodicity::Periodic};

  int const n_u = kFundU + kDegreeU;
  int const n_v = kFundV + kDegreeV;
  WeightedVectorSpace::Coordinates_ coordinates(n_u * n_v, 3);
  WeightedVectorSpace::Weights_ weights(n_u * n_v);
  for (int j = 0; j < n_v; ++j) {
    double phi = 2.0 * M_PI * (j % kFundV) / kFundV;
    for (int i = 0; i < n_u; ++i) {
      double theta = 2.0 * M_PI * (i % kFundU) / kFundU;
      double weight = 1.0 + 0.15 * std::cos(phi); // non-equal weights
      int const flat = j * n_u + i;
      coordinates(flat, 0) =
          (kMajor + kMinor * std::cos(phi)) * std::cos(theta) * weight;
      coordinates(flat, 1) =
          (kMajor + kMinor * std::cos(phi)) * std::sin(theta) * weight;
      coordinates(flat, 2) = kMinor * std::sin(phi) * weight;
      weights[flat] = weight;
    }
  }

  auto parameter_space =
      std::make_shared<ParameterSpace<2>>(knot_vectors, degrees, modes);
  auto surface = std::make_shared<Nurbs<2>>(
      parameter_space,
      std::make_shared<WeightedVectorSpace>(std::move(coordinates),
                                            std::move(weights)),
      modes);

  // Edit the periodic surface: insert near the U seam, then move the seam.
  surface->InsertKnot(0, fund_u[1] * 0.5);
  surface->RotatePeriodicSeam(0, fund_u[2]);

  // Sample a closed grid.  The grid's last U column reuses the first
  // column's vertex indices, giving a watertight seam.
  constexpr int kSegmentsU = 32, kSegmentsV = 16;
  double const u_start =
      parameter_space->GetPeriodicAxis(0).GetDomainStart();
  double const v_start =
      parameter_space->GetPeriodicAxis(1).GetDomainStart();

  auto vertex_index = [](int i, int j) {
    // i ranges over kSegmentsU columns with shared wrap indices.
    return j * kSegmentsU + i + 1; // OBJ indices are 1-based
  };

  std::ofstream obj(output_name);
  if (!obj) {
    std::cerr << "cannot open " << output_name << " for writing\n";
    return 1;
  }
  obj << "# periodic NURBS surface display mesh\n";

  // Only kSegmentsV rows are emitted: both the U wrap (column i = 0) and
  // the V wrap (row j = 0) reuse vertex indices, so the mesh is watertight
  // without duplicate seam vertices.
  for (int j = 0; j < kSegmentsV; ++j) {
    double const v = v_start + kPeriodV * j / kSegmentsV;
    for (int i = 0; i < kSegmentsU; ++i) {
      double const u = u_start + kPeriodU * i / kSegmentsU;
      double parameters[2] = {u, v};
      double point[3];
      surface->Evaluate(parameters, point);
      obj << "v " << point[0] << ' ' << point[1] << ' ' << point[2]
          << '\n';
    }
  }
  if (!obj) {
    std::cerr << "write failure while emitting vertices\n";
    return 1;
  }

  for (int j = 0; j < kSegmentsV; ++j) {
    int const next_j = (j + 1) % kSegmentsV; // shared V seam indices
    for (int i = 0; i < kSegmentsU; ++i) {
      int next_i = (i + 1) % kSegmentsU;     // shared U seam indices
      int a = vertex_index(i, j);
      int b = vertex_index(next_i, j);
      int c = vertex_index(next_i, next_j);
      int d = vertex_index(i, next_j);
      obj << "f " << a << ' ' << b << ' ' << c << ' ' << d << '\n';
    }
  }
  obj.flush();
  if (!obj) {
    std::cerr << "write failure while emitting faces\n";
    return 1;
  }

  std::cout << "wrote " << kSegmentsU * kSegmentsV
            << " vertices and " << kSegmentsU * kSegmentsV
            << " faces to " << output_name << "\n";
  return 0;
}
