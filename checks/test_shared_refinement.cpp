// Tests for the shared refinement mode of ParameterSpace<para_dim>.
//
// Short usage example:
//
//   using ParameterSpace = bsplinelib::parameter_spaces::ParameterSpace<2>;
//   auto parameter_space =
//       std::make_shared<ParameterSpace>(knot_vectors, degrees);
//   parameter_space->EnableSharedRefinement();  // before creating members
//   bsplinelib::splines::BSpline<2> surface{parameter_space, vector_space};
//   bsplinelib::splines::Nurbs<2> patch{parameter_space, weighted_vector_space};
//   surface.InsertKnot(0, 0.5);      // both control nets are refined
//   patch.ElevateDegree(1);          // both degrees are elevated
//   // surface(u, v) / patch(u, v) keep evaluating the original shapes.

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

#include "BSplineLib/ParameterSpaces/parameter_space.hpp"
#include "BSplineLib/Splines/b_spline.hpp"
#include "BSplineLib/Splines/nurbs.hpp"
#include "BSplineLib/VectorSpaces/vector_space.hpp"
#include "BSplineLib/VectorSpaces/weighted_vector_space.hpp"

#include "task_support/check.h"

namespace {

using bsplinelib::parameter_spaces::KnotVector;
using bsplinelib::parameter_spaces::ParameterSpace;
using bsplinelib::splines::BSpline;
using bsplinelib::splines::Nurbs;
using bsplinelib::vector_spaces::VectorSpace;
using bsplinelib::vector_spaces::WeightedVectorSpace;

using ParameterSpace1 = ParameterSpace<1>;
using ParameterSpace2 = ParameterSpace<2>;
using BSpline1 = BSpline<1>;
using BSpline2 = BSpline<2>;
using Nurbs2 = Nurbs<2>;

constexpr double kRelativeTolerance{1e-9};

bool Close(double const& a, double const& b) {
  return std::abs(a - b)
         <= kRelativeTolerance
                * std::max(1.0, std::max(std::abs(a), std::abs(b)));
}

std::shared_ptr<ParameterSpace2> MakeSurfaceParameterSpace() {
  auto u = std::make_shared<KnotVector>(std::vector<double>{0., 0., 1., 1.});
  auto v = std::make_shared<KnotVector>(std::vector<double>{0., 0., 1., 1.});
  return std::make_shared<ParameterSpace2>(
      ParameterSpace2::KnotVectors_{u, v},
      ParameterSpace2::Degrees_{1, 1});
}

std::shared_ptr<VectorSpace>
MakeNet(const std::vector<std::array<double, 3>>& points) {
  VectorSpace::Coordinates_ coordinates(static_cast<int>(points.size()), 3);
  int i{};
  for (const auto& point : points) {
    for (int d{}; d < 3; ++d) {
      coordinates(i, d) = point[d];
    }
    ++i;
  }
  return std::make_shared<VectorSpace>(std::move(coordinates));
}

std::vector<std::array<double, 3>> MakePoints(int const& n) {
  std::vector<std::array<double, 3>> points;
  points.reserve(n);
  for (int i{}; i < n; ++i) {
    points.push_back({0.31 * i + 0.07 * (i % 3),
                      0.17 * i - 0.11 * (i % 4),
                      0.13 * (i % 5) + 0.03 * i});
  }
  return points;
}

std::shared_ptr<VectorSpace> MakeNetOfSize(int const& n) {
  return MakeNet(MakePoints(n));
}

std::shared_ptr<WeightedVectorSpace>
MakeWeightedNet(const std::vector<std::array<double, 3>>& points,
                const std::vector<double>& weights) {
  VectorSpace::Coordinates_ coordinates(static_cast<int>(points.size()), 3);
  int i{};
  for (const auto& point : points) {
    for (int d{}; d < 3; ++d) {
      coordinates(i, d) = point[d];
    }
    ++i;
  }
  WeightedVectorSpace::Weights_ w(static_cast<int>(weights.size()));
  for (int j{}; j < static_cast<int>(weights.size()); ++j) {
    w[j] = weights[j];
  }
  return std::make_shared<WeightedVectorSpace>(coordinates, w);
}

std::shared_ptr<WeightedVectorSpace> MakeWeightedNetOfSize(int const& n) {
  std::vector<double> weights;
  weights.reserve(n);
  for (int i{}; i < n; ++i) {
    weights.push_back(0.5 + 0.25 * (i % 4));
  }
  return MakeWeightedNet(MakePoints(n), weights);
}

struct SurfaceSnapshot {
  std::vector<std::array<double, 3>> positions, du, dv, duv;
};

template<typename SplineType>
SurfaceSnapshot
SnapshotSurface(const SplineType& spline,
                const std::vector<std::array<double, 2>>& points) {
  SurfaceSnapshot snapshot;
  const std::array<int, 2> du{1, 0}, dv{0, 1}, duv{1, 1};
  for (const auto& uv : points) {
    std::array<double, 3> value;
    spline.Evaluate(uv.data(), value.data());
    snapshot.positions.push_back(value);
    const auto du_value = spline(uv.data(), du.data());
    const auto dv_value = spline(uv.data(), dv.data());
    const auto duv_value = spline(uv.data(), duv.data());
    snapshot.du.push_back({du_value[0], du_value[1], du_value[2]});
    snapshot.dv.push_back({dv_value[0], dv_value[1], dv_value[2]});
    snapshot.duv.push_back({duv_value[0], duv_value[1], duv_value[2]});
  }
  return snapshot;
}

template<typename SplineType>
bool MatchesSurface(const SplineType& spline,
                    const std::vector<std::array<double, 2>>& points,
                    const SurfaceSnapshot& snapshot) {
  const SurfaceSnapshot current = SnapshotSurface(spline, points);
  for (size_t i{}; i < points.size(); ++i) {
    for (int d{}; d < 3; ++d) {
      if (!Close(current.positions[i][d], snapshot.positions[i][d]))
        return false;
      if (!Close(current.du[i][d], snapshot.du[i][d])) return false;
      if (!Close(current.dv[i][d], snapshot.dv[i][d])) return false;
      if (!Close(current.duv[i][d], snapshot.duv[i][d])) return false;
    }
  }
  return true;
}

struct CurveSnapshot {
  std::vector<std::array<double, 3>> positions, d1;
};

template<typename SplineType>
CurveSnapshot
SnapshotCurve(const SplineType& spline,
              const std::vector<std::array<double, 1>>& points) {
  CurveSnapshot snapshot;
  const std::array<int, 1> d1{1};
  for (const auto& u : points) {
    std::array<double, 3> value;
    spline.Evaluate(u.data(), value.data());
    snapshot.positions.push_back(value);
    const auto d1_value = spline(u.data(), d1.data());
    snapshot.d1.push_back({d1_value[0], d1_value[1], d1_value[2]});
  }
  return snapshot;
}

template<typename SplineType>
bool MatchesCurve(const SplineType& spline,
                  const std::vector<std::array<double, 1>>& points,
                  const CurveSnapshot& snapshot) {
  const CurveSnapshot current = SnapshotCurve(spline, points);
  for (size_t i{}; i < points.size(); ++i) {
    for (int d{}; d < 3; ++d) {
      if (!Close(current.positions[i][d], snapshot.positions[i][d]))
        return false;
      if (!Close(current.d1[i][d], snapshot.d1[i][d])) return false;
    }
  }
  return true;
}

// A group of two bilinear, non-planar B-spline surfaces and one rational
// patch with non-unit weights is refined through the existing editing entry
// points of alternating members.  After every single edit, all members share
// the same knots and degrees, all control nets have matching sizes, and
// positions as well as first and mixed partial derivatives are unchanged.
int JointEditsPreserveGeometry() {
  auto p = MakeSurfaceParameterSpace();
  TASK_CHECK(!p->IsSharedRefinementEnabled());
  p->EnableSharedRefinement();
  TASK_CHECK(p->IsSharedRefinementEnabled());
  p->EnableSharedRefinement();  // enabling again is harmless
  TASK_CHECK(p->IsSharedRefinementEnabled());

  auto net_a = MakeNet({{0.0, 0.0, 0.0},
                        {1.0, 0.0, 0.8},
                        {0.0, 1.0, 0.3},
                        {1.0, 1.0, 1.2}});
  auto net_b = MakeNet({{0.2, 0.1, 0.5},
                        {1.4, 0.3, 0.1},
                        {0.4, 1.3, 1.0},
                        {1.1, 0.9, 0.4}});
  auto net_c = MakeWeightedNet(
      {{0.1, 0.4, 0.2}, {1.2, 0.5, 0.9}, {0.3, 1.2, 0.7}, {0.8, 1.4, 0.3}},
      {1.0, 0.5, 2.0, 1.5});

  BSpline2 a(p, net_a), b(p, net_b);
  Nurbs2 c(p, net_c);

  auto* const knot_vector_u = p->GetKnotVector(0).get();
  auto* const knot_vector_v = p->GetKnotVector(1).get();

  const std::vector<std::array<double, 2>> points{
      {0.13, 0.31}, {0.47, 0.83}, {0.71, 0.17}, {0.33, 0.61}, {0.9, 0.45}};
  const auto snapshot_a = SnapshotSurface(a, points);
  const auto snapshot_b = SnapshotSurface(b, points);
  const auto snapshot_c = SnapshotSurface(c, points);

  auto state_ok = [&](int expected_total) {
    return p->GetTotalNumberOfBasisFunctions() == expected_total
           && net_a->GetNumberOfCoordinates() == expected_total
           && net_b->GetNumberOfCoordinates() == expected_total
           && net_c->GetNumberOfCoordinates() == expected_total
           && MatchesSurface(a, points, snapshot_a)
           && MatchesSurface(b, points, snapshot_b)
           && MatchesSurface(c, points, snapshot_c);
  };

  TASK_CHECK(state_ok(4));
  a.InsertKnot(0, 0.25);
  TASK_CHECK(state_ok(6));
  c.RefineKnots(1, {0.4, 0.7});
  TASK_CHECK(state_ok(12));
  b.ElevateDegree(0);
  TASK_CHECK(state_ok(20));
  a.ElevateDegree(1);
  TASK_CHECK(state_ok(35));
  c.InsertKnot(0, 0.5);
  TASK_CHECK(state_ok(42));
  c.ElevateDegree(0);  // inserts and re-removes a knot for the Bezier segments
  TASK_CHECK(state_ok(63));
  a.RefineKnots(1, {0.4, 0.55});  // 0.4 is already at full multiplicity
  TASK_CHECK(state_ok(81));
  c.InsertKnot(0, 0.25);  // full multiplicity: one more insertion is allowed
  TASK_CHECK(state_ok(90));
  b.InsertKnot(0, 0.25);  // beyond the insertable multiplicity: clipped no-op
  TASK_CHECK(state_ok(90));
  a.InsertKnot(1, 0.55, 2);
  TASK_CHECK(state_ok(110));
  a.InsertKnot(0, 0.0);  // knots exactly on the bounds are ignored
  TASK_CHECK(state_ok(110));
  c.InsertKnot(1, 1.0);
  TASK_CHECK(state_ok(110));

  // knots, degrees, and multiplicities are shared and as expected
  TASK_CHECK(p->GetDegree(0) == 3);
  TASK_CHECK(p->GetDegree(1) == 2);
  TASK_CHECK(p->GetKnotVector(0)->GetKnots()
             == (std::vector<double>{0,
                                     0,
                                     0,
                                     0,
                                     0.25,
                                     0.25,
                                     0.25,
                                     0.25,
                                     0.5,
                                     0.5,
                                     1,
                                     1,
                                     1,
                                     1}));
  TASK_CHECK(p->GetKnotVector(1)->GetKnots()
             == (std::vector<double>{0,
                                     0,
                                     0,
                                     0.4,
                                     0.4,
                                     0.4,
                                     0.55,
                                     0.55,
                                     0.55,
                                     0.7,
                                     0.7,
                                     1,
                                     1,
                                     1}));
  const auto multiplicities = p->KnotMultiplicities();
  TASK_CHECK(multiplicities[0] == (std::vector<int>{4, 4, 2, 4}));
  TASK_CHECK(multiplicities[1] == (std::vector<int>{3, 3, 3, 2, 3}));

  // object identity of the parameter space and its knot vectors is preserved
  TASK_CHECK(p->GetKnotVector(0).get() == knot_vector_u);
  TASK_CHECK(p->GetKnotVector(1).get() == knot_vector_v);
  return 0;
}

// RemoveKnot, CoarsenKnots, and ReduceDegree throw std::logic_error before
// modifying anything while shared refinement is enabled; degree elevation
// still completes the internal knot removal that restores continuity.
int RemovalOperationsThrow() {
  auto p = MakeSurfaceParameterSpace();
  p->EnableSharedRefinement();
  auto net_a = MakeNetOfSize(4);
  auto net_b = MakeNetOfSize(4);
  auto net_c = MakeWeightedNet(
      {{0.0, 0.0, 0.1}, {1.0, 0.2, 0.4}, {0.3, 1.1, 0.8}, {1.2, 1.0, 0.2}},
      {1.0, 1.5, 0.75, 2.0});
  BSpline2 a(p, net_a), b(p, net_b);
  Nurbs2 c(p, net_c);
  a.InsertKnot(0, 0.5);
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 6);

