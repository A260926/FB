/* Copyright (c) 2018–2021 SplineLib

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */

// Shared refinement checks.  The cases are registered through the C task
// driver in task_support/driver.c via the task_cases/task_case_count symbols.
#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>

#include "BSplineLib/Splines/b_spline.hpp"
#include "BSplineLib/Splines/nurbs.hpp"
#include "task_support/check.h"

namespace {

using namespace bsplinelib;
using P2 = parameter_spaces::ParameterSpace<2>;
using P1 = parameter_spaces::ParameterSpace<1>;
using KnotVector = parameter_spaces::KnotVector;
using V = vector_spaces::VectorSpace;
using WVS = vector_spaces::WeightedVectorSpace;
using BS2 = splines::BSpline<2>;
using NU2 = splines::Nurbs<2>;
using BS1 = splines::BSpline<1>;
using NU1 = splines::Nurbs<1>;

constexpr double kRelTol = 1e-9;

bool Close(double x, double y) {
  return std::abs(x - y) <= kRelTol * std::max(1.0, std::abs(y));
}

template<typename Coordinate>
bool CloseAll(Coordinate const& a, Coordinate const& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (int i = 0; i < a.size(); ++i) {
    if (!Close(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

std::shared_ptr<P2> MakeSurfaceSpace() {
  auto u = std::make_shared<KnotVector>(std::vector<double>{0, 0, 1, 1});
  auto v = std::make_shared<KnotVector>(std::vector<double>{0, 0, 1, 1});
  return std::make_shared<P2>(P2::KnotVectors_{u, v}, P2::Degrees_{1, 1});
}

std::shared_ptr<P1> MakeCurveSpace() {
  auto u = std::make_shared<KnotVector>(
      std::vector<double>{0, 0, 0, 1, 1, 1});
  return std::make_shared<P1>(P1::KnotVectors_{u}, P1::Degrees_{2});
}

// Bilinear, non-planar control net (z carries an i*j term).
std::shared_ptr<V> MakeSurfaceNet(double sx, double sy, double sz) {
  V::Coordinates_ c(4, 3);
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      int const id = j * 2 + i;
      c(id, 0) = sx * (i + 1);
      c(id, 1) = sy * (j + 1);
      c(id, 2) = sz * (i * j + 0.3 * i + 0.7 * j + 0.1);
    }
  }
  return std::make_shared<V>(std::move(c));
}

std::shared_ptr<WVS> MakeWeightedSurfaceNet() {
  WVS::Coordinates_ c(4, 3);
  WVS::Weights_ w(4);
  for (int j = 0; j < 2; ++j) {
    for (int i = 0; i < 2; ++i) {
      int const id = j * 2 + i;
      c(id, 0) = static_cast<double>(i);
      c(id, 1) = static_cast<double>(j);
      c(id, 2) = 0.5 * i * j + 0.2 * i + 0.4 * j + 0.2;
      w[id] = 0.5 + 0.25 * id; // deliberately non-unit weights
    }
  }
  return std::make_shared<WVS>(c, w);
}

std::array<double, 2> const kSurfacePoints[] = {
    {0.13, 0.27}, {0.50, 0.50}, {0.83, 0.41}, {0.37, 0.91}};

template<typename Spline>
struct SurfaceSnapshot {
  std::vector<typename Spline::Coordinate_> val, du, dv, duv;
};

template<typename Spline>
SurfaceSnapshot<Spline>
SnapshotSurface(Spline const& spline,
                std::vector<std::array<double, 2>> const& points) {
  SurfaceSnapshot<Spline> snapshot;
  for (auto const& q : points) {
    double const uv[2] = {q[0], q[1]};
    int const du[2] = {1, 0}, dv[2] = {0, 1}, duv[2] = {1, 1};
    snapshot.val.push_back(spline(uv));
    snapshot.du.push_back(spline(uv, du));
    snapshot.dv.push_back(spline(uv, dv));
    snapshot.duv.push_back(spline(uv, duv));
  }
  return snapshot;
}

template<typename Spline>
bool VerifySurface(Spline const& spline,
                   SurfaceSnapshot<Spline> const& reference,
                   std::vector<std::array<double, 2>> const& points) {
  for (std::size_t k = 0; k < points.size(); ++k) {
    double const uv[2] = {points[k][0], points[k][1]};
    int const du[2] = {1, 0}, dv[2] = {0, 1}, duv[2] = {1, 1};
    if (!CloseAll(spline(uv), reference.val[k])
        || !CloseAll(spline(uv, du), reference.du[k])
        || !CloseAll(spline(uv, dv), reference.dv[k])
        || !CloseAll(spline(uv, duv), reference.duv[k])) {
      return false;
    }
  }
  return true;
}

std::vector<std::array<double, 2>> const kPoints(
    std::begin(kSurfacePoints), std::end(kSurfacePoints));

// ---------------------------------------------------------------------------
// shared_geometry
// ---------------------------------------------------------------------------

int MixedGroupGeometry() {
  auto p = MakeSurfaceSpace();
  p->EnableSharedRefinement();
  TASK_CHECK(p->IsSharedRefinementEnabled());

  auto va = MakeSurfaceNet(1.0, 1.0, 1.0);
  auto vb = MakeSurfaceNet(2.0, 0.5, 1.5);
  auto vw = MakeWeightedSurfaceNet();
  BS2 a(p, va), b(p, vb);
  NU2 n(p, vw);
  TASK_CHECK(p->GetSharedRefinementRegistry()->NumberOfNets() == 3);

  auto const sa = SnapshotSurface(a, kPoints);
  auto const sb = SnapshotSurface(b, kPoints);
  auto const sn = SnapshotSurface(n, kPoints);

  // Alternate edits initiated from every member, along both directions.
  a.InsertKnot(0, 0.5);
  b.InsertKnot(1, 0.25);
  n.InsertKnot(0, 0.75);
  a.RefineKnots(1, std::vector<double>{0.4, 0.6});
  n.ElevateDegree(0);
  b.ElevateDegree(1);
  a.InsertKnot(0, 0.35); // continue inserting after elevation
  n.InsertKnot(1, 0.55);

  int const basis = p->GetTotalNumberOfBasisFunctions();
  TASK_CHECK(basis == va->GetNumberOfCoordinates());
  TASK_CHECK(basis == vb->GetNumberOfCoordinates());
  TASK_CHECK(basis == vw->GetNumberOfCoordinates());
  TASK_CHECK(p->GetDegree(0) == 2 && p->GetDegree(1) == 2);

  TASK_CHECK(VerifySurface(a, sa, kPoints));
  TASK_CHECK(VerifySurface(b, sb, kPoints));
  TASK_CHECK(VerifySurface(n, sn, kPoints));

  // Knot multiplicities are identical for every member (one shared space).
  auto const multiplicities = p->KnotMultiplicities();
  TASK_CHECK(multiplicities.size() == 2);
  return 0;
}

int FullMultiplicityAndEndpoints() {
  auto p = MakeSurfaceSpace();
  p->EnableSharedRefinement();
  auto va = MakeSurfaceNet(1.0, 1.0, 1.0);
  auto vb = MakeSurfaceNet(2.0, 0.5, 1.5);
  BS2 a(p, va), b(p, vb);
  auto const sa = SnapshotSurface(a, kPoints);
  auto const sb = SnapshotSurface(b, kPoints);

  // Drive the interior knot 0.5 to full multiplicity (degree 1 -> p + 1 = 2):
  // the U basis grows 2 -> 4 and the total net size becomes 4 * 2 = 8.
  a.InsertKnot(0, 0.5, Multiplicity{2});
  TASK_CHECK(va->GetNumberOfCoordinates() == 8);
  TASK_CHECK(vb->GetNumberOfCoordinates() == 8);
  auto const mul_full = p->KnotMultiplicities();
  TASK_CHECK(mul_full[0].size() >= 2);

  // Further insertion must be truncated to zero effective insertions.
  int const basis_before = p->GetTotalNumberOfBasisFunctions();
  b.InsertKnot(0, 0.5, Multiplicity{3});
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == basis_before);
  TASK_CHECK(va->GetNumberOfCoordinates() == basis_before);
  TASK_CHECK(vb->GetNumberOfCoordinates() == basis_before);

  // Inserting exactly on an endpoint is an early exit that changes nothing.
  a.InsertKnot(0, 0.0);
  a.InsertKnot(1, 1.0);
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == basis_before);

  TASK_CHECK(VerifySurface(a, sa, kPoints));
  TASK_CHECK(VerifySurface(b, sb, kPoints));
  return 0;
}

int CurveGroupGeometry() {
  auto p = MakeCurveSpace();
  p->EnableSharedRefinement();
  V::Coordinates_ c(3, 2);
  for (int i = 0; i < 3; ++i) {
    c(i, 0) = i;
    c(i, 1) = 0.5 * i + 0.1 * i * i;
  }
  auto vs = std::make_shared<V>(std::move(c));
  WVS::Coordinates_ rc(3, 2);
  WVS::Weights_ w(3);
  for (int i = 0; i < 3; ++i) {
    rc(i, 0) = i;
    rc(i, 1) = i + 0.3;
    w[i] = 0.6 + 0.2 * i;
  }
  auto vw = std::make_shared<WVS>(rc, w);
  BS1 s(p, vs);
  NU1 r(p, vw);

  std::array<double, 4> const ts{0.1, 0.35, 0.5, 0.9};
  std::vector<BS1::Coordinate_> sv, sd, rv, rd;
  int const deriv1[1] = {1};
  for (double t : ts) {
    sv.push_back(s(&t));
    sd.push_back(s(&t, deriv1));
    rv.push_back(r(&t));
    rd.push_back(r(&t, deriv1));
  }

  s.InsertKnot(0, 0.5);
  r.ElevateDegree(0);
  s.InsertKnot(0, 0.25);

  TASK_CHECK(p->GetDegree(0) == 3);
  TASK_CHECK(vs->GetNumberOfCoordinates() == vw->GetNumberOfCoordinates());
  for (std::size_t k = 0; k < ts.size(); ++k) {
    double const t = ts[k];
    TASK_CHECK(CloseAll(s(&t), sv[k]));
    TASK_CHECK(CloseAll(s(&t, deriv1), sd[k]));
    TASK_CHECK(CloseAll(r(&t), rv[k]));
    TASK_CHECK(CloseAll(r(&t, deriv1), rd[k]));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// alias_lifetime
// ---------------------------------------------------------------------------

int SharedNetAliasRefinedOnce() {
  auto p = MakeSurfaceSpace();
  p->EnableSharedRefinement();
  auto va = MakeSurfaceNet(1.0, 1.0, 1.0);
  auto vb = MakeSurfaceNet(2.0, 0.5, 1.5);
  auto vw = MakeWeightedSurfaceNet();
  auto a = std::make_shared<BS2>(p, va);
  auto b = std::make_shared<BS2>(p, vb);
  auto n1 = std::make_shared<NU2>(p, vw);
  BS2 ctor_alias(p, va); // another wrapper on a's net via the constructor
  auto alias = std::make_shared<BS2>();
  alias->ShareMembers(a); // same control net as a
  auto n_alias = std::make_shared<NU2>();
  n_alias->ShareMembers(n1); // same homogeneous net as n1
  // Three distinct nets even though six wrappers participate.
  TASK_CHECK(p->GetSharedRefinementRegistry()->NumberOfNets() == 3);

  auto const before = (*alias)(kPoints[0].data());
  auto const ctor_before = ctor_alias(kPoints[0].data());
  b->InsertKnot(0, 0.5);
  // The aliased net grew exactly once (one insertion, not once per wrapper).
  TASK_CHECK(va->GetNumberOfCoordinates() == 6);
  TASK_CHECK(vb->GetNumberOfCoordinates() == 6);
  TASK_CHECK(vw->GetNumberOfCoordinates() == 6);
  TASK_CHECK(CloseAll((*alias)(kPoints[0].data()), before));
  TASK_CHECK(CloseAll(ctor_alias(kPoints[0].data()), ctor_before));

  a->ElevateDegree(0);
  TASK_CHECK(vw->GetNumberOfCoordinates()
             == p->GetTotalNumberOfBasisFunctions());
  TASK_CHECK(va->GetNumberOfCoordinates()
             == p->GetTotalNumberOfBasisFunctions());
  return 0;
}

int ShareMembersSwitchesGroup() {
  auto pa = MakeSurfaceSpace();
  pa->EnableSharedRefinement();
  auto pb = MakeSurfaceSpace();
  pb->EnableSharedRefinement();

  auto va = MakeSurfaceNet(1.0, 1.0, 1.0);
  auto va2 = MakeSurfaceNet(1.0, 2.0, 1.0);
  auto vb = MakeSurfaceNet(2.0, 0.5, 1.5);
  auto member_a = std::make_shared<BS2>(pa, va);
  auto member_a2 = std::make_shared<BS2>(pa, va2);
  auto member_b = std::make_shared<BS2>(pb, vb);
  auto traveller = std::make_shared<BS2>();
  traveller->ShareMembers(member_a);
  TASK_CHECK(pa->GetSharedRefinementRegistry()->NumberOfNets() == 2);

  // Move into the other parameter space's group: leave A, join B.
  traveller->ShareMembers(member_b);
  TASK_CHECK(pa->GetSharedRefinementRegistry()->NumberOfNets() == 2);
  TASK_CHECK(pb->GetSharedRefinementRegistry()->NumberOfNets() == 1);

  int const basis_a = pa->GetTotalNumberOfBasisFunctions();
  member_b->InsertKnot(0, 0.4); // edit in B must not touch group A
  TASK_CHECK(pa->GetTotalNumberOfBasisFunctions() == basis_a);
  TASK_CHECK(va->GetNumberOfCoordinates() == 4);
  TASK_CHECK(vb->GetNumberOfCoordinates() == 6); // traveller's new net
  member_a->InsertKnot(1, 0.3);
  TASK_CHECK(va->GetNumberOfCoordinates() == 6);
  TASK_CHECK(vb->GetNumberOfCoordinates() == 6); // still B, unaffected by A
  return 0;
}

int CopyIsIndependent() {
  auto p = MakeSurfaceSpace();
  p->EnableSharedRefinement();
  auto va = MakeSurfaceNet(1.0, 1.0, 1.0);
  auto vb = MakeSurfaceNet(2.0, 0.5, 1.5);
  BS2 a(p, va), b(p, vb);
  a.InsertKnot(0, 0.5);

  BS2 copy(a);
  int const shared_basis = p->GetTotalNumberOfBasisFunctions();
  int const shared_coords = va->GetNumberOfCoordinates();
  copy.InsertKnot(1, 0.55); // standalone edit, must not reach the group
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == shared_basis);
  TASK_CHECK(va->GetNumberOfCoordinates() == shared_coords);
  TASK_CHECK(vb->GetNumberOfCoordinates() == shared_coords);
  // The copy grew its own independent control net instead.
  TASK_CHECK(copy.Dim() == 3);

  // Removal is allowed on the independent copy but blocked on group members.
  bool removed = false;
  try {
    copy.RemoveKnot(1, 0.55, 1e-10);
    removed = true;
  } catch (std::logic_error const&) {
  }
  TASK_CHECK(removed);
  bool blocked = false;
  try {
    a.RemoveKnot(0, 0.5, 1e-10);
  } catch (std::logic_error const&) {
    blocked = true;
  }
  TASK_CHECK(blocked);
  return 0;
}

int MoveContinuesParticipation() {
  auto p = MakeSurfaceSpace();
  p->EnableSharedRefinement();
  auto va = MakeSurfaceNet(1.0, 1.0, 1.0);
  auto vb = MakeSurfaceNet(2.0, 0.5, 1.5);
  auto a = std::make_shared<BS2>(p, va);
  auto b = std::make_shared<BS2>(p, vb);

  BS2 moved(std::move(*b)); // new object takes over b's participation
  TASK_CHECK(p->GetSharedRefinementRegistry()->NumberOfNets() == 2);
  auto const snapshot = SnapshotSurface(moved, kPoints);
  moved.InsertKnot(1, 0.7);
  TASK_CHECK(vb->GetNumberOfCoordinates() == 6);
  TASK_CHECK(va->GetNumberOfCoordinates() == 6);
  TASK_CHECK(VerifySurface(moved, snapshot, kPoints));

  BS2 assigned;
  assigned = std::move(*a); // move assignment likewise continues
  TASK_CHECK(p->GetSharedRefinementRegistry()->NumberOfNets() == 2);
  assigned.InsertKnot(0, 0.3);
  TASK_CHECK(va->GetNumberOfCoordinates() == 9);
  TASK_CHECK(vb->GetNumberOfCoordinates() == 9);
  return 0;
}

int LeaveOnDestroy() {
  auto p = MakeSurfaceSpace();
  p->EnableSharedRefinement();
  auto vb = MakeSurfaceNet(2.0, 0.5, 1.5);
  auto keep_net = MakeSurfaceNet(1.0, 1.0, 1.0);
  {
    BS2 transient(p, keep_net);
    TASK_CHECK(p->GetSharedRefinementRegistry()->NumberOfNets() == 1);
    BS2 other(p, vb);
    TASK_CHECK(p->GetSharedRefinementRegistry()->NumberOfNets() == 2);
  } // both wrappers die; the caller still holds the net shared_ptrs
  TASK_CHECK(p->GetSharedRefinementRegistry()->NumberOfNets() == 0);

  auto vc = MakeSurfaceNet(3.0, 3.0, 3.0);
  BS2 fresh(p, vc);
  TASK_CHECK(p->GetSharedRefinementRegistry()->NumberOfNets() == 1);
  fresh.InsertKnot(0, 0.5);
  TASK_CHECK(keep_net->GetNumberOfCoordinates() == 4); // no longer updated
  TASK_CHECK(vb->GetNumberOfCoordinates() == 4);
  TASK_CHECK(vc->GetNumberOfCoordinates() == 6);
  return 0;
}

int ObjectIdentityPreserved() {
  auto p = MakeSurfaceSpace();
  p->EnableSharedRefinement();
  auto va = MakeSurfaceNet(1.0, 1.0, 1.0);
  auto vw = MakeWeightedSurfaceNet();
  void* const p_addr = static_cast<void*>(p.get());
  void* const ku_addr = static_cast<void*>(p->GetKnotVector(0).get());
  void* const kv_addr = static_cast<void*>(p->GetKnotVector(1).get());
  void* const va_addr = static_cast<void*>(va.get());
  void* const vw_addr = static_cast<void*>(vw.get());

  BS2 a(p, va);
  NU2 n(p, vw);
  a.InsertKnot(0, 0.5);
  n.InsertKnot(1, 0.25);
  a.ElevateDegree(0);
  n.ElevateDegree(1);
  a.InsertKnot(0, 0.35);

  TASK_CHECK(static_cast<void*>(p.get()) == p_addr);
  TASK_CHECK(static_cast<void*>(p->GetKnotVector(0).get()) == ku_addr);
  TASK_CHECK(static_cast<void*>(p->GetKnotVector(1).get()) == kv_addr);
  TASK_CHECK(static_cast<void*>(va.get()) == va_addr);
  TASK_CHECK(static_cast<void*>(vw.get()) == vw_addr);
  return 0;
}

// ---------------------------------------------------------------------------
// continuation
// ---------------------------------------------------------------------------

int InterleavedLongRun() {
  auto p = MakeSurfaceSpace();
  p->EnableSharedRefinement();
  auto va = MakeSurfaceNet(1.0, 1.0, 1.0);
  auto vb = MakeSurfaceNet(2.0, 0.5, 1.5);
  auto vw = MakeWeightedSurfaceNet();
  BS2 a(p, va), b(p, vb);
  NU2 n(p, vw);
  auto sa = SnapshotSurface(a, kPoints);
  auto sb = SnapshotSurface(b, kPoints);
  auto sn = SnapshotSurface(n, kPoints);

  // Knots are kept distinct within each direction until both degrees have
  // been elevated, i.e. no interior knot reaches full multiplicity before
  // elevation.  Initiators and directions alternate across all three members.
  a.InsertKnot(0, 0.20);
  b.InsertKnot(1, 0.70);
  n.InsertKnot(0, 0.40);
  a.InsertKnot(1, 0.30);
  b.InsertKnot(0, 0.80);
  n.InsertKnot(1, 0.60);
  a.ElevateDegree(0);
  n.ElevateDegree(1);
  a.InsertKnot(0, 0.50);
  b.InsertKnot(1, 0.50);
  n.InsertKnot(0, 0.60);

  int const basis = p->GetTotalNumberOfBasisFunctions();
  TASK_CHECK(va->GetNumberOfCoordinates() == basis);
  TASK_CHECK(vb->GetNumberOfCoordinates() == basis);
  TASK_CHECK(vw->GetNumberOfCoordinates() == basis);
  TASK_CHECK(p->GetDegree(0) == 2 && p->GetDegree(1) == 2);
  TASK_CHECK(VerifySurface(a, sa, kPoints));
  TASK_CHECK(VerifySurface(b, sb, kPoints));
  TASK_CHECK(VerifySurface(n, sn, kPoints));
  return 0;
}

int RefineAfterLeaveAndRejoin() {
  auto p = MakeSurfaceSpace();
  p->EnableSharedRefinement();
  auto va = MakeSurfaceNet(1.0, 1.0, 1.0);
  auto vb = MakeSurfaceNet(2.0, 0.5, 1.5);
  auto a = std::make_shared<BS2>(p, va);
  auto b = std::make_shared<BS2>(p, vb);
  a->InsertKnot(0, 0.5);
  TASK_CHECK(vb->GetNumberOfCoordinates() == 6);

  // b leaves the group by destruction; a keeps refining on its own.
  b.reset();
  TASK_CHECK(p->GetSharedRefinementRegistry()->NumberOfNets() == 1);
  a->InsertKnot(1, 0.25);
  TASK_CHECK(va->GetNumberOfCoordinates() == 9);
  TASK_CHECK(vb->GetNumberOfCoordinates() == 6); // frozen after leaving

  // A new member constructed later joins and catches up through joint edits.
  // Its net is built at the current basis size (construction requires a
  // matching, owned control net).
  V::Coordinates_ grown(p->GetTotalNumberOfBasisFunctions(), 3);
  for (int i = 0; i < grown.Shape()[0]; ++i) {
    grown(i, 0) = 0.1 * i;
    grown(i, 1) = 0.2 * i;
    grown(i, 2) = 0.05 * i;
  }
  auto vc_match = std::make_shared<V>(std::move(grown));
  BS2 late(p, vc_match);
  TASK_CHECK(p->GetSharedRefinementRegistry()->NumberOfNets() == 2);
  auto const snapshot = SnapshotSurface(late, kPoints);
  a->InsertKnot(0, 0.35);
  TASK_CHECK(vc_match->GetNumberOfCoordinates()
             == va->GetNumberOfCoordinates());
  TASK_CHECK(VerifySurface(late, snapshot, kPoints));
  return 0;
}

int RemovalOperationsBlocked() {
  auto p = MakeSurfaceSpace();
  p->EnableSharedRefinement();
  auto va = MakeSurfaceNet(1.0, 1.0, 1.0);
  auto vw = MakeWeightedSurfaceNet();
  BS2 a(p, va);
  NU2 n(p, vw);
  a.InsertKnot(0, 0.5);
  int const basis = p->GetTotalNumberOfBasisFunctions();

  auto expect_logic = [](auto const& operation) {
    bool thrown = false;
    try {
      operation();
    } catch (std::logic_error const&) {
      thrown = true;
    }
    return thrown;
  };

  TASK_CHECK(expect_logic([&] { a.RemoveKnot(0, 0.5, 1e-10); }));
  TASK_CHECK(expect_logic(
      [&] { a.CoarsenKnots(0, std::vector<double>{0.5}, 1e-10); }));
  TASK_CHECK(expect_logic([&] { a.ReduceDegree(0, 1e-10); }));
  TASK_CHECK(expect_logic([&] { n.RemoveKnot(0, 0.5, 1e-10); }));
  TASK_CHECK(expect_logic([&] { n.ReduceDegree(0, 1e-10); }));

  // Nothing was modified by the blocked operations.
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == basis);
  TASK_CHECK(va->GetNumberOfCoordinates() == basis);
  TASK_CHECK(vw->GetNumberOfCoordinates() == basis);

  // Elevation still works, including its internal knot removal step.
  a.ElevateDegree(0);
  TASK_CHECK(p->GetDegree(0) == 2);
  TASK_CHECK(va->GetNumberOfCoordinates()
             == p->GetTotalNumberOfBasisFunctions());
  TASK_CHECK(vw->GetNumberOfCoordinates()
             == p->GetTotalNumberOfBasisFunctions());
  return 0;
}

int ExistingExceptionsPreserved() {
  auto p = MakeSurfaceSpace();
  p->EnableSharedRefinement();
  auto va = MakeSurfaceNet(1.0, 1.0, 1.0);
  BS2 a(p, va);
  int const basis = p->GetTotalNumberOfBasisFunctions();

  bool bad_dimension = false;
  try {
    a.InsertKnot(9, 0.5);
  } catch (std::out_of_range const&) {
    bad_dimension = true;
  }
  TASK_CHECK(bad_dimension);

  bool out_of_domain = false;
  try {
    a.InsertKnot(0, 2.0);
  } catch (std::domain_error const&) {
    out_of_domain = true;
  }
  TASK_CHECK(out_of_domain);
  TASK_CHECK(p->GetTotalNumberOfBasisFunctions() == basis);
  TASK_CHECK(va->GetNumberOfCoordinates() == 4);
  return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"shared_geometry", "mixed_group_geometry", &MixedGroupGeometry},
    {"shared_geometry", "full_multiplicity_and_endpoints",
     &FullMultiplicityAndEndpoints},
    {"shared_geometry", "curve_group_geometry", &CurveGroupGeometry},
    {"alias_lifetime", "shared_net_alias_refined_once",
     &SharedNetAliasRefinedOnce},
    {"alias_lifetime", "share_members_switches_group",
     &ShareMembersSwitchesGroup},
    {"alias_lifetime", "copy_is_independent", &CopyIsIndependent},
    {"alias_lifetime", "move_continues_participation",
     &MoveContinuesParticipation},
    {"alias_lifetime", "leave_on_destroy", &LeaveOnDestroy},
    {"alias_lifetime", "object_identity_preserved", &ObjectIdentityPreserved},
    {"continuation", "interleaved_long_run", &InterleavedLongRun},
    {"continuation", "refine_after_leave_and_rejoin",
     &RefineAfterLeaveAndRejoin},
    {"continuation", "removal_operations_blocked", &RemovalOperationsBlocked},
    {"continuation", "existing_exceptions_preserved",
     &ExistingExceptionsPreserved},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
