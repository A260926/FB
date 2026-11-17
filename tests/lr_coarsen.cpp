/*
 * Copyright (C) 2024 SINTEF ICT, Applied Mathematics, Norway.
 *
 * Contact information: E-mail: tor.dokken@sintef.no
 * SINTEF ICT, Department of Applied Mathematics,
 * P.O. Box 124 Blindern,
 * 0314 Oslo, Norway.
 *
 * This file is part of GoTools.
 *
 * GoTools is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * GoTools is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public
 * License along with GoTools. If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * In accordance with Section 7(b) of the GNU Affero General Public
 * License, a covered work must retain the producer line in every data
 * file that is created or manipulated using GoTools.
 */

// Regression tests for LRSplineSurface::tryCoarsen.  This file provides no
// main(); the cases are driven through task_support/driver.c.

#include "task_support/check.h"

#include "GoTools/geometry/SplineSurface.h"
#include "GoTools/lrsplines2D/LRSplineSurface.h"
#include "GoTools/lrsplines2D/LRBSpline2D.h"
#include "GoTools/lrsplines2D/LRBSpline2DUtils.h"
#include "GoTools/lrsplines2D/Mesh2DIterator.h"
#include "GoTools/viewlib/vol_and_lr/LRSurfaceTesselator.h"
#include "GoTools/tesselator/GenericTriMesh.h"

#include <cmath>
#include <sstream>
#include <vector>
#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>

using namespace Go;

namespace
{

const double KNOT_TOL = 1.0e-10;
const double GEOM_TOL = 1.0e-8;

// ---------------------------------------------------------------------------
// Surface construction
// ---------------------------------------------------------------------------
LRSplineSurface* make_surface(bool rational, int deg)
{
  const int n = deg + 1;
  std::vector<double> knots;
  knots.insert(knots.end(), n, 0.0);
  knots.insert(knots.end(), n, 1.0);
  std::vector<double> rcoefs;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i)
      {
	const double w = rational ? (1.0 + 0.21*i + 0.13*j + 0.07*i*j) : 1.0;
	const double x = 0.41*i + 0.10*j;
	const double y = 0.33*j + 0.05*i*i;
	const double z = 0.17*i*j + 0.11*i + 0.07*j*j;
	rcoefs.insert(rcoefs.end(), {w*x, w*y, w*z, w});
      }
  SplineSurface spline(n, n, deg + 1, deg + 1,
		       knots.data(), knots.data(), rcoefs.data(),
		       3, rational);
  return new LRSplineSurface(&spline, KNOT_TOL);
}

// ---------------------------------------------------------------------------
// Geometry comparison against an independent copy and a tensor evaluation
// ---------------------------------------------------------------------------
double geometry_gap(const LRSplineSurface& lr,
		    const LRSplineSurface& ref_lr,
		    const SplineSurface& ref_tensor)
{
  double worst = 0.0;
  for (int i = 0; i < 9; ++i)
    for (int j = 0; j < 9; ++j)
      {
	const double u = (i + 0.37)/9.0;
	const double v = (j + 0.61)/9.0;
	for (int du = 0; du <= 1; ++du)
	  for (int dv = 0; dv <= 1 - du; ++dv)
	    {
	      const Point p_lr = lr(u, v, du, dv);
	      const Point p_cp = ref_lr(u, v, du, dv);
	      std::vector<Point> p_tp(3);
	      ref_tensor.point(p_tp, u, v, 1);
	      const Point& p_ts = (du == 0 && dv == 0) ? p_tp[0]
				: (du == 1) ? p_tp[1] : p_tp[2];
	      worst = std::max(worst, p_lr.dist(p_cp));
	      worst = std::max(worst, p_lr.dist(p_ts));
	    }
      }
  return worst;
}

// ---------------------------------------------------------------------------
// Per-segment mesh multiplicity snapshot
// ---------------------------------------------------------------------------
struct MeshSnapshot
{
  std::vector<double> ux;
  std::vector<double> vy;
  std::vector<std::vector<int> > mx; // XFIXED line multiplicities
  std::vector<std::vector<int> > my; // YFIXED line multiplicities
};