  bool threw = false;
  try {
    a.RemoveKnot(0, 0.5, bsplinelib::splines::kEpsilon);
  } catch (const std::logic_error&) {
    threw = true;
  }
  TASK_CHECK(threw);

  threw = false;
  try {
    a.CoarsenKnots(0, {0.5}, bsplinelib::splines::kEpsilon);
  } catch (const std::logic_error&) {
    threw = true;
  }
  TASK_CHECK(threw);

  threw = false;
  try {
    a.ReduceDegree(0, bsplinelib::splines::kEpsilon);
  } catch (const std::logic_error&) {
    threw = true;
  }
  TASK_CHECK(threw);

  threw = false;
  try {
    c.RemoveKnot(0, 0.5, bsplinelib::splines::kEpsilon);
  } catch (const std::logic_error&) {
    threw = true;
  }
  TASK_CHECK(threw);

  threw = false;
  try {
    c.ReduceDegree(0, bsplinelib::splines::kEpsilon);
  } catch (const std::logic_error&) {
    threw = true;
  }
  TASK_CHECK(threw);

  // nothing was modified by the rejected operations
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 6);
  TASK_CHECK(net_a->GetNumberOfCoordinates() == 6);
  TASK_CHECK(net_b->GetNumberOfCoordinates() == 6);
  TASK_CHECK(net_c->GetNumberOfCoordinates() == 6);
  TASK_CHECK(p->GetKnotVector(0)->DetermineMultiplicity(0.5) == 1);
  TASK_CHECK(p->GetDegree(0) == 1);

  // degree elevation still works, including its continuity-restoring
  // internal knot removal
  b.ElevateDegree(0);
  TASK_CHECK(p->GetDegree(0) == 2);
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 10);
  TASK_CHECK(net_a->GetNumberOfCoordinates() == 10);
  TASK_CHECK(net_b->GetNumberOfCoordinates() == 10);
  TASK_CHECK(net_c->GetNumberOfCoordinates() == 10);
  TASK_CHECK(p->GetKnotVector(0)->DetermineMultiplicity(0.5) == 2);
  return 0;
}

