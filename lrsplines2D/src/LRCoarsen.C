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
 *
 * Other Usage
 * You can be released from the requirements of the license by purchasing
 * a commercial license. Buying such a license is mandatory as soon as you
 * develop commercial activities involving the GoTools library without
 * disclosing the source code of your own applications.
 *
 * This file may be used in accordance with the terms contained in a
 * written agreement between you and SINTEF ICT.
 */

#include "GoTools/lrsplines2D/LRSplineSurface.h"
#include "GoTools/lrsplines2D/LRSplineUtils.h"
#include "GoTools/lrsplines2D/LRBSpline2DUtils.h"
#include "GoTools/lrsplines2D/BSplineUniUtils.h"
#include "GoTools/lrsplines2D/Mesh2DUtils.h"
#include "GoTools/lrsplines2D/LRBSpline2D.h"
#include "GoTools/geometry/SplineSurface.h"

#include <stdexcept>
#include <cmath>
#include <vector>
#include <memory>

using std::vector;
using std::unique_ptr;

namespace Go
{
namespace
{

// Relative/absolute tolerances mandated for the coarsening decision.
const double COEF_TOL = 1.0e-10;
const double GEOM_TOL = 1.0e-8;

// Locate the index of the knot value 'val' along direction 'd', requiring a
// match within the given knot tolerance.  Returns -1 if no match.
int find_knot_ix(const Mesh2D& mesh, Direction2D d, double val, double tol)
{
  const int n = mesh.numDistinctKnots(d);
  int ix = Mesh2DUtils::last_nonlarger_knotvalue_ix(mesh, d, val);
  if (ix >= 0 && std::fabs(mesh.kval(d, ix) - val) <= tol)
    return ix;
  if (ix + 1 < n && std::fabs(mesh.kval(d, ix + 1) - val) <= tol)
    return ix + 1;
  return -1;
}

// Verify that the surface belongs to the restricted class the coarsening
// supports.
void check_surface_class(const LRSplineSurface& surf)
{
  if (surf.dimension() != 3)
    throw std::invalid_argument("tryCoarsen: only 3D surfaces are supported.");
  const int degu = surf.degree(XFIXED);
  const int degv = surf.degree(YFIXED);
  if ((degu != 2 && degu != 3) || (degv != 2 && degv != 3))
    throw std::invalid_argument("tryCoarsen: only degrees 2 and 3 supported.");

  const Mesh2D& mesh = surf.mesh();
  const int nx = mesh.numDistinctKnots(XFIXED);
  const int ny = mesh.numDistinctKnots(YFIXED);
  if (mesh.nu(XFIXED, 0, 0, ny - 1) != degu + 1 ||
      mesh.nu(XFIXED, nx - 1, 0, ny - 1) != degu + 1 ||
      mesh.nu(YFIXED, 0, 0, nx - 1) != degv + 1 ||
      mesh.nu(YFIXED, ny - 1, 0, nx - 1) != degv + 1)
    throw std::invalid_argument("tryCoarsen: only open, clamped (k-regular) "
				"LR surfaces are supported.");

  if (surf.rational())
    {
      double max_weight = 0.0;
      for (auto it = surf.basisFunctionsBegin();
	   it != surf.basisFunctionsEnd(); ++it)
	max_weight = std::max(max_weight, std::fabs(it->second->weight()));
      if (!(max_weight > 0.0))
	throw std::invalid_argument("tryCoarsen: rational surface without "
				    "usable weights.");
      for (auto it = surf.basisFunctionsBegin();
	   it != surf.basisFunctionsEnd(); ++it)
	{
	  const double w = it->second->weight();
	  if (!std::isfinite(w) || w <= COEF_TOL*max_weight)
	    throw std::invalid_argument("tryCoarsen: only finite positive "
					"weights are supported.");
	}
    }
}

// Validate the request batch and accumulate the multiplicity decrements onto
// elementary meshrectangles.  Throws std::invalid_argument for any illegal
// request.
void validate_and_accumulate(const LRSplineSurface& surf,
			     const vector<LRSplineSurface::Refinement2D>& reqs,
			     vector<vector<int> >& decrement_x,
			     vector<vector<int> >& decrement_y)
{
  const Mesh2D& mesh = surf.mesh();
  const double tol = surf.getKnotTol();
  const int nx = mesh.numDistinctKnots(XFIXED);
  const int ny = mesh.numDistinctKnots(YFIXED);
  decrement_x.assign(nx, vector<int>(ny - 1, 0));
  decrement_y.assign(ny, vector<int>(nx - 1, 0));

  for (size_t kr = 0; kr < reqs.size(); ++kr)
    {
      const LRSplineSurface::Refinement2D& req = reqs[kr];
      if (req.d != XFIXED && req.d != YFIXED)
	throw std::invalid_argument("tryCoarsen: illegal refinement direction.");
      if (!std::isfinite(req.kval) || !std::isfinite(req.start) ||
	  !std::isfinite(req.end))
	throw std::invalid_argument("tryCoarsen: non-finite parameter value.");
      if (req.multiplicity <= 0)
	throw std::invalid_argument("tryCoarsen: multiplicity must be positive.");
      if (req.multiplicity > surf.degree(req.d) + 1)
	throw std::invalid_argument("tryCoarsen: multiplicity exceeds degree+1.");
      if (!(req.end > req.start))
	throw std::invalid_argument("tryCoarsen: empty or backwards segment.");

      const Direction2D other = (req.d == XFIXED) ? YFIXED : XFIXED;
      const int fixed_ix = find_knot_ix(mesh, req.d, req.kval, tol);
      if (fixed_ix <= 0 ||
	  fixed_ix >= mesh.numDistinctKnots(req.d) - 1)
	throw std::invalid_argument("tryCoarsen: fixed value must be an "
				    "existing internal mesh line.");
      const int start_ix = find_knot_ix(mesh, other, req.start, tol);
      const int end_ix = find_knot_ix(mesh, other, req.end, tol);
      if (start_ix < 0 || end_ix < 0 || end_ix <= start_ix)
	throw std::invalid_argument("tryCoarsen: segment endpoints must be "
				    "existing knots of the other direction.");

      for (int j = start_ix; j < end_ix; ++j)
	{
	  const int current = mesh.nu(req.d, fixed_ix, j, j + 1);
	  if (current <= 0)
	    throw std::invalid_argument("tryCoarsen: segment covers a place "
					"with no existing mesh line.");
	  if (req.d == XFIXED)
	    {
	      decrement_x[fixed_ix][j] += req.multiplicity;
	      if (decrement_x[fixed_ix][j] > current)
		throw std::invalid_argument("tryCoarsen: accumulated decrement "
					    "would make a multiplicity negative.");
	    }
	  else
	    {
	      decrement_y[fixed_ix][j] += req.multiplicity;
	      if (decrement_y[fixed_ix][j] > current)
		throw std::invalid_argument("tryCoarsen: accumulated decrement "
					    "would make a multiplicity negative.");
	    }
	}
    }
}

// Solve R^T lambda = f in the least-squares sense by Gaussian elimination
// with pivoting restricted to the basis-function columns.  'mat' has n_rows
// rows and n_cols basis columns followed by one right-hand-side column.
// Returns false if the basis columns do not have full numerical rank or if
// the residual exceeds 'residual_tol'.
bool solve_row_space(vector<double> mat, int n_rows, int n_cols,
		     double pivot_tol, double residual_tol,
		     vector<double>& solution)
{
  const int stride = n_cols + 1;
  vector<int> pivots;
  int row = 0;
  for (int col = 0; col < n_cols && row < n_rows; ++col)
    {
      int pivot_row = -1;
      double pivot_val = pivot_tol;
      for (int r = row; r < n_rows; ++r)
	{
	  const double v = std::fabs(mat[r*stride + col]);
	  if (v > pivot_val)
	    {
	      pivot_val = v;
	      pivot_row = r;
	    }
	}
      if (pivot_row < 0)
	continue;
      if (pivot_row != row)
	for (int c = 0; c < stride; ++c)
	  std::swap(mat[row*stride + c], mat[pivot_row*stride + c]);
      const double piv = mat[row*stride + col];
      for (int r = 0; r < n_rows; ++r)
	{
	  if (r == row)
	    continue;
	  const double factor = mat[r*stride + col] / piv;
	  if (factor == 0.0)
	    continue;
	  mat[r*stride + col] = 0.0;
	  for (int c = col + 1; c < stride; ++c)
	    mat[r*stride + c] -= factor*mat[row*stride + c];
	}
      pivots.push_back(col);
      ++row;
    }

  if ((int)pivots.size() != n_cols)
    return false; // rank-deficient candidate basis

  for (int r = n_cols; r < n_rows; ++r)
    if (std::fabs(mat[r*stride + n_cols]) > residual_tol)
      return false; // geometry not representable on the coarsened mesh

  solution.assign(n_cols, 0.0);
  for (int r = 0; r < n_cols; ++r)
    {
      const int col = pivots[r];
      solution[col] = mat[r*stride + n_cols] / mat[r*stride + col];
    }
  return true;
}

// Translate the multiplicity structure of 'target' into a batch of
// absolute-multiplicity refinement segments (only internal meshrectangles).
vector<LRSplineSurface::Refinement2D>
mesh_as_refinements(const Mesh2D& target)
{
  vector<LRSplineSurface::Refinement2D> refs;
  const int nx = target.numDistinctKnots(XFIXED);
  const int ny = target.numDistinctKnots(YFIXED);
  for (int ix = 1; ix < nx - 1; ++ix)
    {
      const vector<GPos>& mr = target.mrects(XFIXED, ix);
      for (size_t k = 0; k < mr.size(); ++k)
	{
	  if (mr[k].mult <= 0)
	    continue;
	  const int j1 = mr[k].ix;
	  const int j2 = (k + 1 < mr.size()) ? mr[k + 1].ix : ny - 1;
	  LRSplineSurface::Refinement2D r;
	  r.setVal(target.kval(XFIXED, ix),
		   target.kval(YFIXED, j1),
		   target.kval(YFIXED, j2),
		   XFIXED, mr[k].mult);
	  refs.push_back(r);
	}
    }
  for (int iy = 1; iy < ny - 1; ++iy)
    {
      const vector<GPos>& mr = target.mrects(YFIXED, iy);
      for (size_t k = 0; k < mr.size(); ++k)
	{
	  if (mr[k].mult <= 0)
	    continue;
	  const int i1 = mr[k].ix;
	  const int i2 = (k + 1 < mr.size()) ? mr[k + 1].ix : nx - 1;
	  LRSplineSurface::Refinement2D r;
	  r.setVal(target.kval(YFIXED, iy),
		   target.kval(XFIXED, i1),
		   target.kval(XFIXED, i2),
		   YFIXED, mr[k].mult);
	  refs.push_back(r);
	}
    }
  return refs;
}

// Explicit tensor knot index vector (distinct knot indices repeated
// according to the uniform tensor multiplicity).
vector<int> explicit_knot_vector(const Mesh2D& mesh, Direction2D d)
{
  vector<int> result;
  const int n = mesh.numDistinctKnots(d);
  for (int ix = 0; ix < n; ++ix)
    result.insert(result.end(), mesh.largestMultInLine(d, ix), ix);
  return result;
}

// Find the first position of the knot vector 'kvec' inside the explicit
// tensor knot vector 'tensor_kvec'.
int find_tensor_start(const vector<int>& tensor_kvec,
		      const vector<int>& kvec)
{
  const int span = (int)kvec.size();
  for (int i = 0; i + span <= (int)tensor_kvec.size(); ++i)
    {
      bool equal = true;
      for (int j = 0; j < span; ++j)
	if (tensor_kvec[i + j] != kvec[j])
	  {
	    equal = false;
	    break;
	  }
      if (equal)
	return i;
    }
  return -1;
}

// Check that two meshes carry the same knot values and segment
// multiplicities.
bool meshes_equivalent(const Mesh2D& a, const Mesh2D& b, double tol)
{
  if (a.numDistinctKnots(XFIXED) != b.numDistinctKnots(XFIXED) ||
      a.numDistinctKnots(YFIXED) != b.numDistinctKnots(YFIXED))
    return false;
  for (int d = 0; d < 2; ++d)
    {
      const Direction2D dir = (d == 0) ? XFIXED : YFIXED;
      const int n = a.numDistinctKnots(dir);
      for (int i = 0; i < n; ++i)
	if (std::fabs(a.kval(dir, i) - b.kval(dir, i)) > tol)
	  return false;
    }
  const int nx = a.numDistinctKnots(XFIXED);
  const int ny = a.numDistinctKnots(YFIXED);
  for (int ix = 0; ix < nx; ++ix)
    for (int j = 0; j < ny - 1; ++j)
      if (a.nu(XFIXED, ix, j, j + 1) != b.nu(XFIXED, ix, j, j + 1))
	return false;
  for (int iy = 0; iy < ny; ++iy)
    for (int i = 0; i < nx - 1; ++i)
      if (a.nu(YFIXED, iy, i, i + 1) != b.nu(YFIXED, iy, i, i + 1))
	return false;
  return true;
}

} // anonymous namespace


// =============================================================================
bool LRSplineSurface::tryCoarsen(const vector<Refinement2D>& requests)
// =============================================================================
{
  if (requests.empty())
    return true;

  // Argument validation happens before any edit is attempted.
  check_surface_class(*this);
  vector<vector<int> > decrement_x;
  vector<vector<int> > decrement_y;
  validate_and_accumulate(*this, requests, decrement_x, decrement_y);

  try
  {
    const int dim = dimension();
    const int deg_u = degree(XFIXED);
    const int deg_v = degree(YFIXED);
    const double umin = paramMin(XFIXED);
    const double umax = paramMax(XFIXED);
    const double vmin = paramMin(YFIXED);
    const double vmax = paramMax(YFIXED);

    // Common tensor product representation of the current geometry.  The
    // expansion is done on a scratch object; the current surface is untouched
    // until the whole batch has been proven representable.
    LRSplineSurface work(*this);
    work.expandToFullTensorProduct();
    const Mesh2D& tensor_mesh = work.mesh();
    const int tn_u = tensor_mesh.numDistinctKnots(XFIXED);
    const int tn_v = tensor_mesh.numDistinctKnots(YFIXED);

    // Multiplicity of every line of the common tensor mesh.
    vector<int> tensor_mult_u(tn_u);
    vector<int> tensor_mult_v(tn_v);
    for (int ix = 0; ix < tn_u; ++ix)
      tensor_mult_u[ix] = tensor_mesh.largestMultInLine(XFIXED, ix);
    for (int iy = 0; iy < tn_v; ++iy)
      tensor_mult_v[iy] = tensor_mesh.largestMultInLine(YFIXED, iy);

    // Map a knot value to a common-tensor index.
    auto snap_ix = [&](Direction2D d, double val) -> int
      {
	const int ix = find_knot_ix(tensor_mesh, d, val, knot_tol_);
	if (ix < 0)
	  throw std::runtime_error("tryCoarsen: internal knot mismatch.");
	return ix;
      };

    // Build the requested, coarsened mesh from the current LR mesh (which
    // carries the local multiplicity structure, including gapped lines).
    Mesh2D cand_mesh = mesh_;
    const int nx = cand_mesh.numDistinctKnots(XFIXED);
    const int ny = cand_mesh.numDistinctKnots(YFIXED);
    for (int ix = 1; ix < nx - 1; ++ix)
      for (int j = 0; j < ny - 1; ++j)
	if (decrement_x[ix][j] > 0)
	  {
	    const int cur = cand_mesh.nu(XFIXED, ix, j, j + 1);
	    cand_mesh.setMult(XFIXED, ix, j, j + 1,
			      cur - decrement_x[ix][j]);
	  }
    for (int iy = 1; iy < ny - 1; ++iy)
      for (int i = 0; i < nx - 1; ++i)
	if (decrement_y[iy][i] > 0)
	  {
	    const int cur = cand_mesh.nu(YFIXED, iy, i, i + 1);
	    cand_mesh.setMult(YFIXED, iy, i, i + 1,
			      cur - decrement_y[iy][i]);
	  }
    // Lines that lost all their meshrectangles disappear; the elements on
    // either side are thereby merged.
    cand_mesh.removeUnusedLines(XFIXED);
    cand_mesh.removeUnusedLines(YFIXED);

    // Generate the minimal-support LR basis on the coarsened mesh by starting
    // from the clamped boundary tensor product and replaying the candidate
    // meshrectangles through the ordinary refinement machinery.  This
    // produces exactly the minimal LR B-spline collection of the candidate
    // mesh, including crossing local lines and T-joints.
    unique_ptr<LRSplineSurface> generator;
    {
      vector<double> ku(deg_u + 1, umin);
      vector<double> kv(deg_v + 1, vmin);
      ku.insert(ku.end(), deg_u + 1, umax);
      kv.insert(kv.end(), deg_v + 1, vmax);
      if (rational_)
	{
	  const int nu0 = deg_u + 1;
	  const int nv0 = deg_v + 1;
	  vector<double> rc(nu0*nv0*(dim + 1), 0.0);
	  for (int k = 0; k < nu0*nv0; ++k)
	    rc[k*(dim + 1) + dim] = 1.0;
	  SplineSurface coarse(nu0, nv0, deg_u + 1, deg_v + 1,
			       ku.data(), kv.data(), rc.data(),
			       dim, true);
	  generator.reset(new LRSplineSurface(&coarse, knot_tol_));
	}
      else
	{
	  generator.reset(new LRSplineSurface(deg_u, deg_v,
					      deg_u + 1, deg_v + 1,
					      dim, ku.begin(), kv.begin(),
					      knot_tol_));
	}
      // Replay the candidate meshrectangles in waves: a segment can only be
      // inserted once its two endpoint knots already exist as lines in the
      // orthogonal direction (this is exactly the LR-mesh construction
      // rule).  Full-span segments, whose endpoints are the boundary knots,
      // are therefore applied first; partial T-joint segments follow.
      vector<Refinement2D> pending = mesh_as_refinements(cand_mesh);
      while (!pending.empty())
	{
	  vector<Refinement2D> ready;
	  vector<Refinement2D> deferred;
	  for (size_t k = 0; k < pending.size(); ++k)
	    {
	      const Refinement2D& r = pending[k];
	      const Direction2D other =
		(r.d == XFIXED) ? YFIXED : XFIXED;
	      if (find_knot_ix(generator->mesh(), other, r.start,
			       knot_tol_) >= 0 &&
		  find_knot_ix(generator->mesh(), other, r.end,
			       knot_tol_) >= 0)
		ready.push_back(r);
	      else
		deferred.push_back(r);
	    }
	  if (ready.empty())
	    throw std::runtime_error("tryCoarsen: cannot order mesh segments.");
	  generator->refine(ready, true);
	  pending.swap(deferred);
	}
      if (!meshes_equivalent(cand_mesh, generator->mesh(), knot_tol_))
	return false;
    }

    vector<LRBSpline2D*> cand_funcs;
    cand_funcs.reserve(generator->numBasisFunctions());
    for (auto it = generator->basisFunctionsBegin();
	 it != generator->basisFunctionsEnd(); ++it)
      cand_funcs.push_back(it->second.get());
    const int n_cand = (int)cand_funcs.size();
    const int n_tens = work.numBasisFunctions();
    if (n_cand >= n_tens)
      return false; // nothing was actually coarsened

    // Homogeneous components: geometric coordinates and, for rational
    // surfaces, the weight component.  The decision is taken entirely in the
    // common tensor coefficient space.
    const int n_comp = rational_ ? dim + 1 : dim;

    // Tensor functions in map order.  On a full tensor mesh the basis map is
    // ordered by v-support then u-support, with exactly 'n_tens_u' functions
    // in each v-row, so the map ordinal directly gives the tensor grid
    // position (iu = ordinal % n_tens_u, jv = ordinal / n_tens_u).
    struct TensorEntry { LRBSpline2D* b; int ordinal; };
    vector<TensorEntry> tens_entries;
    tens_entries.reserve(n_tens);
    {
      int ordinal = 0;
      for (auto it = work.basisFunctionsBegin();
	   it != work.basisFunctionsEnd(); ++it, ++ordinal)
	tens_entries.push_back(TensorEntry{it->second.get(), ordinal});
    }
    int sum_mult_u = 0;
    int sum_mult_v = 0;
    for (int ix = 0; ix < tn_u; ++ix)
      sum_mult_u += tensor_mult_u[ix];
    for (int iy = 0; iy < tn_v; ++iy)
      sum_mult_v += tensor_mult_v[iy];
    const int n_tens_u = sum_mult_u - deg_u - 1;
    const int n_tens_v = sum_mult_v - deg_v - 1;
    if (n_tens_u*n_tens_v != n_tens)
      throw std::runtime_error("tryCoarsen: tensor grid size mismatch.");

    // Homogeneous tensor coefficients of the current geometry.  In a rational
    // representation the homogeneous numerator control point is
    // (weight * geometric control point), followed by the weight itself.
    vector<double> tens_coef(n_tens*n_comp, 0.0);
    double coef_scale = 1.0;
    for (int k = 0; k < n_tens; ++k)
      {
	const LRBSpline2D* b = tens_entries[k].b;
	const Point& cg = b->coefTimesGamma();
	if (rational_)
	  {
	    const double w = b->weight();
	    for (int c = 0; c < dim; ++c)
	      {
		tens_coef[k*n_comp + c] = w*cg[c];
		coef_scale = std::max(coef_scale,
				      std::fabs(tens_coef[k*n_comp + c]));
	      }
	    tens_coef[k*n_comp + dim] = w;
	    coef_scale = std::max(coef_scale, std::fabs(w));
	  }
	else
	  {
	    for (int c = 0; c < dim; ++c)
	      {
		tens_coef[k*n_comp + c] = cg[c];
		coef_scale = std::max(coef_scale, std::fabs(cg[c]));
	      }
	  }
      }

    // Express every candidate minimal B-spline in the common tensor B-spline
    // space by means of the Oslo algorithm in each parameter direction.
    const Mesh2D& gen_mesh = generator->mesh();
    const vector<int> tkix_u = explicit_knot_vector(tensor_mesh, XFIXED);
    const vector<int> tkix_v = explicit_knot_vector(tensor_mesh, YFIXED);
    vector<double> candidate_in_tensor(n_cand*n_tens, 0.0);
    double max_r = 0.0;
    for (int cb = 0; cb < n_cand; ++cb)
      {
	LRBSpline2D* b = cand_funcs[cb];
	vector<int> old_u;
	vector<int> old_v;
	{
	  const vector<int>& ku = b->kvec(XFIXED);
	  const vector<int>& kv = b->kvec(YFIXED);
	  for (size_t i = 0; i < ku.size(); ++i)
	    old_u.push_back(snap_ix(XFIXED, gen_mesh.kval(XFIXED, ku[i])));
	  for (size_t i = 0; i < kv.size(); ++i)
	    old_v.push_back(snap_ix(YFIXED, gen_mesh.kval(YFIXED, kv[i])));
	}

	const vector<int> insert_u =
	  LRSplineUtils::knots_to_insert(old_u, tensor_mult_u);
	const vector<int> insert_v =
	  LRSplineUtils::knots_to_insert(old_v, tensor_mult_v);
	vector<int> final_u;
	vector<int> final_v;
	vector<double> alpha_u;
	vector<double> alpha_v;
	LRBSpline2DUtils::split_several(tensor_mesh.knotsBegin(XFIXED),
				       old_u, insert_u, final_u, alpha_u);
	LRBSpline2DUtils::split_several(tensor_mesh.knotsBegin(YFIXED),
				       old_v, insert_v, final_v, alpha_v);

	// Each subdivision child has a knot vector that is a contiguous
	// sub-vector of the explicit tensor knot vector; its first position is
	// the tensor grid coordinate of that child.
	vector<int> grid_u(alpha_u.size());
	vector<int> grid_v(alpha_v.size());
	for (int i = 0; i < (int)alpha_u.size(); ++i)
	  {
	    vector<int> child(final_u.begin() + i,
			      final_u.begin() + i + deg_u + 2);
	    grid_u[i] = find_tensor_start(tkix_u, child);
	    if (grid_u[i] < 0)
	      throw std::runtime_error("tryCoarsen: u support not in tensor.");
	  }
	for (int j = 0; j < (int)alpha_v.size(); ++j)
	  {
	    vector<int> child(final_v.begin() + j,
			      final_v.begin() + j + deg_v + 2);
	    grid_v[j] = find_tensor_start(tkix_v, child);
	    if (grid_v[j] < 0)
	      throw std::runtime_error("tryCoarsen: v support not in tensor.");
	  }

	for (int j = 0; j < (int)alpha_v.size(); ++j)
	  for (int i = 0; i < (int)alpha_u.size(); ++i)
	    {
	      const int idx = grid_v[j]*n_tens_u + grid_u[i];
	      const double alpha = alpha_u[i]*alpha_v[j];
	      candidate_in_tensor[cb*n_tens + idx] += alpha;
	      max_r = std::max(max_r, std::fabs(alpha));
	    }
      }

    // The coarsened space can represent the current surface exactly when the
    // current homogeneous tensor coefficients lie in the row space of the
    // candidate-to-tensor refinement matrix R.  Solving R^T lambda = f for
    // every homogeneous component simultaneously establishes full column rank
    // of R^T (linear independence of the candidate basis) and consistency of
    // the representation; a nonzero residual means the requested mesh line
    // carries real detail and cannot be removed.
    vector<double> rt(n_tens*n_cand, 0.0);
    for (int cb = 0; cb < n_cand; ++cb)
      for (int k = 0; k < n_tens; ++k)
	rt[k*n_cand + cb] = candidate_in_tensor[cb*n_tens + k];

    const double pivot_tol = COEF_TOL*std::max(1.0, max_r);
    const double residual_tol = COEF_TOL*coef_scale;
    vector<double> lambda(n_cand*n_comp, 0.0);
    for (int q = 0; q < n_comp; ++q)
      {
	vector<double> aug(n_tens*(n_cand + 1), 0.0);
	for (int k = 0; k < n_tens; ++k)
	  {
	    for (int cb = 0; cb < n_cand; ++cb)
	      aug[k*(n_cand + 1) + cb] = rt[k*n_cand + cb];
	    aug[k*(n_cand + 1) + n_cand] = tens_coef[k*n_comp + q];
	  }
	vector<double> y;
	if (!solve_row_space(aug, n_tens, n_cand, pivot_tol,
			     residual_tol, y))
	  return false;
	for (int cb = 0; cb < n_cand; ++cb)
	  lambda[cb*n_comp + q] = y[cb];
      }

    // Rational weights must remain finite and strictly positive; multiplying
    // every weight by a common positive number would not change any decision
    // above.
    if (rational_)
      {
	double max_new_weight = 0.0;
	for (int cb = 0; cb < n_cand; ++cb)
	  max_new_weight =
	    std::max(max_new_weight, std::fabs(lambda[cb*n_comp + dim]));
	for (int cb = 0; cb < n_cand; ++cb)
	  {
	    const double w = lambda[cb*n_comp + dim];
	    if (!std::isfinite(w) || w <= COEF_TOL*max_new_weight)
	      return false;
	  }
      }

    // Assign the solved homogeneous coefficients to the generated basis.
    for (int cb = 0; cb < n_cand; ++cb)
      {
	LRBSpline2D* b = cand_funcs[cb];
	Point cg(dim);
	if (rational_)
	  {
	    const double w = lambda[cb*n_comp + dim];
	    for (int k = 0; k < dim; ++k)
	      cg[k] = lambda[cb*n_comp + k] / w;
	    b->weight() = w;
	  }
	else
	  {
	    for (int k = 0; k < dim; ++k)
	      cg[k] = lambda[cb*n_comp + k];
	    b->weight() = 1.0;
	  }
	b->gamma() = 1.0;
	b->coefTimesGamma() = cg;
      }

    // Assemble the candidate surface through the private assembly
    // constructor, which rebuilds the univariate pools, the basis map and the
    // bidirectional basis/element support from scratch.
    LRSplineSurface candidate(knot_tol_, rational_, cand_mesh, cand_funcs,
			      0, 0);

    // Independent geometric verification: positions and first partial
    // derivatives over the whole parameter domain must agree with the
    // current surface.
    const int n_check = 7;
    for (int iv = 0; iv < n_check; ++iv)
      for (int iu = 0; iu < n_check; ++iu)
	{
	  const double u = umin + (umax - umin)*(iu + 0.37)/n_check;
	  const double v = vmin + (vmax - vmin)*(iv + 0.61)/n_check;
	  for (int du = 0; du <= 1; ++du)
	    for (int dv = 0; dv <= 1 - du; ++dv)
	      {
		const Point p0 = (*this)(u, v, du, dv);
		const Point p1 = candidate(u, v, du, dv);
		for (int c = 0; c < dim; ++c)
		  if (!std::isfinite(p1[c]))
		    return false;
		const double scale = std::max(1.0, p0.length());
		if (p0.dist(p1) > GEOM_TOL*(scale + 1.0))
		  return false;
	      }
	}

    // The whole batch is representable.  Commit the edit atomically.
    this->swap(candidate);
    curr_element_ = NULL;
    return true;
  }
  catch (const std::invalid_argument&)
  {
    throw;
  }
  catch (const std::exception&)
  {
    // Any structural failure means the coarsening could not be carried out;
    // the current object has not been modified.
    return false;
  }
}

} // end namespace Go