MeshSnapshot snapshot(const LRSplineSurface& lr)
{
  const Mesh2D& m = lr.mesh();
  MeshSnapshot s;
  const int nx = m.numDistinctKnots(XFIXED);
  const int ny = m.numDistinctKnots(YFIXED);
  for (int ix = 0; ix < nx; ++ix)
    s.ux.push_back(m.kval(XFIXED, ix));
  for (int iy = 0; iy < ny; ++iy)
    s.vy.push_back(m.kval(YFIXED, iy));
  s.mx.assign(nx, std::vector<int>(ny - 1, 0));
  s.my.assign(ny, std::vector<int>(nx - 1, 0));
  for (int ix = 0; ix < nx; ++ix)
    for (int j = 0; j < ny - 1; ++j)
      s.mx[ix][j] = m.nu(XFIXED, ix, j, j + 1);
  for (int iy = 0; iy < ny; ++iy)
    for (int i = 0; i < nx - 1; ++i)
      s.my[iy][i] = m.nu(YFIXED, iy, i, i + 1);
  return s;
}

bool same_snapshot(const MeshSnapshot& a, const MeshSnapshot& b, double tol)
{
  if (a.ux.size() != b.ux.size() || a.vy.size() != b.vy.size())
    return false;
  for (size_t i = 0; i < a.ux.size(); ++i)
    if (std::fabs(a.ux[i] - b.ux[i]) > tol)
      return false;
  for (size_t i = 0; i < a.vy.size(); ++i)
    if (std::fabs(a.vy[i] - b.vy[i]) > tol)
      return false;
  return a.mx == b.mx && a.my == b.my;
}

// ---------------------------------------------------------------------------
// Every LR B-spline must be minimal-support on the current mesh.  This is the
// same criterion as LRBSpline2DUtils::try_split_once.
// ---------------------------------------------------------------------------
int start_multiplicity(const std::vector<int>& v)
{
  int m = 0;
  while (m < (int)v.size() && v[m] == v.front())
    ++m;
  return m;
}
int end_multiplicity(const std::vector<int>& v)
{
  int m = 0;
  while (m < (int)v.size() && v[v.size() - 1 - m] == v.back())
    ++m;
  return m;
}
int num_inner_knots(const std::vector<int>& v)
{
  return (int)v.size() - start_multiplicity(v) - end_multiplicity(v);
}

bool all_basis_minimal(const LRSplineSurface& lr)
{
  const Mesh2D& m = lr.mesh();
  for (auto it = lr.basisFunctionsBegin(); it != lr.basisFunctionsEnd(); ++it)
    {
      const LRBSpline2D* b = it->second.get();
      const int umin = b->suppMin(XFIXED);
      const int umax = b->suppMax(XFIXED);
      const int vmin = b->suppMin(YFIXED);
      const int vmax = b->suppMax(YFIXED);
      const std::vector<int> mu =
	LRBSpline2DUtils::derive_knots(m, XFIXED, umin, umax, vmin, vmax);
      const std::vector<int> mv =
	LRBSpline2DUtils::derive_knots(m, YFIXED, vmin, vmax, umin, umax);
      if (!std::includes(mu.begin(), mu.end(),
			 b->kvec(XFIXED).begin(), b->kvec(XFIXED).end()) ||
	  !std::includes(mv.begin(), mv.end(),
			 b->kvec(YFIXED).begin(), b->kvec(YFIXED).end()))
	return false;
      if (num_inner_knots(mu) != num_inner_knots(b->kvec(XFIXED)) ||
	  num_inner_knots(mv) != num_inner_knots(b->kvec(YFIXED)))
	return false;
    }
  return true;
}

// ---------------------------------------------------------------------------
// Bidirectional basis/element support consistency.
// ---------------------------------------------------------------------------
bool support_consistent(const LRSplineSurface& lr)
{
  for (auto eit = lr.elementsBegin(); eit != lr.elementsEnd(); ++eit)
    {
      const Element2D* e = eit->second.get();
      for (auto fit = e->supportBegin(); fit != e->supportEnd(); ++fit)
	if (!(*fit)->hasSupportedElement(const_cast<Element2D*>(e)))
	  return false;
    }
  for (auto bit = lr.basisFunctionsBegin();
       bit != lr.basisFunctionsEnd(); ++bit)
    {
      LRBSpline2D* b = bit->second.get();
      const std::vector<Element2D*>& elems = b->supportedElements();
      if (elems.empty())
	return false;
      for (size_t k = 0; k < elems.size(); ++k)
	if (!elems[k]->hasSupportFunction(b))
	  return false;
    }
  return true;
}