// Invalid dimensions and out-of-domain knots keep raising the existing
// exceptions, and failed edits leave the whole group untouched.
int InvalidEditsKeepExistingExceptions() {
  auto p = MakeSurfaceParameterSpace();
  p->EnableSharedRefinement();
  auto net_a = MakeNetOfSize(4);
  auto net_b = MakeNetOfSize(4);
  BSpline2 a(p, net_a), b(p, net_b);

  bool threw = false;
  try {
    a.InsertKnot(2, 0.5);  // dimension out of range
  } catch (const std::out_of_range&) {
    threw = true;
  }
  TASK_CHECK(threw);

  threw = false;
  try {
    a.InsertKnot(0, 1.5);  // knot outside the domain
  } catch (const std::domain_error&) {
    threw = true;
  }
  TASK_CHECK(threw);

  threw = false;
  try {
    b.ElevateDegree(-1);  // dimension out of range
  } catch (const std::out_of_range&) {
    threw = true;
  }
  TASK_CHECK(threw);

  // the group is untouched
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 4);
  TASK_CHECK(net_a->GetNumberOfCoordinates() == 4);
  TASK_CHECK(net_b->GetNumberOfCoordinates() == 4);
  TASK_CHECK(p->GetDegree(0) == 1);
  return 0;
}

