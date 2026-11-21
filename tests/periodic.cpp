/* Copyright (c) 2024 BSplineLib contributors

Periodic spline regression tests.  The reference implementations are
independent of the production code: periodic Cox-de Boor recursion on the
infinite knot column and, for rational splines, the homogeneous quotient
rule for derivatives. */

#include "task_support/check.h"

#include "BSplineLib/InputOutput/iges.hpp"
#include "BSplineLib/Splines/b_spline.hpp"
#include "BSplineLib/Splines/nurbs.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace bsplinelib;
using parameter_spaces::KnotVector;
using parameter_spaces::ParameterSpace;
using parameter_spaces::PeriodicAxis;
using parameter_spaces::PeriodicModes;
using parameter_spaces::Periodicity;
using splines::BSpline;
using splines::Nurbs;
using vector_spaces::VectorSpace;
using vector_spaces::WeightedVectorSpace;

namespace {

constexpr double kTol = 1e-9;

bool Close(double a, double b) {
  return std::abs(a - b) <= kTol * std::max(1.0, std::abs(b));
}

std::vector<double> MakeFiniteKnots(const std::vector<double>& fundamental,
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

// Independent periodic Cox-de Boor machinery on the infinite knot column.
struct ReferenceAxis {
  int degree;
  int m;
  double period_length;
  std::vector<double> fundamental;

  double Knot(int index) const {
    int local = index % m;
    if (local < 0)
      local += m;
    int block = (index - local) / m;
    return fundamental[local] + block * period_length;
  }

  double Map(double u) const {
    double const a = fundamental.front();
    double shifts = std::floor((u - a) / period_length);
    double mapped = u - shifts * period_length;
    if (mapped >= a + period_length)
      mapped -= period_length;
    if (mapped < a)
      mapped = a;
    return mapped;
  }

  double Basis(int index, int d, double u) const {
    return BasisDegree(index, d, u);
  }

  // Independent derivative recursion of the degree-p periodic basis.
  double Derivative(int index, int order, double u) const {
    return DerivativeRec(index, degree, order, u);
  }

  double DerivativeRec(int index,
                       int degree,
                       int order,
                       double u) const {
    if (order == 0)
      return BasisDegree(index, degree, u);
    double d1 = Knot(index + degree) - Knot(index);
    double d2 = Knot(index + degree + 1) - Knot(index + 1);
    double left = (d1 != 0.0)
                      ? static_cast<double>(degree) / d1
                            * DerivativeRec(index, degree - 1, order - 1, u)
                      : 0.0;
    double right = (d2 != 0.0)
                       ? static_cast<double>(degree) / d2
                             * DerivativeRec(index + 1, degree - 1,
                                             order - 1, u)
                       : 0.0;
    return left - right;
  }

  double BasisDegree(int index, int degree, double u) const {
    if (degree == 0)
      return (Knot(index) <= u && u < Knot(index + 1)) ? 1.0 : 0.0;
    double left = 0.0, right = 0.0;
    double d1 = Knot(index + degree) - Knot(index);
    if (d1 != 0.0)
      left = (u - Knot(index)) / d1 * BasisDegree(index, degree - 1, u);
    double d2 = Knot(index + degree + 1) - Knot(index + 1);
    if (d2 != 0.0)
      right = (Knot(index + degree + 1) - u) / d2
              * BasisDegree(index + 1, degree - 1, u);
    return left + right;
  }
};

// Rational periodic curve reference.  Coordinates are the fundamental m
// controls; weights are the fundamental weights.
struct ReferenceCurve {
  ReferenceAxis axis;
  std::vector<std::array<double, 3>> points;
  std::vector<double> weights;

  void Evaluate(double u,
                int order,
                std::array<double, 3>& out) const {
    double const uu = axis.Map(u);
    double A[3] = {}, dA[3] = {}, ddA[3] = {};
    double W = 0.0, dW = 0.0, ddW = 0.0;
    for (int r = -axis.degree; r < axis.m; ++r) {
      int slot = r % axis.m;
      if (slot < 0)
        slot += axis.m;
      double b[3];
      b[0] = axis.Basis(r, axis.degree, uu);
      b[1] = axis.Derivative(r, 1, uu);
      b[2] = axis.Derivative(r, 2, uu);
      double const w = weights[slot];
      for (int c = 0; c < 3; ++c) {
        A[c] += b[0] * points[slot][c] * w;
        dA[c] += b[1] * points[slot][c] * w;
        ddA[c] += b[2] * points[slot][c] * w;
      }
      W += b[0] * w;
      dW += b[1] * w;
      ddW += b[2] * w;
    }
    std::array<double, 3> P{A[0] / W, A[1] / W, A[2] / W};
    if (order == 0) {
      out = P;
      return;
    }
    std::array<double, 3> Pp{};
    for (int c = 0; c < 3; ++c)
      Pp[c] = (dA[c] * W - A[c] * dW) / (W * W);
    if (order == 1) {
      out = Pp;
      return;
    }
    for (int c = 0; c < 3; ++c)
      out[c] = ddA[c] / W - 2.0 * dW / W * Pp[c] - ddW / W * P[c];
  }
};

template<int para_dim>
typename ParameterSpace<para_dim>::KnotVectors_
MakeKnotVectors(const std::vector<std::vector<double>>& finite) {
  typename ParameterSpace<para_dim>::KnotVectors_ kv{};
  for (int d = 0; d < para_dim; ++d) {
    kv[d] = std::make_shared<KnotVector>(
        KnotVector::Knots_(finite[d].begin(), finite[d].end()));
  }
  return kv;
}

bool AllClose3(const double a[3], const std::array<double, 3>& b) {
  for (int c = 0; c < 3; ++c)
    if (!Close(a[c], b[c]))
      return false;
  return true;
}

std::string ReadFile(const std::string& name) {
  std::ifstream file(name);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// Concatenates the parameter-data content (columns 1..64) of all P-section
// lines and drops the trailing ';'.  This strips the IGES line counters and
// column padding so the numbers can be parsed independently of wrapping.
std::string ExtractPData(std::string const& text) {
  std::istringstream in(text);
  std::string line, data;
  while (std::getline(in, line)) {
    if (line.size() >= 73 && line[72] == 'P')
      data.append(line, 0, 64);
  }
  data.erase(std::remove_if(data.begin(),
                            data.end(),
                            [](char c) { return c == ' '; }),
             data.end());
  if (!data.empty() && data.back() == ';')
    data.pop_back();
  return data;
}

std::string Block(std::string const& raw, int width, bool align_right) {
  std::string pad(width - static_cast<int>(raw.size()), ' ');
  return align_right ? pad + raw : raw + pad;
}

// Minimal, self-contained IGES file for a 126 (curve) or 128 (surface)
// entity with the given parameter-data payload.  The periodic flags are
// supplied by the test, not by the production writer.
std::string SerializeMinimalIGES(std::string pdata, int entity_type) {
  std::vector<std::string> p_lines;
  while (!pdata.empty()) {
    int take = static_cast<int>(std::min<std::size_t>(64, pdata.size()));
    if (take < static_cast<int>(pdata.size())) {
      int last_comma = -1;
      for (int i = 0; i < take; ++i)
        if (pdata[i] == ',')
          last_comma = i;
      if (last_comma > 0)
        take = last_comma + 1;
    }
    p_lines.push_back(pdata.substr(0, take));
    pdata.erase(0, take);
  }
  std::string const type_string = std::to_string(entity_type);
  std::string out;
  out += Block("minimal periodic test file", 72, false) + "S      1\n";
  out += Block("1H,1H;", 72, false) + "G      1\n";
  // Directory entry: exactly 18 fields (144 characters), wrapped into two
  // 72-character D-section lines like the production writer.
  std::string directory;
  directory += Block(type_string, 8, true) + Block("1", 8, true);
  for (int i = 0; i < 6; ++i)
    directory += Block("0", 8, true);
  directory += "00000001";
  directory += Block(type_string, 8, true) + Block("0", 8, true)
               + Block("0", 8, true)
               + Block(std::to_string(p_lines.size()), 8, true);
  for (int i = 0; i < 3; ++i)
    directory += Block("0", 8, true);
  directory += Block("test", 8, true) + Block("0", 8, true);
  for (int line = 0; line < 2; ++line) {
    out += directory.substr(line * 72, 72) + "D"
           + Block(std::to_string(line + 1), 7, true) + "\n";
  }
  for (std::size_t i = 0; i < p_lines.size(); ++i) {
    out += Block(p_lines[i], 64, false)
           + Block(std::to_string(i + 1), 8, true) + "P"
           + Block(std::to_string(i + 1), 7, true) + "\n";
  }
  out += "S      1G      1D      2P"
        + Block(std::to_string(p_lines.size()), 7, true)
        + Block("", 40, false) + "T      1\n";
  return out;
}

// Splits a comma-separated numeric payload into doubles.
std::vector<double> ParseDoubles(std::string const& data) {
  std::vector<double> values;
  std::string token;
  for (char c : data) {
    if (c == ',') {
      if (!token.empty())
        values.push_back(std::stod(token));
      token.clear();
    } else {
      token.push_back(c);
    }
  }
  if (!token.empty())
    values.push_back(std::stod(token));
  return values;
}

} // namespace

// ---------------------------------------------------------------------------
// orbit group
// ---------------------------------------------------------------------------

static int OrbitCurveTranslationAndPadding() {
  const int p = 2, m = 4;
  const double L = 4.0;
  std::vector<double> fundamental{0.0, 1.0, 2.0, 3.0};
  auto finite = MakeFiniteKnots(fundamental, p, L);
  auto kv = MakeKnotVectors<1>({finite});
  ParameterSpace<1>::Degrees_ degrees{p};
  PeriodicModes<1> modes{Periodicity::Periodic};

  int const n = m + p;
  WeightedVectorSpace::Coordinates_ xyz(n, 3);
  WeightedVectorSpace::Weights_ weights(n);
  double base[4][3] = {{1, 0, 0}, {0, 1, .5}, {-1, 0, .2}, {0, -1, .4}};
  for (int i = 0; i < n; ++i) {
    for (int c = 0; c < 3; ++c)
      xyz(i, c) = base[i % 4][c];
    weights[i] = 1.0 + 0.13 * (i % 4); // non-equal weights
  }
  auto ps = std::make_shared<ParameterSpace<1>>(kv, degrees, modes);
  auto curve = std::make_shared<Nurbs<1>>(
      ps, std::make_shared<WeightedVectorSpace>(std::move(xyz), weights), modes);

  PeriodicAxis const& axis = ps->GetPeriodicAxis(0);
  // Real knot translation t(i + m) = t(i) + L over the finite storage.
  for (int i = -p; i < p; ++i) {
    TASK_CHECK(Close(axis.KnotAt(i + m), axis.KnotAt(i) + L));
  }
  TASK_CHECK(Close(axis.GetPeriodLength(), L));
  TASK_CHECK(Close(axis.GetDomainEnd(), L));

  // The finite knot storage has exactly m + 2p + 1 entries.
  TASK_CHECK(static_cast<int>(
                 ps->GetKnotVector(0)->GetKnots().size())
             == m + 2 * p + 1);

  // Homogeneous padding rows are exact periodic copies of the first p rows
  // (weighted coordinates and weights together, i.e. all 4 components).
  auto const& homogeneous =
      curve->GetWeightedVectorSpace()->GetCoordinates();
  TASK_CHECK(homogeneous.Shape()[0] == n);
  TASK_CHECK(homogeneous.Shape()[1] == 4);
  for (int j = 0; j < p; ++j)
    for (int c = 0; c < 4; ++c)
      TASK_CHECK(Close(homogeneous(j, c), homogeneous(m + j, c)));

  // Inserting the same orbit from three equivalent parameters raises the
  // fundamental multiplicity only up to the degree.
  curve->InsertKnot(0, 0.5);
  curve->InsertKnot(0, 0.5 + L);
  curve->InsertKnot(0, 0.5 - 2.0 * L);
  TASK_CHECK(axis.DetermineOrbitMultiplicity(0.5) == p);
  TASK_CHECK(axis.GetNumberOfFundamentalKnots() == m + 2);
  curve->InsertKnot(0, 0.5 + 7.0 * L); // clipped: multiplicity already p
  TASK_CHECK(axis.GetNumberOfFundamentalKnots() == m + 2);

  // Insertion at the seam itself must not be ignored.
  int const before_seam = axis.GetNumberOfFundamentalKnots();
  curve->InsertKnot(0, axis.GetDomainStart());
  TASK_CHECK(axis.GetNumberOfFundamentalKnots() == before_seam + 1);

  // Negative and multi-period evaluation stays periodic.
  for (double u : {-8.0, -4.0, 0.0, 4.0, 8.0, 12.0}) {
    double got[3];
    curve->Evaluate(&u, got);
    double canonical = axis.MapToDomain(u);
    double ref[3];
    curve->Evaluate(&canonical, ref);
    TASK_CHECK(AllClose3(got, {ref[0], ref[1], ref[2]}));
  }
  return 0;
}

static int OrbitSingleAxisSurface() {
  const int pu = 2, mu = 4;
  const double Lu = 6.0;
  std::vector<double> fu{0.0, 1.5, 3.0, 4.5};
  auto fku = MakeFiniteKnots(fu, pu, Lu);
  KnotVector::Knots_ fkv{0, 0, 0, 1, 2, 3, 3, 3}; // clamped V
  auto kv = MakeKnotVectors<2>({fku, std::vector<double>(fkv.begin(), fkv.end())});
  ParameterSpace<2>::Degrees_ degrees{pu, 2};
  PeriodicModes<2> modes{Periodicity::Periodic, Periodicity::NonPeriodic};

  int const nu = mu + pu, nv = 5;
  VectorSpace::Coordinates_ cp(nu * nv, 3);
  for (int j = 0; j < nv; ++j)
    for (int i = 0; i < nu; ++i) {
      double theta = 2.0 * M_PI * (i % mu) / mu;
      cp(j * nu + i, 0) = std::cos(theta);
      cp(j * nu + i, 1) = std::sin(theta);
      cp(j * nu + i, 2) = 0.5 * j;
    }
  auto ps = std::make_shared<ParameterSpace<2>>(kv, degrees, modes);
  auto surface = std::make_shared<BSpline<2>>(
      ps, std::make_shared<VectorSpace>(std::move(cp)), modes);

  // Geometry snapshots over negative and multi-period parameters.
  std::vector<std::pair<std::array<double, 2>, std::array<double, 3>>> samples;
  for (double u = -7.0; u < 13.0; u += 1.31)
    for (double v = 0.1; v < 2.9; v += 0.73) {
      double q[2] = {u, v}, o[3];
      surface->Evaluate(q, o);
      samples.push_back({{u, v}, {o[0], o[1], o[2]}});
    }
  auto unchanged = [&]() {
    for (auto const& [q, b] : samples) {
      double qq[2] = {q[0], q[1]}, o[3];
      surface->Evaluate(qq, o);
      for (int c = 0; c < 3; ++c)
        if (!Close(o[c], b[c]))
          return false;
    }
    return true;
  };

  surface->InsertKnot(0, 2.25);
  TASK_CHECK(unchanged());
  surface->InsertKnot(1, 1.5); // clamped axis keeps working
  TASK_CHECK(unchanged());
  TASK_CHECK(surface->IsPeriodic(0));
  TASK_CHECK(!surface->IsPeriodic(1));
  surface->InsertKnot(0, 0.0); // seam insertion on U
  TASK_CHECK(unchanged());
  // Periodic padding survives edits on the other axis.
  PeriodicAxis const& axis = ps->GetPeriodicAxis(0);
  TASK_CHECK(axis.GetNumberOfFundamentalKnots() == mu + 2);
  return 0;
}

static int OrbitDoubleAxisAlternating() {
  const int pu = 2, mu = 4, pv = 3, mv = 5;
  const double Lu = 6.0, Lv = 5.0;
  std::vector<double> fu{0.0, 1.5, 3.0, 4.5};
  std::vector<double> fv{0.0, 1.0, 2.0, 3.0, 4.0};
  auto kv = MakeKnotVectors<2>(
      {MakeFiniteKnots(fu, pu, Lu), MakeFiniteKnots(fv, pv, Lv)});
  ParameterSpace<2>::Degrees_ degrees{pu, pv};
  PeriodicModes<2> modes{Periodicity::Periodic, Periodicity::Periodic};

  int const nu = mu + pu, nv = mv + pv;
  VectorSpace::Coordinates_ cp(nu * nv, 3);
  for (int j = 0; j < nv; ++j)
    for (int i = 0; i < nu; ++i) {
      double theta = 2.0 * M_PI * (i % mu) / mu;
      double radius = 1.0 + 0.25 * (j % mv);
      cp(j * nu + i, 0) = radius * std::cos(theta);
      cp(j * nu + i, 1) = radius * std::sin(theta);
      cp(j * nu + i, 2) = 0.4 * (j % mv);
    }
  auto ps = std::make_shared<ParameterSpace<2>>(kv, degrees, modes);
  auto surface = std::make_shared<BSpline<2>>(
      ps, std::make_shared<VectorSpace>(std::move(cp)), modes);

  std::vector<std::pair<std::array<double, 2>, std::array<double, 3>>> samples;
  for (double u = -6.5; u < 12.5; u += 1.43)
    for (double v = -5.5; v < 10.5; v += 1.29) {
      double q[2] = {u, v}, o[3];
      surface->Evaluate(q, o);
      samples.push_back({{u, v}, {o[0], o[1], o[2]}});
    }
  auto unchanged = [&]() {
    for (auto const& [q, b] : samples) {
      double qq[2] = {q[0], q[1]}, o[3];
      surface->Evaluate(qq, o);
      for (int c = 0; c < 3; ++c)
        if (!Close(o[c], b[c]))
          return false;
    }
    return true;
  };

  // Alternating U/V insertion must never lose either periodic state.
  surface->InsertKnot(0, 2.25);
  TASK_CHECK(unchanged());
  surface->InsertKnot(1, 2.5);
  TASK_CHECK(unchanged());
  surface->InsertKnot(0, 3.75);
  TASK_CHECK(unchanged());
  surface->InsertKnot(1, 0.0);
  TASK_CHECK(unchanged());
  surface->InsertKnot(1, 2.5); // multiplicity 2
  TASK_CHECK(unchanged());
  TASK_CHECK(surface->IsPeriodic(0));
  TASK_CHECK(surface->IsPeriodic(1));
  TASK_CHECK(ps->GetPeriodicAxis(0).GetNumberOfFundamentalKnots() == mu + 2);
  TASK_CHECK(ps->GetPeriodicAxis(1).GetNumberOfFundamentalKnots() == mv + 3);
  return 0;
}

static int OrbitSeamRotationRoundTrip() {
  const int p = 3, m = 5;
  const double L = 2.5, a = 0.5;
  std::vector<double> fundamental{a, 0.9, 1.4, 2.1, 2.6};
  auto finite = MakeFiniteKnots(fundamental, p, L);
  auto kv = MakeKnotVectors<1>({finite});
  ParameterSpace<1>::Degrees_ degrees{p};
  PeriodicModes<1> modes{Periodicity::Periodic};
  int const n = m + p;
  VectorSpace::Coordinates_ cp(n, 3);
  for (int i = 0; i < n; ++i) {
    double theta = 2.0 * M_PI * (i % m) / m;
    cp(i, 0) = std::cos(theta);
    cp(i, 1) = std::sin(theta);
    cp(i, 2) = 0.2 * (i % m);
  }
  auto ps = std::make_shared<ParameterSpace<1>>(kv, degrees, modes);
  auto curve = std::make_shared<BSpline<1>>(
      ps, std::make_shared<VectorSpace>(std::move(cp)), modes);

  std::vector<std::pair<double, std::array<double, 3>>> samples;
  for (double u = a - 2 * L; u < a + 3 * L; u += 0.213) {
    double o[3];
    curve->Evaluate(&u, o);
    samples.push_back({u, {o[0], o[1], o[2]}});
  }
  auto unchanged = [&]() {
    for (auto const& [u, b] : samples) {
      double o[3];
      curve->Evaluate(&u, o);
      for (int c = 0; c < 3; ++c)
        if (!Close(o[c], b[c]))
          return false;
    }
    return true;
  };

  curve->RotatePeriodicSeam(0, 1.4);
  TASK_CHECK(Close(ps->GetPeriodicAxis(0).GetDomainStart(), 1.4));
  TASK_CHECK(unchanged());
  curve->InsertKnot(0, 1.9); // insert after moving the seam
  TASK_CHECK(unchanged());
  curve->RotatePeriodicSeam(0, 1.4 + 2.0 * L); // orbit-equivalent seam
  TASK_CHECK(unchanged());
  curve->RotatePeriodicSeam(0, a + 2.0 * L); // back to the original seam
  // The seam is the same orbit as a, though it may be represented as a + kL
  // inside the shifted canonical interval.
  double restored = ps->GetPeriodicAxis(0).GetDomainStart();
  double wrapped_restored =
      restored - L * std::floor((restored - a) / L);
  TASK_CHECK(Close(wrapped_restored, a));
  TASK_CHECK(unchanged());

  // Arbitrary parameters cannot move the seam.
  bool threw = false;
  try {
    curve->RotatePeriodicSeam(0, 1.77);
  } catch (std::invalid_argument const&) {
    threw = true;
  }
  TASK_CHECK(threw);
  TASK_CHECK(unchanged());
  return 0;
}

static int OrbitCopySeparationAndRejections() {
  const int p = 2, m = 4;
  const double L = 4.0;
  std::vector<double> fundamental{0.0, 1.0, 2.0, 3.0};
  auto kv = MakeKnotVectors<1>({MakeFiniteKnots(fundamental, p, L)});
  ParameterSpace<1>::Degrees_ degrees{p};
  PeriodicModes<1> modes{Periodicity::Periodic};
  int const n = m + p;
  VectorSpace::Coordinates_ cp(n, 3);
  for (int i = 0; i < n; ++i)
    for (int c = 0; c < 3; ++c)
      cp(i, c) = static_cast<double>(i % m) + 0.1 * c;
  auto ps = std::make_shared<ParameterSpace<1>>(kv, degrees, modes);
  auto curve = std::make_shared<BSpline<1>>(
      ps, std::make_shared<VectorSpace>(std::move(cp)), modes);

  auto clone = std::make_shared<BSpline<1>>(*curve);
  TASK_CHECK(clone->IsPeriodic(0));
  int const original_m = ps->GetPeriodicAxis(0).GetNumberOfFundamentalKnots();
  clone->InsertKnot(0, 0.5);
  // The original spline is untouched by the clone's edit.
  TASK_CHECK(ps->GetPeriodicAxis(0).GetNumberOfFundamentalKnots()
             == original_m);
  TASK_CHECK(clone->GetParameterSpace()->GetPeriodicAxis(0)
                 .GetNumberOfFundamentalKnots()
             == original_m + 1);

  // Copy assignment and move construction both preserve the periodic state.
  BSpline<1> assigned;
  assigned = *curve;
  TASK_CHECK(assigned.IsPeriodic(0));
  TASK_CHECK(assigned.GetParameterSpace()->GetPeriodicAxis(0)
                 .GetNumberOfFundamentalKnots()
             == original_m);
  auto moved_into = std::make_unique<BSpline<1>>(std::move(assigned));
  TASK_CHECK(moved_into->IsPeriodic(0));
  double u = 1.23, a[3], b[3];
  curve->Evaluate(&u, a);
  moved_into->Evaluate(&u, b);
  TASK_CHECK(AllClose3(b, {a[0], a[1], a[2]}));
  moved_into->InsertKnot(0, 0.25);
  TASK_CHECK(ps->GetPeriodicAxis(0).GetNumberOfFundamentalKnots()
             == original_m);

  // Unsupported modifications throw before changing the spline.
  auto knots_before = ps->GetKnotVector(0)->GetKnots();
  bool threw = false;
  try {
    curve->RemoveKnot(0, 1.0, 1e-9);
  } catch (std::logic_error const&) {
    threw = true;
  }
  TASK_CHECK(threw);
  threw = false;
  try {
    curve->ElevateDegree(0);
  } catch (std::logic_error const&) {
    threw = true;
  }
  TASK_CHECK(threw);
  threw = false;
  try {
    curve->ReduceDegree(0, 1e-9);
  } catch (std::logic_error const&) {
    threw = true;
  }
  TASK_CHECK(threw);
  auto knots_after = ps->GetKnotVector(0)->GetKnots();
  TASK_CHECK(knots_before == knots_after);
  return 0;
}

// ---------------------------------------------------------------------------
// derivatives group
// ---------------------------------------------------------------------------

static int DerivativesNonUniformRationalCurve() {
  const int p = 3, m = 6;
  const double L = 3.0;
  std::vector<double> fundamental{0.0, 0.4, 0.9, 1.6, 2.2, 2.7};
  ReferenceAxis reference{p, m, L, fundamental};
  auto kv = MakeKnotVectors<1>({MakeFiniteKnots(fundamental, p, L)});
  ParameterSpace<1>::Degrees_ degrees{p};
  PeriodicModes<1> modes{Periodicity::Periodic};
  int const n = m + p;
  WeightedVectorSpace::Coordinates_ xyz(n, 3);
  WeightedVectorSpace::Weights_ weights(n);
  ReferenceCurve ref_curve;
  ref_curve.axis = reference;
  ref_curve.points.resize(m);
  ref_curve.weights.resize(m);
  for (int i = 0; i < n; ++i) {
    int slot = i % m;
    double theta = 2.0 * M_PI * slot / m;
    double point[3] = {std::cos(theta), std::sin(theta), 0.3 * slot};
    for (int c = 0; c < 3; ++c)
      xyz(i, c) = point[c];
    weights[i] = 0.7 + 0.09 * slot;
    if (i < m) {
      ref_curve.points[slot] = {point[0], point[1], point[2]};
      ref_curve.weights[slot] = weights[i];
    }
  }
  auto ps = std::make_shared<ParameterSpace<1>>(kv, degrees, modes);
  auto curve = std::make_shared<Nurbs<1>>(
      ps, std::make_shared<WeightedVectorSpace>(std::move(xyz), weights), modes);

  for (double u = -2.0 * L; u < 3.0 * L; u += 0.173) {
    for (int order = 0; order <= 2; ++order) {
      ParameterSpace<1>::Derivative_ derivative{order};
      double got[3];
      curve->EvaluateDerivative(&u, derivative.data(), got);
      std::array<double, 3> expected;
      ref_curve.Evaluate(u, order, expected);
      TASK_CHECK(AllClose3(got, expected));
    }
  }
  // Exactly at the seam the a-side (right-hand) value is returned.
  double at_seam[3], just_inside[3];
  double seam = 0.0, inside = 1e-12;
  curve->Evaluate(&seam, at_seam);
  curve->Evaluate(&inside, just_inside);
  TASK_CHECK(AllClose3(at_seam, {just_inside[0], just_inside[1],
                                  just_inside[2]}));
  // A parameter at b maps to the a-side.
  double b_value = L, a_value = 0.0, at_b[3], at_a[3];
  curve->Evaluate(&b_value, at_b);
  curve->Evaluate(&a_value, at_a);
  TASK_CHECK(AllClose3(at_b, {at_a[0], at_a[1], at_a[2]}));
  return 0;
}

static int DerivativesDoublePeriodicSurface() {
  const int pu = 2, mu = 4, pv = 2, mv = 5;
  const double Lu = 4.0, Lv = 5.0;
  std::vector<double> fu{0.0, 1.0, 2.0, 3.0};
  std::vector<double> fv{0.0, 0.8, 2.1, 3.3, 4.2};
  ReferenceAxis ru{pu, mu, Lu, fu};
  ReferenceAxis rv{pv, mv, Lv, fv};
  auto kv = MakeKnotVectors<2>(
      {MakeFiniteKnots(fu, pu, Lu), MakeFiniteKnots(fv, pv, Lv)});
  ParameterSpace<2>::Degrees_ degrees{pu, pv};
  PeriodicModes<2> modes{Periodicity::Periodic, Periodicity::Periodic};
  int const nu = mu + pu, nv = mv + pv;
  VectorSpace::Coordinates_ cp(nu * nv, 3);
  auto point = [&](int i, int j) {
    double theta = 2.0 * M_PI * i / mu;
    double phi = 2.0 * M_PI * j / mv;
    return std::array<double, 3>{std::cos(theta) * (1.0 + 0.3 * std::cos(phi)),
                                 std::sin(theta) * (1.0 + 0.3 * std::cos(phi)),
                                 0.4 * std::sin(phi)};
  };
  for (int j = 0; j < nv; ++j)
    for (int i = 0; i < nu; ++i) {
      auto q = point(i % mu, j % mv);
      for (int c = 0; c < 3; ++c)
        cp(j * nu + i, c) = q[c];
    }
  auto ps = std::make_shared<ParameterSpace<2>>(kv, degrees, modes);
  auto surface = std::make_shared<BSpline<2>>(
      ps, std::make_shared<VectorSpace>(std::move(cp)), modes);

  // Non-rational tensor reference for value and selected derivatives.
  auto reference = [&](double u, double v, int du, int dv, double out[3]) {
    double uu = ru.Map(u), vv = rv.Map(v);
    for (int c = 0; c < 3; ++c)
      out[c] = 0.0;
    for (int ri = -pu; ri < mu; ++ri) {
      int si = ri % mu;
      if (si < 0)
        si += mu;
      double bu = (du == 0) ? ru.Basis(ri, pu, uu)
                            : ru.Derivative(ri, du, uu);
      for (int rj = -pv; rj < mv; ++rj) {
        int sj = rj % mv;
        if (sj < 0)
          sj += mv;
        double bv = (dv == 0) ? rv.Basis(rj, pv, vv)
                              : rv.Derivative(rj, dv, vv);
        auto q = point(si, sj);
        for (int c = 0; c < 3; ++c)
          out[c] += bu * bv * q[c];
      }
    }
  };

  for (double u = -Lu; u < 2.0 * Lu; u += 0.83)
    for (double v = 0.3; v < Lv - 0.3; v += 0.91) {
      struct Query {
        int du, dv;
      };
      Query queries[5] = {{0, 0}, {1, 0}, {0, 1}, {2, 0}, {1, 1}};
      for (auto query : queries) {
        ParameterSpace<2>::Derivative_ derivative{query.du, query.dv};
        double got[3], expected[3];
        double q[2] = {u, v};
        surface->EvaluateDerivative(q, derivative.data(), got);
        reference(u, v, query.du, query.dv, expected);
        TASK_CHECK(AllClose3(got, {expected[0], expected[1], expected[2]}));
      }
    }
  return 0;
}

static int DerivativesAcrossRepeatedKnots() {
  // A repeated fundamental knot (multiplicity 2, degree 3) exercises the
  // right-hand side convention at an internal kink.
  const int p = 3, m = 6;
  const double L = 4.0;
  std::vector<double> fundamental{0.0, 1.0, 1.0, 2.0, 3.0, 3.5};
  auto kv = MakeKnotVectors<1>({MakeFiniteKnots(fundamental, p, L)});
  ParameterSpace<1>::Degrees_ degrees{p};
  PeriodicModes<1> modes{Periodicity::Periodic};
  int const n = m + p;
  VectorSpace::Coordinates_ cp(n, 2);
  for (int i = 0; i < n; ++i) {
    cp(i, 0) = static_cast<double>(i % m);
    cp(i, 1) = (i % m) * (i % m);
  }
  auto ps = std::make_shared<ParameterSpace<1>>(kv, degrees, modes);
  auto curve = std::make_shared<BSpline<1>>(
      ps, std::make_shared<VectorSpace>(std::move(cp)), modes);

  ReferenceAxis axis{p, m, L, fundamental};
  // One-sided first derivative just right of the repeated knot u = 1.
  double eps = 1e-7;
  double u = 1.0 + eps;
  double got[2];
  ParameterSpace<1>::Derivative_ d1{1};
  curve->EvaluateDerivative(&u, d1.data(), got);
  double expected[2] = {};
  for (int r = -p; r < m; ++r) {
    int slot = r % m;
    if (slot < 0)
      slot += m;
    double basis_derivative = axis.Derivative(r, 1, u);
    expected[0] += basis_derivative * static_cast<double>(slot);
    expected[1] += basis_derivative * static_cast<double>(slot * slot);
  }
  TASK_CHECK(Close(got[0], expected[0]));
  TASK_CHECK(Close(got[1], expected[1]));

  // The callable interface agrees with Evaluate on periodic parameters.
  double call_input = 1.0 + 1e-7;
  auto via_call = curve->operator()(&call_input);
  double via_eval[2];
  curve->Evaluate(&call_input, via_eval);
  TASK_CHECK(Close(via_call[0], via_eval[0]));
  TASK_CHECK(Close(via_call[1], via_eval[1]));

  // Total derivative order above 2 is rejected.
  bool threw = false;
  try {
    ParameterSpace<1>::Derivative_ too_high{3};
    double discard[2];
    curve->EvaluateDerivative(&call_input, too_high.data(), discard);
  } catch (std::invalid_argument const&) {
    threw = true;
  }
  TASK_CHECK(threw);
  return 0;
}

// ---------------------------------------------------------------------------
// roundtrip group
// ---------------------------------------------------------------------------

static std::shared_ptr<Nurbs<1>> MakePeriodicCurve() {
  const int p = 2, m = 4;
  const double L = 4.0;
  std::vector<double> fundamental{0.0, 1.0, 2.0, 3.0};
  auto kv = MakeKnotVectors<1>({MakeFiniteKnots(fundamental, p, L)});
  ParameterSpace<1>::Degrees_ degrees{p};
  PeriodicModes<1> modes{Periodicity::Periodic};
  int const n = m + p;
  WeightedVectorSpace::Coordinates_ xyz(n, 3);
  WeightedVectorSpace::Weights_ weights(n);
  double base[4][3] = {{1, 0, 0}, {0, 1, .5}, {-1, 0, .2}, {0, -1, .4}};
  for (int i = 0; i < n; ++i) {
    for (int c = 0; c < 3; ++c)
      xyz(i, c) = base[i % 4][c];
    weights[i] = 1.0 + 0.11 * (i % 4);
  }
  auto ps = std::make_shared<ParameterSpace<1>>(kv, degrees, modes);
  return std::make_shared<Nurbs<1>>(
      ps, std::make_shared<WeightedVectorSpace>(std::move(xyz), weights), modes);
}

static int RoundtripCurveFlagsRangesAndEditing() {
  auto curve = MakePeriodicCurve();
  std::string const file = ".task-build/periodic_curve.igs";
  input_output::iges::Write(input_output::operations::Splines{curve}, file,
                            Precision{17});

  // Independently inspect the written parameter bytes for the explicit
  // periodic trailer instead of trusting the library's own reader.
  std::string text = ReadFile(file);
  std::vector<double> pdata = ParseDoubles(ExtractPData(text));
  TASK_CHECK(pdata.size() >= 5);
  int const tail = static_cast<int>(pdata.size()) - 5;
  TASK_CHECK(Close(pdata[tail], 918273.0));     // magic
  TASK_CHECK(Close(pdata[tail + 1], 1.0));      // version
  TASK_CHECK(Close(pdata[tail + 2], 1.0));      // periodic flag
  TASK_CHECK(Close(pdata[tail + 3], 0.0));      // range start
  TASK_CHECK(Close(pdata[tail + 4], 4.0));      // range end

  auto loaded = input_output::iges::Read(file);
  TASK_CHECK(loaded.size() == 1);
  auto copy = std::dynamic_pointer_cast<Nurbs<1>>(loaded[0]);
  TASK_CHECK(copy != nullptr);
  TASK_CHECK(copy->IsPeriodic(0));
  auto ranges = copy->GetParameterSpace()->GetDomainRanges();
  TASK_CHECK(Close(ranges[0][0], 0.0));
  TASK_CHECK(Close(ranges[0][1], 4.0));

  // Geometry agrees at negative and multi-period parameters.
  for (double u = -8.3; u < 12.0; u += 0.57) {
    double a[3], b[3];
    curve->Evaluate(&u, a);
    copy->Evaluate(&u, b);
    TASK_CHECK(AllClose3(b, {a[0], a[1], a[2]}));
  }
  // The reloaded spline remains editable as a genuine periodic spline.
  copy->InsertKnot(0, 0.5);
  TASK_CHECK(copy->GetParameterSpace()->GetPeriodicAxis(0)
                 .GetNumberOfFundamentalKnots()
             == 5);
  copy->RotatePeriodicSeam(0, 2.0);
  TASK_CHECK(Close(copy->GetParameterSpace()->GetPeriodicAxis(0)
                       .GetDomainStart(),
                   2.0));

  // After moving the seam, the new canonical ranges persist through a
  // further write/read cycle.
  std::string const rotated_file = ".task-build/periodic_rotated.igs";
  input_output::iges::Write(
      input_output::operations::Splines{copy}, rotated_file, Precision{17});
  auto rotated_loaded = input_output::iges::Read(rotated_file);
  auto rotated_curve =
      std::dynamic_pointer_cast<Nurbs<1>>(rotated_loaded[0]);
  TASK_CHECK(rotated_curve != nullptr);
  TASK_CHECK(rotated_curve->IsPeriodic(0));
  auto rotated_ranges =
      rotated_curve->GetParameterSpace()->GetDomainRanges();
  TASK_CHECK(Close(rotated_ranges[0][0], 2.0));
  TASK_CHECK(Close(rotated_ranges[0][1], 6.0));
  // Geometry at an original real parameter is still the same.
  for (double u = -4.3; u < 8.0; u += 1.13) {
    double a[3], b[3];
    curve->Evaluate(&u, a);
    rotated_curve->Evaluate(&u, b);
    TASK_CHECK(AllClose3(b, {a[0], a[1], a[2]}));
  }
  return 0;
}

static int RoundtripExplicitlyConstructedInput() {
  // Use the library only to obtain a standard, flag-less 126 parameter
  // payload; the periodic flag and ranges are then added by the test's own
  // serializer so the periodic semantics do not come from the production
  // writer.
  auto curve = MakePeriodicCurve();
  std::string const source_file = ".task-build/periodic_source.igs";
  input_output::iges::Write(
      input_output::operations::Splines{curve}, source_file, Precision{17});
  std::string pdata = ExtractPData(ReadFile(source_file));

  // Remove any production trailer from the reused standard body.
  std::size_t trailer_pos = pdata.find(",918273");
  if (trailer_pos != std::string::npos)
    pdata.erase(trailer_pos);

  // Flag-less input: even though the geometry is closed, no periodic flag
  // means the spline must stay non-periodic.
  std::string const ordinary_file = ".task-build/periodic_ordinary.igs";
  std::ofstream(ordinary_file)
      << SerializeMinimalIGES(pdata + ";", 126);
  auto ordinary_loaded = input_output::iges::Read(ordinary_file);
  auto ordinary_curve =
      std::dynamic_pointer_cast<Nurbs<1>>(ordinary_loaded[0]);
  TASK_CHECK(ordinary_curve != nullptr);
  TASK_CHECK(!ordinary_curve->IsPeriodic(0));

  // Explicitly periodic input: flag 1 and canonical range [0, 4), built by
  // the test itself rather than the production writer.
  std::string const injected_file = ".task-build/periodic_injected.igs";
  std::ofstream(injected_file)
      << SerializeMinimalIGES(pdata + ",918273,1,1,0,4;", 126);
  auto injected_loaded = input_output::iges::Read(injected_file);
  auto injected_curve =
      std::dynamic_pointer_cast<Nurbs<1>>(injected_loaded[0]);
  TASK_CHECK(injected_curve != nullptr);
  TASK_CHECK(injected_curve->IsPeriodic(0));
  auto ranges = injected_curve->GetParameterSpace()->GetDomainRanges();
  TASK_CHECK(Close(ranges[0][0], 0.0));
  TASK_CHECK(Close(ranges[0][1], 4.0));

  // The trailer round-trips: the written file still carries the flag.
  std::string const rewrite_file = ".task-build/periodic_rewrite.igs";
  input_output::iges::Write(
      input_output::operations::Splines{injected_curve}, rewrite_file,
      Precision{17});
  std::vector<double> rewritten = ParseDoubles(ExtractPData(ReadFile(rewrite_file)));
  int const tail = static_cast<int>(rewritten.size()) - 5;
  TASK_CHECK(Close(rewritten[tail], 918273.0));
  TASK_CHECK(Close(rewritten[tail + 2], 1.0));

  // Editing the explicitly-flagged input works as a periodic spline.
  injected_curve->InsertKnot(0, 1.5);
  TASK_CHECK(injected_curve->GetParameterSpace()->GetPeriodicAxis(0)
                 .GetNumberOfFundamentalKnots()
             == 5);
  injected_curve->RotatePeriodicSeam(0, 2.0);
  TASK_CHECK(Close(injected_curve->GetParameterSpace()->GetPeriodicAxis(0)
                       .GetDomainStart(),
                   2.0));
  return 0;
}

static int RoundtripDoublePeriodicSurface() {
  const int pu = 2, mu = 4, pv = 3, mv = 5;
  const double Lu = 6.0, Lv = 5.0;
  std::vector<double> fu{0.0, 1.5, 3.0, 4.5};
  std::vector<double> fv{0.0, 1.0, 2.0, 3.0, 4.0};
  auto kv = MakeKnotVectors<2>(
      {MakeFiniteKnots(fu, pu, Lu), MakeFiniteKnots(fv, pv, Lv)});
  ParameterSpace<2>::Degrees_ degrees{pu, pv};
  PeriodicModes<2> modes{Periodicity::Periodic, Periodicity::Periodic};
  int const nu = mu + pu, nv = mv + pv;
  VectorSpace::Coordinates_ cp(nu * nv, 3);
  for (int j = 0; j < nv; ++j)
    for (int i = 0; i < nu; ++i) {
      double theta = 2.0 * M_PI * (i % mu) / mu;
      cp(j * nu + i, 0) = std::cos(theta);
      cp(j * nu + i, 1) = std::sin(theta);
      cp(j * nu + i, 2) = 0.3 * (j % mv);
    }
  auto ps = std::make_shared<ParameterSpace<2>>(kv, degrees, modes);
  auto surface = std::make_shared<BSpline<2>>(
      ps, std::make_shared<VectorSpace>(std::move(cp)), modes);

  std::string const file = ".task-build/periodic_surface.igs";
  input_output::iges::Write(
      input_output::operations::Splines{surface}, file, Precision{17});
  auto loaded = input_output::iges::Read(file);
  TASK_CHECK(loaded.size() == 1);
  auto copy = std::dynamic_pointer_cast<BSpline<2>>(loaded[0]);
  TASK_CHECK(copy != nullptr);
  TASK_CHECK(copy->IsPeriodic(0));
  TASK_CHECK(copy->IsPeriodic(1));
  auto ranges = copy->GetParameterSpace()->GetDomainRanges();
  TASK_CHECK(Close(ranges[0][0], 0.0));
  TASK_CHECK(Close(ranges[0][1], Lu));
  TASK_CHECK(Close(ranges[1][0], 0.0));
  TASK_CHECK(Close(ranges[1][1], Lv));

  for (double u = -6.0; u < 12.0; u += 1.7)
    for (double v = -5.0; v < 10.0; v += 1.9) {
      double q[2] = {u, v}, a[3], b[3];
      surface->Evaluate(q, a);
      copy->Evaluate(q, b);
      TASK_CHECK(AllClose3(b, {a[0], a[1], a[2]}));
    }
  // Alternating edits continue to work after the round trip.
  copy->InsertKnot(0, 2.25);
  copy->InsertKnot(1, 2.5);
  TASK_CHECK(copy->IsPeriodic(0));
  TASK_CHECK(copy->IsPeriodic(1));
  return 0;
}

static int RoundtripNonPeriodicClosedStaysNonPeriodic() {
  // A clamped curve whose first and last controls coincide looks closed but
  // carries no periodic flag; the IGES path must not promote it.
  KnotVector::Knots_ knots{0, 0, 0, 1, 2, 3, 3, 3};
  auto kv = MakeKnotVectors<1>({std::vector<double>(knots.begin(), knots.end())});
  ParameterSpace<1>::Degrees_ degrees{2};
  VectorSpace::Coordinates_ cp(5, 3);
  double points[5][3] = {{1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0},
                         {1, 0, 0}}; // last equals first
  for (int i = 0; i < 5; ++i)
    for (int c = 0; c < 3; ++c)
      cp(i, c) = points[i][c];
  auto ps = std::make_shared<ParameterSpace<1>>(kv, degrees);
  auto curve = std::make_shared<BSpline<1>>(
      ps, std::make_shared<VectorSpace>(std::move(cp)));
  TASK_CHECK(!curve->IsPeriodic(0));

  std::string const file = ".task-build/closed_nonperiodic.igs";
  input_output::iges::Write(
      input_output::operations::Splines{curve}, file, Precision{17});
  TASK_CHECK(ReadFile(file).find("918273") == std::string::npos);
  auto loaded = input_output::iges::Read(file);
  auto copy = std::dynamic_pointer_cast<BSpline<1>>(loaded[0]);
  TASK_CHECK(copy != nullptr);
  TASK_CHECK(!copy->IsPeriodic(0));
  return 0;
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------

static int OrbitConstructionValidationAndMultiplicity() {
  const int p = 2, m = 4;
  const double L = 4.0;
  std::vector<double> fundamental{0.0, 1.0, 2.0, 3.0};
  auto finite = MakeFiniteKnots(fundamental, p, L);

  // m must exceed the degree: a fundamental multiset with m == p is
  // rejected by the parameter space.
  {
    std::vector<double> short_fund{0.0, 1.0};
    auto short_finite = MakeFiniteKnots(short_fund, p, L);
    auto bad_kv = MakeKnotVectors<1>({short_finite});
    bool threw = false;
    try {
      ParameterSpace<1> bad(bad_kv, ParameterSpace<1>::Degrees_{p},
                            PeriodicModes<1>{Periodicity::Periodic});
    } catch (std::domain_error const&) {
      threw = true;
    }
    TASK_CHECK(threw);
  }

  // A finite knot storage that does not obey t(i+m)=t(i)+L is rejected.
  {
    auto broken = finite;
    broken[p + 1] += 0.5; // break the periodic extension
    auto bad_kv = MakeKnotVectors<1>({broken});
    bool threw = false;
    try {
      ParameterSpace<1> bad(bad_kv, ParameterSpace<1>::Degrees_{p},
                            PeriodicModes<1>{Periodicity::Periodic});
    } catch (std::domain_error const&) {
      threw = true;
    }
    TASK_CHECK(threw);
  }

  // Non-periodic control padding is rejected by the spline constructor.
  {
    auto kv = MakeKnotVectors<1>({finite});
    int const n = m + p;
    VectorSpace::Coordinates_ cp(n, 3);
    for (int i = 0; i < n; ++i)
      for (int c = 0; c < 3; ++c)
        cp(i, c) = static_cast<double>(i); // last p rows are not copies
    auto ps = std::make_shared<ParameterSpace<1>>(
        kv, ParameterSpace<1>::Degrees_{p},
        PeriodicModes<1>{Periodicity::Periodic});
    bool threw = false;
    try {
      BSpline<1> bad(ps, std::make_shared<VectorSpace>(std::move(cp)),
                     PeriodicModes<1>{Periodicity::Periodic});
    } catch (std::domain_error const&) {
      threw = true;
    }
    TASK_CHECK(threw);
  }

  // Non-positive weights are rejected by the NURBS constructor.
  {
    auto kv = MakeKnotVectors<1>({finite});
    int const n = m + p;
    WeightedVectorSpace::Coordinates_ xyz(n, 3);
    WeightedVectorSpace::Weights_ weights(n);
    for (int i = 0; i < n; ++i)
      weights[i] = 1.0;
    for (int i = 0; i < n; ++i)
      for (int c = 0; c < 3; ++c)
        xyz(i, c) = static_cast<double>(i % m);
    weights[2] = 0.0; // non-positive weight
    auto ps = std::make_shared<ParameterSpace<1>>(
        kv, ParameterSpace<1>::Degrees_{p},
        PeriodicModes<1>{Periodicity::Periodic});
    bool threw = false;
    try {
      Nurbs<1> bad(ps,
                   std::make_shared<WeightedVectorSpace>(std::move(xyz),
                                                         weights),
                   PeriodicModes<1>{Periodicity::Periodic});
    } catch (std::domain_error const&) {
      threw = true;
    }
    TASK_CHECK(threw);
  }

  // A single multiplicity-2 call inserts the orbit twice and preserves the
  // geometry; a further call is clipped at fundamental multiplicity p.
  {
    auto kv = MakeKnotVectors<1>({finite});
    int const n = m + p;
    VectorSpace::Coordinates_ cp(n, 3);
    for (int i = 0; i < n; ++i) {
      double theta = 2.0 * M_PI * (i % m) / m;
      cp(i, 0) = std::cos(theta);
      cp(i, 1) = std::sin(theta);
      cp(i, 2) = 0.2 * (i % m);
    }
    auto ps = std::make_shared<ParameterSpace<1>>(
        kv, ParameterSpace<1>::Degrees_{p},
        PeriodicModes<1>{Periodicity::Periodic});
    auto curve = std::make_shared<BSpline<1>>(
        ps, std::make_shared<VectorSpace>(std::move(cp)),
        PeriodicModes<1>{Periodicity::Periodic});

    std::vector<std::pair<double, std::array<double, 3>>> samples;
    for (double u = -4.0; u < 8.0; u += 0.31) {
      double o[3];
      curve->Evaluate(&u, o);
      samples.push_back({u, {o[0], o[1], o[2]}});
    }
    curve->InsertKnot(0, 1.5, Multiplicity{2});
    TASK_CHECK(ps->GetPeriodicAxis(0).GetNumberOfFundamentalKnots()
               == m + 2);
    TASK_CHECK(ps->GetPeriodicAxis(0).DetermineOrbitMultiplicity(1.5) == 2);
    for (auto const& [u, b] : samples) {
      double o[3];
      curve->Evaluate(&u, o);
      TASK_CHECK(AllClose3(o, b));
    }
    curve->InsertKnot(0, 1.5, Multiplicity{2}); // clipped: cap is p = 2
    TASK_CHECK(ps->GetPeriodicAxis(0).GetNumberOfFundamentalKnots()
               == m + 2);
  }
  return 0;
}

const struct task_case task_cases[] = {
    {"orbit", "curve_translation_padding", OrbitCurveTranslationAndPadding},
    {"orbit", "single_axis_surface", OrbitSingleAxisSurface},
    {"orbit", "double_axis_alternating", OrbitDoubleAxisAlternating},
    {"orbit", "seam_rotation_roundtrip", OrbitSeamRotationRoundTrip},
    {"orbit", "copy_separation_rejections", OrbitCopySeparationAndRejections},
    {"orbit", "construction_validation",
     OrbitConstructionValidationAndMultiplicity},
    {"derivatives", "nonuniform_rational_curve",
     DerivativesNonUniformRationalCurve},
    {"derivatives", "double_periodic_surface", DerivativesDoublePeriodicSurface},
    {"derivatives", "repeated_knot_right_side", DerivativesAcrossRepeatedKnots},
    {"roundtrip", "curve_flags_ranges_editing",
     RoundtripCurveFlagsRangesAndEditing},
    {"roundtrip", "explicitly_constructed_input",
     RoundtripExplicitlyConstructedInput},
    {"roundtrip", "double_periodic_surface", RoundtripDoublePeriodicSurface},
    {"roundtrip", "closed_nonperiodic_stays",
     RoundtripNonPeriodicClosedStaysNonPeriodic},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