// ---------------------------------------------------------------------------
// Elements partition the full parameter domain.
// ---------------------------------------------------------------------------
bool domain_covered(const LRSplineSurface& lr)
{
  if (lr.numElements() == 0)
    return false;
  const Mesh2D& m = lr.mesh();
  int mesh_cells = 0;
  for (Mesh2DIterator mit = m.begin(); mit != m.end(); ++mit)
    {
      const int u0 = (*mit)[0];
      const int v0 = (*mit)[1];
      const LRSplineSurface::ElemKey key = {m.kval(XFIXED, u0),
					    m.kval(YFIXED, v0)};
      bool found = false;
      for (auto eit = lr.elementsBegin(); eit != lr.elementsEnd(); ++eit)
	if (!(eit->first < key) && !(key < eit->first))
	  {
	    found = true;
	    break;
	  }
      if (!found)
	return false;
      ++mesh_cells;
    }
  if (mesh_cells != lr.numElements())
    return false;

  // Dense parameter sampling must always resolve to an element.
  for (int i = 0; i < 13; ++i)
    for (int j = 0; j < 13; ++j)
      {
	const double u = (i + 0.5)/13.0;
	const double v = (j + 0.5)/13.0;
	Element2D* e = lr.coveringElement(u, v);
	if (!e || !e->contains(u, v))
	  return false;
      }
  return true;
}

// ---------------------------------------------------------------------------
// gamma normalization (and positive rational weights)
// ---------------------------------------------------------------------------
bool normalization_ok(const LRSplineSurface& lr)
{
  for (auto it = lr.basisFunctionsBegin(); it != lr.basisFunctionsEnd(); ++it)
    {
      const LRBSpline2D* b = it->second.get();
      if (std::fabs(b->gamma() - 1.0) > 1.0e-12)
	return false;
      if (lr.rational() &&
	  (!std::isfinite(b->weight()) || b->weight() <= 0.0))
	return false;
    }
  return true;
}

// ---------------------------------------------------------------------------
// Production tesselation path: back-substituted parameters, positions and
// first-order normals.
// ---------------------------------------------------------------------------
bool production_mesh_ok(const LRSplineSurface& lr)
{
  LRSurfaceTesselator tesselator(lr);
  tesselator.changeRes(15, 11);
  shared_ptr<GenericTriMesh> mesh = tesselator.getMesh();
  if (mesh->numVertices() <= 0 || mesh->numTriangles() <= 0)
    return false;

  const double umin = lr.startparam_u();
  const double umax = lr.endparam_u();
  const double vmin = lr.startparam_v();
  const double vmax = lr.endparam_v();
  for (int i = 0; i < mesh->numVertices(); ++i)
    {
      const double u = mesh->paramArray()[2*i];
      const double v = mesh->paramArray()[2*i + 1];
      if (u < umin - 1e-10 || u > umax + 1e-10 ||
	  v < vmin - 1e-10 || v > vmax + 1e-10)
	return false;
      Point pos;
      lr.point(pos, u, v);
      const Point got(mesh->vertexArray() + 3*i,
		      mesh->vertexArray() + 3*i + 3);
      if (pos.dist(got) > GEOM_TOL*(1.0 + pos.length()))
	return false;
      Point normal;
      lr.normal(normal, u, v);
      const Point got_n(mesh->normalArray() + 3*i,
			mesh->normalArray() + 3*i + 3);
      // Normals are unit vectors; allow a slightly looser tolerance.
      if (normal.dist(got_n) > 1.0e-6)
	return false;
    }
  const unsigned int* tri = mesh->triangleIndexArray();
  for (int i = 0; i < 3*mesh->numTriangles(); ++i)
    if ((int)tri[i] >= mesh->numVertices())
      return false;
  return true;
}

// A reference tensor spline matching the current LR geometry exactly.
// asSplineSurface() is a non-const operation in the GoTools interface.
SplineSurface* tensor_reference(LRSplineSurface& lr)
{
  return lr.asSplineSurface();
}

} // anonymous namespace

// ===========================================================================
// local_basis group
// ===========================================================================