// Shared refinement also works for curves (para_dim == 1).
int CurvesShareRefinement() {
  auto u = std::make_shared<KnotVector>(std::vector<double>{0., 0., 1., 1.});
  auto p = std::make_shared<ParameterSpace1>(ParameterSpace1::KnotVectors_{u},
                                             ParameterSpace1::Degrees_{1});
  TASK_CHECK(!p->IsSharedRefinementEnabled());
  p->EnableSharedRefinement();
  TASK_CHECK(p->IsSharedRefinementEnabled());

  auto net_a = MakeNet({{0.0, 0.0, 0.0}, {1.0, 1.0, 0.5}});
  auto net_b = MakeNet({{0.5, 0.0, 1.0}, {1.5, 0.5, 0.0}});
  BSpline1 a(p, net_a), b(p, net_b);

  const std::vector<std::array<double, 1>> points{{0.15}, {0.55}, {0.9}};
  const auto snapshot_a = SnapshotCurve(a, points);
  const auto snapshot_b = SnapshotCurve(b, points);

  auto state_ok = [&](int expected_total) {
    return p->GetTotalNumberOfBasisFunctions() == expected_total
           && net_a->GetNumberOfCoordinates() == expected_total
           && net_b->GetNumberOfCoordinates() == expected_total
           && MatchesCurve(a, points, snapshot_a)
           && MatchesCurve(b, points, snapshot_b);
  };

  TASK_CHECK(state_ok(2));
  a.InsertKnot(0, 0.4);
  TASK_CHECK(state_ok(3));
  b.ElevateDegree(0);
  TASK_CHECK(state_ok(5));
  a.RefineKnots(0, {0.4, 0.8});
  TASK_CHECK(state_ok(7));
  TASK_CHECK(p->GetDegree(0) == 2);
  return 0;
}

// Several wrappers referencing the same control net - via the constructor or
// as a Nurbs' internal homogeneous BSpline - refine that net only once.
int SharedNetRefinedOnce() {
  auto p = MakeSurfaceParameterSpace();
  p->EnableSharedRefinement();
  auto net = MakeNetOfSize(4);
  BSpline2 a(p, net), b(p, net);  // two wrappers, one control net
  auto weighted_net = MakeWeightedNet(
      {{0.1, 0.2, 0.3}, {1.1, 0.4, 0.2}, {0.2, 1.2, 0.6}, {1.3, 1.1, 0.5}},
      {1.0, 2.0, 0.5, 1.25});
  Nurbs2 c(p, weighted_net);  // internal homogeneous BSpline subscribes once

  a.InsertKnot(0, 0.5);
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 6);
  TASK_CHECK(net->GetNumberOfCoordinates() == 6);  // not refined twice
  TASK_CHECK(weighted_net->GetNumberOfCoordinates() == 6);

  b.InsertKnot(1, 0.5);
  TASK_CHECK(net->GetNumberOfCoordinates() == 9);
  TASK_CHECK(weighted_net->GetNumberOfCoordinates() == 9);
  return 0;
}

// ShareMembers leaves the old group (the old net is unsubscribed) and joins
// the source object's group.
int ShareMembersRegroups() {
  auto p1 = MakeSurfaceParameterSpace();
  p1->EnableSharedRefinement();
  auto p2 = MakeSurfaceParameterSpace();
  p2->EnableSharedRefinement();

  auto net1 = MakeNetOfSize(4);
  auto net2 = MakeNetOfSize(4);
  auto net3 = MakeNetOfSize(4);
  auto wnet1 = MakeWeightedNet(
      {{0.0, 0.1, 0.2}, {1.0, 0.3, 0.5}, {0.4, 1.0, 0.7}, {1.2, 1.2, 0.1}},
      {1.0, 1.5, 0.5, 2.0});
  auto wnet2 = MakeWeightedNet(
      {{0.2, 0.0, 0.4}, {0.9, 0.5, 0.1}, {0.1, 0.9, 0.3}, {1.1, 1.0, 0.6}},
      {0.75, 1.25, 1.0, 1.5});

  auto a1 = std::make_shared<BSpline2>(p1, net1);
  auto a2 = std::make_shared<BSpline2>(p2, net2);
  auto n1 = std::make_shared<Nurbs2>(p1, wnet1);
  auto n2 = std::make_shared<Nurbs2>(p2, wnet2);

  a2->ShareMembers(a1);  // leaves p2's group, shares net1 on p1
  n2->ShareMembers(n1);  // leaves p2's group, shares wnet1 on p1

  a1->InsertKnot(0, 0.5);
  TASK_CHECK(net1->GetNumberOfCoordinates() == 6);  // refined once
  TASK_CHECK(wnet1->GetNumberOfCoordinates() == 6);
  TASK_CHECK(net2->GetNumberOfCoordinates() == 4);  // old nets left behind
  TASK_CHECK(wnet2->GetNumberOfCoordinates() == 4);

  // p2's group no longer references the old nets
  BSpline2 a3(p2, net3);
  a3.InsertKnot(0, 0.7);
  TASK_CHECK(net3->GetNumberOfCoordinates() == 6);
  TASK_CHECK(net2->GetNumberOfCoordinates() == 4);
  TASK_CHECK(wnet2->GetNumberOfCoordinates() == 4);

  // the regrouped wrappers keep participating in p1's group
  n1->ElevateDegree(1);
  TASK_CHECK(net1->GetNumberOfCoordinates() == 9);
  TASK_CHECK(wnet1->GetNumberOfCoordinates() == 9);
  TASK_CHECK(net3->GetNumberOfCoordinates() == 6);
  return 0;
}