// Crossing full-span lines and partial T-joint lines; exact inverse of the
// refinement, verified segment by segment.
static int case_crossing_tjoint_reverse()
{
  std::unique_ptr<LRSplineSurface> lr(make_surface(false, 2));
  std::unique_ptr<LRSplineSurface> ref(lr->clone());
  const MeshSnapshot tensor_snap = snapshot(*lr);

  lr->refine(XFIXED, 0.5, 0.0, 1.0);
  lr->refine(YFIXED, 0.5, 0.0, 1.0);
  lr->refine(XFIXED, 0.25, 0.0, 0.5);   // T-joint
  lr->refine(YFIXED, 0.75, 0.5, 1.0);   // T-joint
  TASK_CHECK(lr->numBasisFunctions() > ref->numBasisFunctions());
  TASK_CHECK(lr->numElements() > ref->numElements());
  const MeshSnapshot refined_snap = snapshot(*lr);

  // Warm the element query cache first.
  Point warm;
  lr->point(warm, 0.31, 0.71);
  lr->normal(warm, 0.31, 0.71);

  std::vector<LRSplineSurface::Refinement2D> reqs(4);
  reqs[0].setVal(0.25, 0.0, 0.5, XFIXED, 1);
  reqs[1].setVal(0.75, 0.5, 1.0, YFIXED, 1);
  reqs[2].setVal(0.5, 0.0, 1.0, XFIXED, 1);
  reqs[3].setVal(0.5, 0.0, 1.0, YFIXED, 1);
  TASK_CHECK(lr->tryCoarsen(reqs));

  const MeshSnapshot coarse_snap = snapshot(*lr);
  TASK_CHECK(same_snapshot(coarse_snap, tensor_snap, KNOT_TOL));
  TASK_CHECK(lr->numBasisFunctions() == ref->numBasisFunctions());
  TASK_CHECK(lr->numElements() == ref->numElements());
  TASK_CHECK(all_basis_minimal(*lr));
  TASK_CHECK(support_consistent(*lr));
  TASK_CHECK(domain_covered(*lr));
  TASK_CHECK(normalization_ok(*lr));
  TASK_CHECK(lr->isFullTensorProduct());
  (void)refined_snap;

  std::unique_ptr<SplineSurface> tps(tensor_reference(*lr));
  TASK_CHECK(geometry_gap(*lr, *ref, *tps) < GEOM_TOL);
  TASK_CHECK(production_mesh_ok(*lr));
  return 0;
}

// Mixed local and full-span lines; coarsen only part of the mesh, so the
// result stays a genuine (non-tensor) LR mesh.
static int case_local_fullspan_mix()
{
  std::unique_ptr<LRSplineSurface> lr(make_surface(false, 3));
  std::unique_ptr<LRSplineSurface> ref(lr->clone());

  lr->refine(XFIXED, 0.5, 0.0, 1.0);             // full span
  lr->refine(XFIXED, 0.25, 0.0, 0.5);            // local T-joint line
  lr->refine(YFIXED, 0.5, 0.0, 1.0);             // full span
  lr->refine(YFIXED, 0.75, 0.25, 1.0);           // local segment
  const int basis_before = lr->numBasisFunctions();
  const int elements_before = lr->numElements();

  std::vector<LRSplineSurface::Refinement2D> reqs(2);
  reqs[0].setVal(0.25, 0.0, 0.5, XFIXED, 1);    // remove local line
  reqs[1].setVal(0.75, 0.25, 1.0, YFIXED, 1);   // remove local segment
  TASK_CHECK(lr->tryCoarsen(reqs));

  TASK_CHECK(lr->numBasisFunctions() < basis_before);
  TASK_CHECK(lr->numElements() < elements_before);
  TASK_CHECK(!lr->isFullTensorProduct());        // full-span lines remain
  TASK_CHECK(all_basis_minimal(*lr));
  TASK_CHECK(support_consistent(*lr));
  TASK_CHECK(domain_covered(*lr));
  TASK_CHECK(normalization_ok(*lr));

  // The two full-span lines must still be present, segment by segment.
  const Mesh2D& m = lr->mesh();
  double probe = 0.5;
  const int ix5 = m.getKnotIdx(XFIXED, probe, KNOT_TOL);
  TASK_CHECK(ix5 >= 0);
  const int ny = m.numDistinctKnots(YFIXED);
  for (int j = 0; j < ny - 1; ++j)
    TASK_CHECK(m.nu(XFIXED, ix5, j, j + 1) == 1);

  std::unique_ptr<SplineSurface> tps(tensor_reference(*lr));
  TASK_CHECK(geometry_gap(*lr, *ref, *tps) < GEOM_TOL);
  return 0;
}

// A real geometric detail cannot be represented on the coarsened mesh.
static int case_unremovable_detail()
{
  std::unique_ptr<LRSplineSurface> lr(make_surface(false, 2));
  lr->refine(XFIXED, 0.5, 0.0, 1.0);
  const int basis_before = lr->numBasisFunctions();
  const int elements_before = lr->numElements();
  const MeshSnapshot snap_before = snapshot(*lr);

  // Introduce detail that genuinely needs the extra line of freedom.
  auto it = lr->basisFunctionsBegin();
  std::advance(it, 3);
  Point coef = it->second->Coef();
  coef[2] += 1.37;
  lr->setCoef(coef, it->second.get());
  std::unique_ptr<LRSplineSurface> detailed(lr->clone());

  std::vector<LRSplineSurface::Refinement2D> req(1);
  req[0].setVal(0.5, 0.0, 1.0, XFIXED, 1);
  TASK_CHECK(!lr->tryCoarsen(req));

  // Complete preservation of the object.
  TASK_CHECK(lr->numBasisFunctions() == basis_before);
  TASK_CHECK(lr->numElements() == elements_before);
  TASK_CHECK(same_snapshot(snapshot(*lr), snap_before, KNOT_TOL));
  TASK_CHECK(support_consistent(*lr));
  std::unique_ptr<SplineSurface> tps1(tensor_reference(*lr));
  TASK_CHECK(geometry_gap(*lr, *detailed, *tps1) < 1.0e-14);

  // An unrelated full-span refinement is still removable on its own: adding
  // another line and pulling it back must work.
  lr->refine(YFIXED, 0.5, 0.0, 1.0);
  std::vector<LRSplineSurface::Refinement2D> ok_req(1);
  ok_req[0].setVal(0.5, 0.0, 1.0, YFIXED, 1);
  TASK_CHECK(lr->tryCoarsen(ok_req));
  TASK_CHECK(lr->numBasisFunctions() == basis_before);
  std::unique_ptr<SplineSurface> tps2(tensor_reference(*lr));
  TASK_CHECK(geometry_gap(*lr, *detailed, *tps2) < GEOM_TOL);
  return 0;
}

// ===========================================================================
// homogeneous group
// ===========================================================================

// Rational surface with non-uniform weights: exact inverse, homogeneous
// numerator and denominator preserved.
static int case_rational_nonuniform()
{
  std::unique_ptr<LRSplineSurface> lr(make_surface(true, 2));
  std::unique_ptr<LRSplineSurface> ref(lr->clone());

  lr->refine(XFIXED, 0.5, 0.0, 1.0);
  lr->refine(YFIXED, 0.5, 0.0, 1.0);
  lr->refine(XFIXED, 0.25, 0.0, 0.5);
  TASK_CHECK(lr->rational());

  std::vector<LRSplineSurface::Refinement2D> reqs(3);
  reqs[0].setVal(0.25, 0.0, 0.5, XFIXED, 1);
  reqs[1].setVal(0.5, 0.0, 1.0, XFIXED, 1);
  reqs[2].setVal(0.5, 0.0, 1.0, YFIXED, 1);
  TASK_CHECK(lr->tryCoarsen(reqs));

  TASK_CHECK(lr->rational());
  TASK_CHECK(normalization_ok(*lr));
  TASK_CHECK(all_basis_minimal(*lr));
  TASK_CHECK(support_consistent(*lr));
  TASK_CHECK(domain_covered(*lr));

  // Non-uniform weights survive (they do not all collapse to one value).
  double wmin = std::numeric_limits<double>::max();
  double wmax = 0.0;
  for (auto it = lr->basisFunctionsBegin();
       it != lr->basisFunctionsEnd(); ++it)
    {
      wmin = std::min(wmin, it->second->weight());
      wmax = std::max(wmax, it->second->weight());
    }
  TASK_CHECK(wmax > wmin);

  std::unique_ptr<SplineSurface> tps(tensor_reference(*lr));
  TASK_CHECK(tps->rational());
  TASK_CHECK(geometry_gap(*lr, *ref, *tps) < GEOM_TOL);
  TASK_CHECK(production_mesh_ok(*lr));
  return 0;
}