// A control net leaves the group as soon as its last wrapper is destroyed,
// even if the caller still holds the control net shared_ptr.
int LastWrapperDeparture() {
  auto p = MakeSurfaceParameterSpace();
  p->EnableSharedRefinement();
  auto anchor_net = MakeNetOfSize(4);
  BSpline2 anchor(p, anchor_net);
  auto net = MakeNetOfSize(4);
  {
    auto first = std::make_unique<BSpline2>(p, net);
    auto second = std::make_unique<BSpline2>(p, net);
    first.reset();  // one wrapper remains: the net stays subscribed
    anchor.InsertKnot(0, 0.3);
    TASK_CHECK(net->GetNumberOfCoordinates() == 6);
    second.reset();  // last wrapper gone: the net leaves the group
  }
  anchor.InsertKnot(0, 0.7);
  TASK_CHECK(anchor_net->GetNumberOfCoordinates() == 8);
  TASK_CHECK(net->GetNumberOfCoordinates() == 6);  // not refined anymore
  return 0;
}

// Copies are independent and their parameter spaces do not share refinement;
// moved-from memberships continue with the moved-to object.
int CopyAndMove() {
  auto p = MakeSurfaceParameterSpace();
  p->EnableSharedRefinement();
  auto net1 = MakeNet({{0.0, 0.0, 0.2},
                       {1.0, 0.1, 0.9},
                       {0.2, 1.0, 0.5},
                       {1.1, 1.2, 0.3}});
  auto net2 = MakeNetOfSize(4);
  BSpline2 a(p, net1);
  const std::vector<std::array<double, 2>> points{{0.2, 0.4}, {0.6, 0.8}};
  const auto snapshot_a = SnapshotSurface(a, points);

  BSpline2 copied(a);  // independent copy, shared refinement off
  BSpline2 assigned;
  assigned = a;  // copy assignment is independent as well
  BSpline2 b(p, net2);
  BSpline2 moved(std::move(b));  // move: keeps participating

  a.InsertKnot(0, 0.5);
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 6);
  TASK_CHECK(net1->GetNumberOfCoordinates() == 6);
  TASK_CHECK(net2->GetNumberOfCoordinates() == 6);  // moved participates
  TASK_CHECK(MatchesSurface(copied, points, snapshot_a));
  TASK_CHECK(MatchesSurface(assigned, points, snapshot_a));

  copied.InsertKnot(0, 0.25);
  assigned.InsertKnot(1, 0.25);
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 6);  // group untouched
  TASK_CHECK(net1->GetNumberOfCoordinates() == 6);
  TASK_CHECK(net2->GetNumberOfCoordinates() == 6);

  moved.InsertKnot(1, 0.5);  // the moved-to object can initiate group edits
  TASK_CHECK(net1->GetNumberOfCoordinates() == 9);
  TASK_CHECK(net2->GetNumberOfCoordinates() == 9);
  TASK_CHECK(MatchesSurface(a, points, snapshot_a));

  BSpline2 move_assigned;
  move_assigned = std::move(moved);  // move assignment keeps participating
  a.InsertKnot(0, 0.1);
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 12);
  TASK_CHECK(net1->GetNumberOfCoordinates() == 12);
  TASK_CHECK(net2->GetNumberOfCoordinates() == 12);

  // Nurbs members: copies are independent, moved ones keep participating
  auto wnet = MakeWeightedNetOfSize(12);
  Nurbs2 n1(p, wnet);
  Nurbs2 n1_copied(n1);
  Nurbs2 n1_moved(std::move(n1));
  move_assigned.InsertKnot(1, 0.1);  // the move-assigned object can initiate
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 16);
  TASK_CHECK(wnet->GetNumberOfCoordinates() == 16);
  TASK_CHECK(net1->GetNumberOfCoordinates() == 16);
  n1_copied.InsertKnot(0, 0.35);
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 16);  // group untouched
  TASK_CHECK(wnet->GetNumberOfCoordinates() == 16);
  return 0;
}