// Multiplying every weight by a common positive number must not change the
// coarsening decision.
static int case_weight_scaling()
{
  std::unique_ptr<LRSplineSurface> lr(make_surface(true, 2));
  lr->refine(XFIXED, 0.5, 0.0, 1.0);
  lr->refine(YFIXED, 0.5, 0.0, 1.0);
  for (auto it = lr->basisFunctionsBegin();
       it != lr->basisFunctionsEnd(); ++it)
    {
      const double new_w = 4.25*it->second->weight();
      Point c = it->second->Coef();
      it->second->weight() = new_w;
      it->second->setCoefAndGamma(c, it->second->gamma());
    }
  std::unique_ptr<LRSplineSurface> ref(lr->clone());

  std::vector<LRSplineSurface::Refinement2D> reqs(2);
  reqs[0].setVal(0.5, 0.0, 1.0, XFIXED, 1);
  reqs[1].setVal(0.5, 0.0, 1.0, YFIXED, 1);
  TASK_CHECK(lr->tryCoarsen(reqs));
  TASK_CHECK(normalization_ok(*lr));
  std::unique_ptr<SplineSurface> tps(tensor_reference(*lr));
  TASK_CHECK(geometry_gap(*lr, *ref, *tps) < GEOM_TOL);
  return 0;
}

// Rational cubic surface with T-joints.
static int case_rational_cubic()
{
  std::unique_ptr<LRSplineSurface> lr(make_surface(true, 3));
  std::unique_ptr<LRSplineSurface> ref(lr->clone());

  // Internal knot values valid for a cubic surface.
  lr->refine(XFIXED, 1.0/3.0, 0.0, 1.0);
  lr->refine(YFIXED, 2.0/3.0, 0.0, 1.0);
  lr->refine(XFIXED, 0.5, 0.0, 2.0/3.0);
  TASK_CHECK(lr->degree(XFIXED) == 3 && lr->degree(YFIXED) == 3);

  std::vector<LRSplineSurface::Refinement2D> reqs(3);
  reqs[0].setVal(0.5, 0.0, 2.0/3.0, XFIXED, 1);
  reqs[1].setVal(1.0/3.0, 0.0, 1.0, XFIXED, 1);
  reqs[2].setVal(2.0/3.0, 0.0, 1.0, YFIXED, 1);
  TASK_CHECK(lr->tryCoarsen(reqs));
  TASK_CHECK(all_basis_minimal(*lr));
  TASK_CHECK(support_consistent(*lr));
  TASK_CHECK(domain_covered(*lr));
  std::unique_ptr<SplineSurface> tps(tensor_reference(*lr));
  TASK_CHECK(geometry_gap(*lr, *ref, *tps) < GEOM_TOL);
  return 0;
}

// ===========================================================================
// continuation group
// ===========================================================================

// Coarsening after warm queries; the cached element pointer is invalidated
// and rebuilt transparently.
static int case_warm_query_coarsen()
{
  std::unique_ptr<LRSplineSurface> lr(make_surface(false, 2));
  lr->refine(XFIXED, 0.5, 0.0, 1.0);
  lr->refine(YFIXED, 0.5, 0.0, 1.0);

  Point p0;
  lr->point(p0, 0.37, 0.43);
  Element2D* old_elem = lr->coveringElement(0.37, 0.43);
  TASK_CHECK(old_elem != 0);

  std::vector<LRSplineSurface::Refinement2D> reqs(2);
  reqs[0].setVal(0.5, 0.0, 1.0, XFIXED, 1);
  reqs[1].setVal(0.5, 0.0, 1.0, YFIXED, 1);
  TASK_CHECK(lr->tryCoarsen(reqs));

  // Queries after coarsening must work and return identical geometry.
  Point p1;
  lr->point(p1, 0.37, 0.43);
  TASK_CHECK(p0.dist(p1) < GEOM_TOL);
  Point d1 = (*lr)(0.21, 0.83, 1, 0);
  Point d2 = (*lr)(0.21, 0.83, 0, 1);
  TASK_CHECK(std::isfinite(d1[0]) && std::isfinite(d2[0]));
  TASK_CHECK(domain_covered(*lr));
  return 0;
}