// After a member leaves the group, the remaining members keep refining
// together while the departed control net stays frozen.
int RefinementAfterDeparture() {
  auto p = MakeSurfaceParameterSpace();
  p->EnableSharedRefinement();
  auto net1 = MakeNetOfSize(4);
  auto net2 = MakeNetOfSize(4);
  auto net3 = MakeNetOfSize(4);
  auto s1 = std::make_unique<BSpline2>(p, net1);
  auto s2 = std::make_unique<BSpline2>(p, net2);
  auto s3 = std::make_unique<BSpline2>(p, net3);

  const std::vector<std::array<double, 2>> points{{0.15, 0.35}, {0.65, 0.85}};
  s1->InsertKnot(0, 0.2);
  s2->ElevateDegree(1);
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 9);
  TASK_CHECK(net1->GetNumberOfCoordinates() == 9);
  const auto snapshot2 = SnapshotSurface(*s2, points);
  const auto snapshot3 = SnapshotSurface(*s3, points);

  s1.reset();  // net1 leaves the group

  s2->InsertKnot(0, 0.6);
  s3->ElevateDegree(0);
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == 21);
  TASK_CHECK(net2->GetNumberOfCoordinates() == 21);
  TASK_CHECK(net3->GetNumberOfCoordinates() == 21);
  TASK_CHECK(net1->GetNumberOfCoordinates() == 9);  // frozen at departure
  TASK_CHECK(MatchesSurface(*s2, points, snapshot2));
  TASK_CHECK(MatchesSurface(*s3, points, snapshot3));
  return 0;
}

// Groups form only per parameter space object, never by knot values; a copy
// made mid-sequence keeps its own state.
int IndependentGroupsAndCopies() {
  auto p1 = MakeSurfaceParameterSpace();
  p1->EnableSharedRefinement();
  auto p2 = MakeSurfaceParameterSpace();  // same knot values, other object
  p2->EnableSharedRefinement();

  auto net1 = MakeNet({{0.1, 0.0, 0.3},
                       {1.2, 0.2, 0.6},
                       {0.0, 0.9, 0.8},
                       {0.9, 1.1, 0.1}});
  auto net2 = MakeNetOfSize(4);
  BSpline2 a(p1, net1);
  BSpline2 b(p2, net2);

  const std::vector<std::array<double, 2>> points{{0.25, 0.75}};
  const auto snapshot_a = SnapshotSurface(a, points);

  a.InsertKnot(0, 0.5);
  TASK_CHECK(net2->GetNumberOfCoordinates() == 4);  // groups are not merged
  TASK_CHECK(p2->GetTotalNumberOfBasisFunctions() == 4);

  b.RefineKnots(1, {0.3, 0.6});
  TASK_CHECK(net2->GetNumberOfCoordinates() == 8);
  TASK_CHECK(net1->GetNumberOfCoordinates() == 6);  // p1's group untouched
  TASK_CHECK(p1->GetTotalNumberOfBasisFunctions() == 6);

  // a copy made mid-sequence stays frozen at its own state
  BSpline2 frozen(a);
  a.RefineKnots(1, {0.25, 0.75});
  TASK_CHECK(net1->GetNumberOfCoordinates() == 12);
  TASK_CHECK(MatchesSurface(frozen, points, snapshot_a));
  TASK_CHECK(MatchesSurface(a, points, snapshot_a));
  return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"shared_geometry", "joint_edits_preserve_geometry",
     JointEditsPreserveGeometry},
    {"shared_geometry", "removal_operations_throw", RemovalOperationsThrow},
    {"shared_geometry", "invalid_edits_keep_existing_exceptions",
     InvalidEditsKeepExistingExceptions},
    {"shared_geometry", "curves_share_refinement", CurvesShareRefinement},
    {"alias_lifetime", "shared_net_refined_once", SharedNetRefinedOnce},
    {"alias_lifetime", "share_members_regroups", ShareMembersRegroups},
    {"alias_lifetime", "last_wrapper_departure", LastWrapperDeparture},
    {"alias_lifetime", "copy_and_move", CopyAndMove},
    {"continuation", "refinement_after_departure", RefinementAfterDeparture},
    {"continuation", "independent_groups_and_copies",
     IndependentGroupsAndCopies},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