// A batch whose last request cannot be honored: nothing at all may change.
static int case_batch_last_fails()
{
  std::unique_ptr<LRSplineSurface> lr(make_surface(false, 2));
  lr->refine(XFIXED, 0.5, 0.0, 1.0);
  lr->refine(YFIXED, 0.5, 0.0, 1.0);
  auto it = lr->basisFunctionsBegin();
  std::advance(it, 4);
  Point coef = it->second->Coef();
  coef[1] += 2.1;
  lr->setCoef(coef, it->second.get());

  std::unique_ptr<LRSplineSurface> before(lr->clone());
  const MeshSnapshot snap_before = snapshot(*lr);
  const int basis_before = lr->numBasisFunctions();

  // First request is removable; the second removes the line carrying detail.
  std::vector<LRSplineSurface::Refinement2D> reqs(2);
  reqs[0].setVal(0.5, 0.0, 1.0, YFIXED, 1);
  reqs[1].setVal(0.5, 0.0, 1.0, XFIXED, 1);
  TASK_CHECK(!lr->tryCoarsen(reqs));

  TASK_CHECK(lr->numBasisFunctions() == basis_before);
  TASK_CHECK(same_snapshot(snapshot(*lr), snap_before, KNOT_TOL));
  TASK_CHECK(support_consistent(*lr));
  std::unique_ptr<SplineSurface> tps(tensor_reference(*lr));
  TASK_CHECK(geometry_gap(*lr, *before, *tps) < 1.0e-14);
  return 0;
}

// write/read roundtrip leaves no operation history; coarsening must still
// work on the reloaded object.
static int case_reload_no_history()
{
  std::unique_ptr<LRSplineSurface> lr(make_surface(true, 2));
  lr->refine(XFIXED, 0.5, 0.0, 1.0);
  lr->refine(YFIXED, 0.5, 0.0, 1.0);
  lr->refine(XFIXED, 0.25, 0.0, 0.5);
  std::unique_ptr<LRSplineSurface> ref(lr->clone());

  std::stringstream stream;
  lr->write(stream);
  LRSplineSurface loaded;
  loaded.read(stream);
  TASK_CHECK(loaded.numBasisFunctions() == lr->numBasisFunctions());
  TASK_CHECK(loaded.numElements() == lr->numElements());

  std::vector<LRSplineSurface::Refinement2D> reqs(3);
  reqs[0].setVal(0.25, 0.0, 0.5, XFIXED, 1);
  reqs[1].setVal(0.5, 0.0, 1.0, XFIXED, 1);
  reqs[2].setVal(0.5, 0.0, 1.0, YFIXED, 1);
  TASK_CHECK(loaded.tryCoarsen(reqs));
  TASK_CHECK(all_basis_minimal(loaded));
  TASK_CHECK(support_consistent(loaded));
  TASK_CHECK(domain_covered(loaded));
  std::unique_ptr<SplineSurface> tps(tensor_reference(loaded));
  TASK_CHECK(geometry_gap(loaded, *ref, *tps) < GEOM_TOL);

  // A second roundtrip on the coarsened object.
  std::stringstream stream2;
  loaded.write(stream2);
  LRSplineSurface reloaded;
  reloaded.read(stream2);
  TASK_CHECK(reloaded.numBasisFunctions() == loaded.numBasisFunctions());
  std::unique_ptr<SplineSurface> tps2(tensor_reference(reloaded));
  TASK_CHECK(geometry_gap(reloaded, *ref, *tps2) < GEOM_TOL);
  return 0;
}

// Coarsen, then refine again; the round trip preserves geometry and all
// structural invariants.
static int case_refine_after_coarsen()
{
  std::unique_ptr<LRSplineSurface> lr(make_surface(false, 3));
  std::unique_ptr<LRSplineSurface> ref(lr->clone());
  lr->refine(XFIXED, 0.5, 0.0, 1.0);
  lr->refine(YFIXED, 0.5, 0.0, 1.0);
  lr->refine(XFIXED, 0.25, 0.0, 0.5);

  std::vector<LRSplineSurface::Refinement2D> reqs(3);
  reqs[0].setVal(0.25, 0.0, 0.5, XFIXED, 1);
  reqs[1].setVal(0.5, 0.0, 1.0, XFIXED, 1);
  reqs[2].setVal(0.5, 0.0, 1.0, YFIXED, 1);
  TASK_CHECK(lr->tryCoarsen(reqs));
  TASK_CHECK(lr->isFullTensorProduct());

  // Refine again along a different pattern, building a genuine local
  // T-joint line whose segment carries multiplicity two.
  lr->refine(YFIXED, 1.0/3.0, 0.0, 1.0);
  lr->refine(XFIXED, 0.5, 0.0, 1.0/3.0);
  lr->refine(XFIXED, 0.5, 0.0, 1.0/3.0); // second multiplicity on the segment
  TASK_CHECK(all_basis_minimal(*lr));
  TASK_CHECK(support_consistent(*lr));
  TASK_CHECK(domain_covered(*lr));
  TASK_CHECK(!lr->isFullTensorProduct());
  {
    double probe = 0.5;
    const int ix = lr->mesh().getKnotIdx(XFIXED, probe, KNOT_TOL);
    TASK_CHECK(ix >= 0);
    TASK_CHECK(lr->mesh().nu(XFIXED, ix, 0, 1) == 2);
  }

  // Two overlapping requests accumulate and remove multiplicity two in one
  // batch; the line disappears and the underlying tensor mesh is recovered.
  std::vector<LRSplineSurface::Refinement2D> doubled(2);
  doubled[0].setVal(0.5, 0.0, 1.0/3.0, XFIXED, 1);
  doubled[1].setVal(0.5, 0.0, 1.0/3.0, XFIXED, 1);
  TASK_CHECK(lr->tryCoarsen(doubled));
  TASK_CHECK(support_consistent(*lr));
  TASK_CHECK(domain_covered(*lr));
  {
    double probe = 0.5;
    TASK_CHECK(lr->mesh().getKnotIdx(XFIXED, probe, KNOT_TOL) < 0);
  }

  // Rebuild the multiplicity-two local line for the remaining checks.
  lr->refine(XFIXED, 0.5, 0.0, 1.0/3.0);
  lr->refine(XFIXED, 0.5, 0.0, 1.0/3.0);

  // Pull one multiplicity back; the local line itself must remain.
  std::vector<LRSplineSurface::Refinement2D> back(1);
  back[0].setVal(0.5, 0.0, 1.0/3.0, XFIXED, 1);
  TASK_CHECK(lr->tryCoarsen(back));
  TASK_CHECK(support_consistent(*lr));
  TASK_CHECK(domain_covered(*lr));
  TASK_CHECK(!lr->isFullTensorProduct());

  // Overlapping requests accumulate segment by segment: after the previous
  // edit only one multiplicity remains, so a cumulative batch asking for two
  // is rejected with std::invalid_argument and must leave the object
  // untouched, while a single request removes the local line completely.
  {
    double probe = 0.5;
    const int ix = lr->mesh().getKnotIdx(XFIXED, probe, KNOT_TOL);
    TASK_CHECK(ix >= 0);
    TASK_CHECK(lr->mesh().nu(XFIXED, ix, 0, 1) == 1);
  }
  const int basis_before_doubled = lr->numBasisFunctions();
  bool threw = false;
  try { lr->tryCoarsen(doubled); }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);
  TASK_CHECK(lr->numBasisFunctions() == basis_before_doubled);

  std::vector<LRSplineSurface::Refinement2D> single(1);
  single[0].setVal(0.5, 0.0, 1.0/3.0, XFIXED, 1);
  TASK_CHECK(lr->tryCoarsen(single));
  TASK_CHECK(support_consistent(*lr));
  TASK_CHECK(domain_covered(*lr));
  {
    double probe = 0.5;
    TASK_CHECK(lr->mesh().getKnotIdx(XFIXED, probe, KNOT_TOL) < 0);
  }

  std::unique_ptr<SplineSurface> tps(tensor_reference(*lr));
  TASK_CHECK(geometry_gap(*lr, *ref, *tps) < GEOM_TOL);
  TASK_CHECK(production_mesh_ok(*lr));
  return 0;
}

// ===========================================================================
// Case registration
// ===========================================================================
extern const struct task_case task_cases[] = {
  {"local_basis", "crossing_tjoint_reverse", case_crossing_tjoint_reverse},
  {"local_basis", "local_fullspan_mix",      case_local_fullspan_mix},
  {"local_basis", "unremovable_detail",      case_unremovable_detail},
  {"homogeneous", "rational_nonuniform",     case_rational_nonuniform},
  {"homogeneous", "weight_scaling",          case_weight_scaling},
  {"homogeneous", "rational_cubic",          case_rational_cubic},
  {"continuation", "warm_query_coarsen",     case_warm_query_coarsen},
  {"continuation", "batch_last_fails",       case_batch_last_fails},
  {"continuation", "reload_no_history",      case_reload_no_history},
  {"continuation", "refine_after_coarsen",   case_refine_after_coarsen},
};

extern const size_t task_case_count =
  sizeof(task_cases)/sizeof(task_cases[0]);
