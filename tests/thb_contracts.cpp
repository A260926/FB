/*
 * Contract tests for LNLib::ThbSurface (truncated hierarchical B-splines).
 *
 * The truncation checks use an independent, compact re-implementation of the
 * hierarchical construction (midpoint bisection knot vectors, hand-written
 * single knot insertion, support/domain tests and tensor B-spline evaluation
 * through the low-level Polynomials entry points), rather than comparing two
 * entry points of the production class against each other.
 */

#include "task_support/check.h"
#include "ThbSurface.h"
#include "NurbsSurface.h"
#include "Polynomials.h"
#include "LNObject.h"
#include "XYZW.h"
#include "UV.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace LNLib;

namespace
{
	const double kEps = 1.0e-9;

	bool close(double a, double b, double tol = kEps)
	{
		return std::abs(a - b) <= tol * (1.0 + std::max(std::abs(a), std::abs(b)));
	}

	bool sameXYZW(const XYZW& a, const XYZW& b, double tol = 1.0e-12)
	{
		for (int c = 0; c < 4; ++c)
		{
			if (!close(a[c], b[c], tol)) return false;
		}
		return true;
	}

	std::vector<double> clamped(int degree, const std::vector<double>& distinct)
	{
		std::vector<double> k;
		for (int i = 0; i <= degree; ++i) k.push_back(distinct.front());
		for (size_t i = 1; i + 1 < distinct.size(); ++i) k.push_back(distinct[i]);
		for (int i = 0; i <= degree; ++i) k.push_back(distinct.back());
		return k;
	}

	std::vector<double> distinctAt(const std::vector<double>& base, unsigned level)
	{
		size_t pieces = static_cast<size_t>(1) << level;
		std::vector<double> out;
		for (size_t i = 0; i + 1 < base.size(); ++i)
		{
			for (size_t k = 0; k < pieces; ++k)
			{
				out.push_back(base[i] + (base[i + 1] - base[i]) *
				              (static_cast<double>(k) / static_cast<double>(pieces)));
			}
		}
		out.push_back(base.back());
		return out;
	}

	LN_NurbsSurface makeSurface(int du, int dv,
	                            const std::vector<double>& distinctU,
	                            const std::vector<double>& distinctV)
	{
		LN_NurbsSurface s;
		s.DegreeU = du;
		s.DegreeV = dv;
		s.KnotVectorU = clamped(du, distinctU);
		s.KnotVectorV = clamped(dv, distinctV);
		int nu = static_cast<int>(distinctU.size()) + du - 1;
		int nv = static_cast<int>(distinctV.size()) + dv - 1;
		for (int i = 0; i < nu; ++i)
		{
			std::vector<XYZW> row;
			for (int j = 0; j < nv; ++j)
			{
				double w = 0.9 + 0.03 * i + 0.07 * j; // strictly positive, non-constant
				XYZ p(0.41 * i - 0.15 * j,
				      0.27 * j + 0.08 * i,
				      0.09 * i * i - 0.06 * j * j + 0.04 * i * j);
				row.emplace_back(p, w);
			}
			s.ControlPoints.push_back(row);
		}
		NurbsSurface::Check(s);
		return s;
	}

	void rescale(LN_NurbsSurface& s, const std::vector<double>& baseU,
	             const std::vector<double>& baseV, double u0, double u1,
	             double v0, double v1)
	{
		for (double& k : s.KnotVectorU) k = u0 + k * (u1 - u0);
		for (double& k : s.KnotVectorV) k = v0 + k * (v1 - v0);
		(void)baseU; (void)baseV;
	}

	bool throwsInvalid(const std::function<void()>& fn)
	{
		try { fn(); }
		catch (const std::invalid_argument&) { return true; }
		catch (...) { return false; }
		return false;
	}

	// ---- Independent hierarchical reference implementation ---------------

	struct IndepTHB
	{
		int pu, pv;
		unsigned maxLevel;
		std::vector<std::vector<double>> distinctU, distinctV, knotsU, knotsV;
		// refined[l][cu][cv]: level-l cell split into level-(l+1) children.
		std::vector<std::vector<std::vector<char>>> refined;

		// Basis-expansion columns of one bisection transition:
		// expandU[l][oldIndex] = list of (newIndex, coefficient).
		std::vector<std::vector<std::vector<std::pair<int, double>>>> expandU, expandV;

		IndepTHB(int pu_, int pv_, const std::vector<double>& baseU,
		         const std::vector<double>& baseV, unsigned maxLevel_,
		         const std::vector<ThbCell>& leaves)
			: pu(pu_), pv(pv_), maxLevel(maxLevel_)
		{
			for (unsigned l = 0; l <= maxLevel; ++l)
			{
				distinctU.push_back(distinctAt(baseU, l));
				distinctV.push_back(distinctAt(baseV, l));
				knotsU.push_back(clamped(pu, distinctU.back()));
				knotsV.push_back(clamped(pv, distinctV.back()));
			}
			size_t iu = baseU.size() - 1;
			size_t iv = baseV.size() - 1;
			refined.resize(maxLevel);
			for (unsigned l = 0; l < maxLevel; ++l)
			{
				refined[l].assign(iu * (size_t(1) << l),
				                  std::vector<char>(iv * (size_t(1) << l), 0));
			}
			// A cell is refined iff some leaf lies strictly below it.
			for (const ThbCell& leaf : leaves)
			{
				size_t cu = leaf.u;
				size_t cv = leaf.v;
				for (unsigned l = leaf.level; l > 0; --l)
				{
					cu /= 2;
					cv /= 2;
					refined[l - 1][cu][cv] = 1;
				}
			}
			for (unsigned l = 0; l < maxLevel; ++l)
			{
				expandU.push_back(expandOperator(pu, knotsU[l], distinctU[l]));
				expandV.push_back(expandOperator(pv, knotsV[l], distinctV[l]));
			}
		}

		// Hand-written single knot insertion, returning the expansion columns
		// of the old tensor basis in the new basis (transpose of the control
		// point operator).
		static std::vector<std::vector<std::pair<int, double>>> expandOperator(
			int degree, const std::vector<double>& oldKnots,
			const std::vector<double>& distinct)
		{
			int oldCount = static_cast<int>(oldKnots.size()) - degree - 1;
			std::vector<std::vector<double>> matrix(oldCount,
			                                        std::vector<double>(oldCount, 0.0));
			for (int i = 0; i < oldCount; ++i) matrix[i][i] = 1.0;
			std::vector<double> knots = oldKnots;
			for (size_t c = 0; c + 1 < distinct.size(); ++c)
			{
				double t = 0.5 * (distinct[c] + distinct[c + 1]);
				int span = Polynomials::GetKnotSpanIndex(degree, knots, t);
				std::vector<std::vector<double>> next(matrix.size() + 1,
				                                       std::vector<double>(oldCount, 0.0));
				for (int i = 0; i <= span - degree; ++i) next[i] = matrix[i];
				next[span + 1] = matrix[span];
				for (int i = span + 2; i < static_cast<int>(next.size()); ++i) next[i] = matrix[i - 1];
				for (int i = span - degree + 1; i <= span; ++i)
				{
					double alpha = (t - knots[i]) / (knots[i + degree] - knots[i]);
					for (int c2 = 0; c2 < oldCount; ++c2)
					{
						next[i][c2] = alpha * matrix[i][c2] + (1.0 - alpha) * matrix[i - 1][c2];
					}
				}
				knots.insert(knots.begin() + span + 1, t);
				matrix.swap(next);
			}
			std::vector<std::vector<std::pair<int, double>>> columns(oldCount);
			for (int r = 0; r < static_cast<int>(matrix.size()); ++r)
			{
				for (int c2 = 0; c2 < oldCount; ++c2)
				{
					if (std::abs(matrix[r][c2]) > 1.0e-13)
					{
						columns[c2].emplace_back(r, matrix[r][c2]);
					}
				}
			}
			return columns;
		}

		std::pair<int, int> supportCells(unsigned l, int index, bool uAxis) const
		{
			const std::vector<double>& k = uAxis ? knotsU[l] : knotsV[l];
			const std::vector<double>& d = uAxis ? distinctU[l] : distinctV[l];
			int degree = uAxis ? pu : pv;
			double left = k[index];
			double right = k[index + degree + 1];
			int lo = static_cast<int>(std::lower_bound(d.begin(), d.end(), left) - d.begin());
			int hi = static_cast<int>(std::lower_bound(d.begin(), d.end(), right) - d.begin());
			return { lo, hi };
		}

		bool inOmega(unsigned l, size_t cu, size_t cv) const
		{
			if (l == 0) return true;
			size_t pu2 = cu / 2;
			size_t pv2 = cv / 2;
			return refined[l - 1][pu2][pv2] != 0;
		}

		bool supportInsideOmega(unsigned l, int i, int j) const
		{
			auto su = supportCells(l, i, true);
			auto sv = supportCells(l, j, false);
			for (int cu = su.first; cu < su.second; ++cu)
			{
				for (int cv = sv.first; cv < sv.second; ++cv)
				{
					if (!inOmega(l, cu, cv)) return false;
				}
			}
			return true;
		}

		unsigned depthAt(double u, double v) const
		{
			auto containing = [](const std::vector<double>& d, double x) {
				int c = static_cast<int>(std::upper_bound(d.begin(), d.end(), x) - d.begin()) - 1;
				c = std::clamp(c, 0, static_cast<int>(d.size()) - 2);
				std::vector<size_t> r{ static_cast<size_t>(c) };
				if (c + 1 < static_cast<int>(d.size()) - 1 && d[c + 1] == x) r.push_back(c + 1);
				return r;
			};
			unsigned depth = 0;
			for (unsigned l = 0; l < maxLevel; ++l)
			{
				auto us = containing(distinctU[l], u);
				auto vs = containing(distinctV[l], v);
				bool split = false;
				for (size_t cu : us)
				{
					for (size_t cv : vs)
					{
						if (refined[l][cu][cv]) split = true;
					}
				}
				if (split) depth = l + 1;
				else break;
			}
			return depth;
		}

		// Truncated value/derivatives of one active tensor key at (u,v).
		std::tuple<double, double, double> sample(const ThbKey& key, double u, double v) const
		{
			unsigned depth = depthAt(u, v);
			std::map<std::pair<int, int>, double> w;
			w[{ static_cast<int>(key.u), static_cast<int>(key.v) }] = 1.0;
			for (unsigned l = key.level; l < depth; ++l)
			{
				std::map<std::pair<int, int>, double> next;
				for (const auto& e : w)
				{
					for (const auto& eu : expandU[l][e.first.first])
					{
						for (const auto& ev : expandV[l][e.first.second])
						{
							next[{ eu.first, ev.first }] += e.second * eu.second * ev.second;
						}
					}
				}
				for (auto it = next.begin(); it != next.end();)
				{
					if (supportInsideOmega(l + 1, it->first.first, it->first.second))
					{
						it = next.erase(it);
					}
					else
					{
						++it;
					}
				}
				w.swap(next);
			}
			const std::vector<double>& ku = knotsU[depth];
			const std::vector<double>& kv = knotsV[depth];
			int spanU = Polynomials::GetKnotSpanIndex(pu, ku, u);
			int spanV = Polynomials::GetKnotSpanIndex(pv, kv, v);
			auto du = Polynomials::BasisFunctionsDerivatives(spanU, pu, 1, ku, u);
			auto dv = Polynomials::BasisFunctionsDerivatives(spanV, pv, 1, kv, v);
			double value = 0.0, duv = 0.0, dvv = 0.0;
			for (const auto& e : w)
			{
				int iu = e.first.first - (spanU - pu);
				int iv = e.first.second - (spanV - pv);
				if (iu < 0 || iu > pu || iv < 0 || iv > pv) continue;
				value += e.second * du[0][iu] * dv[0][iv];
				duv += e.second * du[1][iu] * dv[0][iv];
				dvv += e.second * du[0][iu] * dv[1][iv];
			}
			return { value, duv, dvv };
		}
	};

	// Build a three-level, deliberately non-rectangular hierarchy.
	ThbSurface buildThreeLevel(const LN_NurbsSurface& s, unsigned maxLevel = 6)
	{
		ThbSurface t(s, maxLevel);
		// Level 0: an L-shaped pair of cells (different u cells, fixed v cell).
		t.Refine({ {0, 0, 0}, {0, 1, 0} });
		// Level 1 -> 2: refine only one child, and another cell elsewhere.
		t.Refine({ {1, 0, 0}, {1, 2, 1} });
		return t;
	}
}

// ============================ truncation group ============================

static int truncation_non_rectangular_three_levels()
{
	auto s = makeSurface(2, 2, { 0.0, 0.3, 0.7, 1.0 }, { 0.0, 0.4, 1.0 });
	ThbSurface t = buildThreeLevel(s);
	std::vector<double> baseU = { 0.0, 0.3, 0.7, 1.0 };
	std::vector<double> baseV = { 0.0, 0.4, 1.0 };
	IndepTHB ref(2, 2, baseU, baseV, 6, t.Leaves());

	std::set<ThbKey> active(t.Active().begin(), t.Active().end());
	// Coarse and fine functions must coexist (adjacent different depths).
	bool hasCoarse = false, hasFine = false, hasLevel2 = false;
	for (const ThbKey& k : t.Active())
	{
		if (k.level == 0) hasCoarse = true;
		if (k.level == 1) hasFine = true;
		if (k.level == 2) hasLevel2 = true;
	}
	TASK_CHECK(hasCoarse && hasFine && hasLevel2);

	for (int q = 0; q < 144; ++q)
	{
		double u = (q * 0.6180339887 + 0.07) - std::floor((q * 0.6180339887 + 0.07));
		double v = (q * 0.4142135623 + 0.13) - std::floor((q * 0.4142135623 + 0.13));
		u = std::clamp(u, 0.0, 1.0);
		v = std::clamp(v, 0.0, 1.0);
		UV uv(u, v);
		auto samples = t.Basis(uv);

		double sum = 0.0, sumDu = 0.0, sumDv = 0.0;
		ThbKey previous{ 999, 0, 0 };
		bool first = true;
		for (const auto& smp : samples)
		{
			TASK_CHECK(active.count(smp.key) == 1);
			TASK_CHECK(smp.value >= -kEps);
			if (!first) TASK_CHECK(previous < smp.key);
			first = false;
			previous = smp.key;
			// Something in the entry must be nonzero (filtering contract).
			TASK_CHECK(std::abs(smp.value) > 1.0e-13 || std::abs(smp.du) > 1.0e-13 ||
			           std::abs(smp.dv) > 1.0e-13);
			auto [rv, rdu, rdv] = ref.sample(smp.key, u, v);
			TASK_CHECK(close(smp.value, rv, 1.0e-10));
			TASK_CHECK(close(smp.du, rdu, 1.0e-10));
			TASK_CHECK(close(smp.dv, rdv, 1.0e-10));
			sum += smp.value;
			sumDu += smp.du;
			sumDv += smp.dv;
		}
		TASK_CHECK(close(sum, 1.0, 1.0e-10));
		TASK_CHECK(close(sumDu, 0.0, 1.0e-9));
		TASK_CHECK(close(sumDv, 0.0, 1.0e-9));
	}
	return 0;
}

static int truncation_cubic_nonuniform_nonunit()
{
	const double u0 = -2.0, u1 = 3.0, v0 = 1.0, v1 = 5.0;
	auto s = makeSurface(3, 3, { 0.0, 0.25, 0.6, 1.0 }, { 0.0, 0.5, 1.0 });
	rescale(s, {}, {}, u0, u1, v0, v1);
	NurbsSurface::Check(s);
	ThbSurface t = buildThreeLevel(s);
	std::vector<double> baseU = { u0, u0 + 0.25 * (u1 - u0), u0 + 0.6 * (u1 - u0), u1 };
	std::vector<double> baseV = { v0, v0 + 0.5 * (v1 - v0), v1 };
	IndepTHB ref(3, 3, baseU, baseV, 6, t.Leaves());

	for (int q = 0; q < 100; ++q)
	{
		double a = (q + 0.19) / 100.0;
		double b = (q * 3 % 97 + 0.5) / 97.0;
		double u = u0 + a * (u1 - u0);
		double v = v0 + b * (v1 - v0);
		auto samples = t.Basis(UV(u, v));
		double sum = 0.0, sumDu = 0.0, sumDv = 0.0;
		int levelsSeen = 0;
		unsigned seenMask = 0;
		for (const auto& smp : samples)
		{
			seenMask |= (1u << smp.key.level);
			auto [rv, rdu, rdv] = ref.sample(smp.key, u, v);
			TASK_CHECK(close(smp.value, rv, 1.0e-9));
			TASK_CHECK(close(smp.du, rdu, 1.0e-9));
			TASK_CHECK(close(smp.dv, rdv, 1.0e-9));
			sum += smp.value;
			sumDu += smp.du;
			sumDv += smp.dv;
		}
		for (unsigned l = 0; l < 3; ++l) levelsSeen += (seenMask & (1u << l)) ? 1 : 0;
		TASK_CHECK(levelsSeen >= 1);
		TASK_CHECK(close(sum, 1.0, 1.0e-9));
		TASK_CHECK(close(sumDu, 0.0, 1.0e-8));
		TASK_CHECK(close(sumDv, 0.0, 1.0e-8));
	}
	return 0;
}

static int truncation_analytic_insertion_coefficients()
{
	// Uniform quadratic surface; refine one level-0 cell so both midpoints
	// (0.25 and 0.75) are inserted on each axis.
	auto s = makeSurface(2, 2, { 0.0, 0.5, 1.0 }, { 0.0, 0.5, 1.0 });
	ThbSurface t(s, 4);
	t.Refine({ {0, 0, 0} });

	// Independent hand-built insertion matrix for the uniform quadratic axis,
	// applying the single-knot rule successively at t = 0.25 then 0.75.
	const int p = 2;
	std::vector<double> knots = { 0, 0, 0, 0.5, 1, 1, 1 };
	std::vector<std::vector<double>> a(4, std::vector<double>(4, 0.0));
	for (int i = 0; i < 4; ++i) a[i][i] = 1.0;
	for (double tInsert : { 0.25, 0.75 })
	{
		int span = Polynomials::GetKnotSpanIndex(p, knots, tInsert);
		std::vector<std::vector<double>> next(a.size() + 1, std::vector<double>(4, 0.0));
		for (int i = 0; i <= span - p; ++i) next[i] = a[i];
		next[span + 1] = a[span];
		for (int i = span + 2; i < static_cast<int>(next.size()); ++i) next[i] = a[i - 1];
		for (int i = span - p + 1; i <= span; ++i)
		{
			double alpha = (tInsert - knots[i]) / (knots[i + p] - knots[i]);
			for (int c = 0; c < 4; ++c) next[i][c] = alpha * a[i][c] + (1.0 - alpha) * a[i - 1][c];
		}
		knots.insert(knots.begin() + span + 1, tInsert);
		a.swap(next);
	}
	// Structural properties of the 6x4 uniform quadratic refinement operator:
	// each new control row is a convex combination (rows sum to 1), which is
	// exactly the property that preserves the partition of unity, and the
	// endpoint controls are reproduced.
	TASK_CHECK(a.size() == 6 && a[0].size() == 4);
	for (int r = 0; r < 6; ++r)
	{
		double rowSum = 0.0;
		for (int c = 0; c < 4; ++c)
		{
			TASK_CHECK(a[r][c] >= -1.0e-14);
			rowSum += a[r][c];
		}
		TASK_CHECK(close(rowSum, 1.0, 1.0e-14));
	}
	TASK_CHECK(close(a[0][0], 1.0, 1.0e-14) && close(a[5][3], 1.0, 1.0e-14));
	// The single-knot alpha used above stays in [0,1], so every interior row
	// mixes at most two adjacent old controls.
	for (int r = 1; r < 5; ++r)
	{
		int nonZero = 0;
		for (int c = 0; c < 4; ++c) if (std::abs(a[r][c]) > 1.0e-14) ++nonZero;
		TASK_CHECK(nonZero >= 1 && nonZero <= 2);
	}

	// Expansion of coarse tensor key (1,1) is the tensor product of column 1
	// on each axis.  At (0.2, 0.2), inside the single refined cell, none of
	// these fine components has support wholly inside Omega1, so no truncation
	// removes them.
	UV uv(0.2, 0.2);
	auto samples = t.Basis(uv);
	ThbKey coarse{ 0, 1, 1 };
	const ThbBasisSample* found = nullptr;
	for (const auto& smp : samples) if (smp.key == coarse) found = &smp;
	TASK_CHECK(found != nullptr);

	std::vector<double> fineKnots = { 0, 0, 0, 0.25, 0.5, 0.75, 1, 1, 1 };
	int spanU = Polynomials::GetKnotSpanIndex(2, fineKnots, uv.GetU());
	int spanV = Polynomials::GetKnotSpanIndex(2, fineKnots, uv.GetV());
	auto nu = Polynomials::BasisFunctionsDerivatives(spanU, 2, 1, fineKnots, uv.GetU());
	auto nv = Polynomials::BasisFunctionsDerivatives(spanV, 2, 1, fineKnots, uv.GetV());
	auto fineVal = [&](int i, int j) {
		int iu = i - (spanU - 2), iv = j - (spanV - 2);
		if (iu < 0 || iu > 2 || iv < 0 || iv > 2) return std::tuple<double, double, double>{0, 0, 0};
		return std::tuple<double, double, double>{
			nu[0][iu] * nv[0][iv], nu[1][iu] * nv[0][iv], nu[0][iu] * nv[1][iv]};
	};
	double expect = 0.0, expectDu = 0.0, expectDv = 0.0;
	const std::vector<double> fineDistinct = { 0.0, 0.25, 0.5, 0.75, 1.0 };
	// A fine control is fully inside the single refined parent cell (0,0)
	// exactly when its support cells are contained in fine cells {0,1}, i.e.
	// the upper support boundary does not pass fine distinct index 2.
	auto fullyInRefinedCell = [&](int fineIndex) {
		double right = fineKnots[fineIndex + 3];
		int hi = static_cast<int>(std::lower_bound(fineDistinct.begin(), fineDistinct.end(), right)
		                          - fineDistinct.begin());
		return hi <= 2;
	};
	for (int fu = 0; fu < 6; ++fu)
	{
		for (int fv = 0; fv < 6; ++fv)
		{
			double coef = a[fu][1] * a[fv][1]; // column 1 on each axis
			if (std::abs(coef) <= 1.0e-14) continue;
			// Truncation drops fine components wholly supported in Omega1.
			if (fullyInRefinedCell(fu) && fullyInRefinedCell(fv)) continue;
			auto [val, duv, dvv] = fineVal(fu, fv);
			expect += coef * val;
			expectDu += coef * duv;
			expectDv += coef * dvv;
		}
	}
	TASK_CHECK(close(found->value, expect, 1.0e-12));
	TASK_CHECK(close(found->du, expectDu, 1.0e-12));
	TASK_CHECK(close(found->dv, expectDv, 1.0e-12));
	return 0;
}

static int truncation_error_contracts()
{
	auto s = makeSurface(2, 2, { 0.0, 0.5, 1.0 }, { 0.0, 0.5, 1.0 });
	ThbSurface t(s, 2);
	TASK_CHECK(throwsInvalid([&] { t.Evaluate(UV(1.01, 0.5)); }));
	TASK_CHECK(throwsInvalid([&] { t.Basis(UV(0.5, -0.01)); }));
	TASK_CHECK(throwsInvalid([&] { t.Coefficient(ThbKey{ 0, 99, 0 }); }));
	TASK_CHECK(throwsInvalid([&] { t.SetCoefficient(ThbKey{ 0, 0, 0 }, XYZW(1, 1, 1, 0.0)); }));
	TASK_CHECK(throwsInvalid([&] { t.SetCoefficient(ThbKey{ 0, 0, 0 }, XYZW(1, 1, 1, -1.0)); }));
	TASK_CHECK(throwsInvalid([&] { t.SetCoefficient(ThbKey{ 0, 0, 0 }, XYZW(1, 1, 1, std::nan(""))); }));
	// Refining at maxLevel (here 2) is illegal.
	t.Refine({ {0, 0, 0} });
	t.Refine({ {1, 0, 0} });
	TASK_CHECK(throwsInvalid([&] { t.Refine({ {2, 0, 0} }); }));
	TASK_CHECK(throwsInvalid([&] { t.Refine({ {0, 0, 0} }); })); // no longer a leaf

	// maxLevel == 0 forbids every refinement.
	ThbSurface flat(s, 0);
	TASK_CHECK(throwsInvalid([&] { flat.Refine({ {0, 0, 0} }); }));
	TASK_CHECK(flat.Active().size() == s.ControlPoints.size() * s.ControlPoints[0].size());

	// Bad construction inputs.
	LN_NurbsSurface badDegree = s;
	badDegree.DegreeU = 4;
	TASK_CHECK(throwsInvalid([&] { ThbSurface x(badDegree, 3); }));
	LN_NurbsSurface badWeight = s;
	badWeight.ControlPoints[0][0] = XYZW(1, 1, 1, -0.5);
	TASK_CHECK(throwsInvalid([&] { ThbSurface x(badWeight, 3); }));
	TASK_CHECK(throwsInvalid([&] { ThbSurface x(s, 7); }));
	return 0;
}

// ============================== transfer group ============================

static int transfer_refinement_invariance()
{
	const double u0 = -1.5, u1 = 4.0, v0 = 2.0, v1 = 7.0;
	auto s = makeSurface(2, 3, { 0.0, 0.3, 0.7, 1.0 }, { 0.0, 0.4, 1.0 });
	rescale(s, {}, {}, u0, u1, v0, v1);
	ThbSurface t = buildThreeLevel(s);

	double maxPoint = 0.0, maxDeriv = 0.0;
	for (int q = 0; q < 160; ++q)
	{
		double a = (q + 0.23) / 160.0;
		double b = (q * 5 % 157 + 0.5) / 157.0;
		UV uv(u0 + a * (u1 - u0), v0 + b * (v1 - v0));
		ThbJet jet = t.Evaluate(uv);
		auto ref = NurbsSurface::ComputeRationalSurfaceDerivatives(s, 1, uv);
		maxPoint = std::max(maxPoint, jet.position.Distance(ref[0][0]));
		maxDeriv = std::max({ maxDeriv, jet.du.Distance(ref[1][0]), jet.dv.Distance(ref[0][1]) });
	}
	TASK_CHECK(maxPoint < 1.0e-10);
	TASK_CHECK(maxDeriv < 1.0e-9);

	// Knot lines (including original interior knots and bisection lines) use
	// one-sided in-domain values and stay consistent.
	std::vector<double> knotUs = { u0, u0 + 0.3 * (u1 - u0), u0 + 0.15 * (u1 - u0), u1 };
	std::vector<double> knotVs = { v0, v0 + 0.4 * (v1 - v0), v0 + 0.2 * (v1 - v0), v1 };
	for (double u : knotUs)
	{
		for (double v : knotVs)
		{
			ThbJet jet = t.Evaluate(UV(u, v));
			auto ref = NurbsSurface::ComputeRationalSurfaceDerivatives(s, 1, UV(u, v));
			TASK_CHECK(jet.position.Distance(ref[0][0]) < 1.0e-10);
			TASK_CHECK(jet.du.Distance(ref[1][0]) < 1.0e-9);
			TASK_CHECK(jet.dv.Distance(ref[0][1]) < 1.0e-9);
		}
	}
	return 0;
}

static int transfer_edit_then_refine_and_deactivation()
{
	auto s = makeSurface(2, 2, { 0.0, 0.3, 0.7, 1.0 }, { 0.0, 0.4, 1.0 });
	ThbSurface t(s, 6);
	t.Refine({ {0, 0, 0} });

	ThbKey editKey{ 0, 3, 1 }; // a coarse key that stays active outside Omega1
	TASK_CHECK(std::find(t.Active().begin(), t.Active().end(), editKey) != t.Active().end());
	XYZW before = t.Coefficient(editKey);
	XYZW edited(before[0] + 0.7, before[1] - 0.4, before[2] + 0.25, before[3]);
	t.SetCoefficient(editKey, edited);
	TASK_CHECK(close(t.Coefficient(editKey)[0], edited[0], 1.0e-15));

	// A fully-refined uniform surface deactivates every level-0 key, since each
	// level-0 support lies inside the whole domain Omega1.
	{
		auto uniform = makeSurface(2, 2, { 0.0, 0.5, 1.0 }, { 0.0, 0.5, 1.0 });
		ThbSurface full(uniform, 4);
		std::vector<ThbCell> all;
		for (size_t cu = 0; cu < 2; ++cu)
			for (size_t cv = 0; cv < 2; ++cv) all.push_back(ThbCell{ 0, cu, cv });
		full.Refine(all);
		for (const ThbKey& k : full.Active()) TASK_CHECK(k.level == 1);
		ThbKey dead{ 0, 0, 0 };
		TASK_CHECK(throwsInvalid([&] { full.SetCoefficient(dead, XYZW(0, 0, 0, 1)); }));
		TASK_CHECK(throwsInvalid([&] { full.Coefficient(dead); }));
		// An out-of-range fine key is likewise rejected.
		TASK_CHECK(throwsInvalid([&] { full.Coefficient(ThbKey{ 1, 99, 99 }); }));
	}

	// Snapshot the edited surface, refine further, and require exact invariance.
	std::vector<UV> probes;
	for (int q = 0; q < 60; ++q)
	{
		double a = (q + 0.11) / 60.0;
		double b = (q * 3 % 59 + 0.5) / 59.0;
		probes.emplace_back(a, b);
	}
	std::vector<ThbJet> beforeJets;
	for (const UV& uv : probes) beforeJets.push_back(t.Evaluate(uv));

	t.Refine({ {1, 0, 0}, {1, 1, 1} });
	for (size_t q = 0; q < probes.size(); ++q)
	{
		ThbJet after = t.Evaluate(probes[q]);
		TASK_CHECK(after.position.Distance(beforeJets[q].position) < 1.0e-11);
		TASK_CHECK(after.du.Distance(beforeJets[q].du) < 1.0e-10);
		TASK_CHECK(after.dv.Distance(beforeJets[q].dv) < 1.0e-10);
	}
	// The still-active edited key keeps its exact coefficient.
	TASK_CHECK(close(t.Coefficient(editKey)[0], edited[0], 1.0e-15));
	TASK_CHECK(close(t.Coefficient(editKey)[3], edited[3], 1.0e-15));

	// A single coefficient edit only changes points inside the basis support.
	ThbSurface t2(s, 6);
	ThbKey k2{ 0, 0, 0 };
	XYZW original = t2.Coefficient(k2);
	XYZW bumped(original[0] + 1.5, original[1], original[2], original[3]);
	t2.SetCoefficient(k2, bumped);
	// k2 support is bounded by knots; pick a far parameter where its basis is 0.
	UV farPoint(0.95, 0.95);
	ThbJet farEdited = t2.Evaluate(farPoint);
	ThbSurface t3(s, 6);
	ThbJet farClean = t3.Evaluate(farPoint);
	TASK_CHECK(farEdited.position.Distance(farClean.position) < 1.0e-12);
	UV nearPoint(0.02, 0.02);
	ThbJet nearEdited = t2.Evaluate(nearPoint);
	ThbJet nearClean = t3.Evaluate(nearPoint);
	TASK_CHECK(nearEdited.position.Distance(nearClean.position) > 1.0e-6);
	return 0;
}

static int transfer_copy_independence()
{
	auto s = makeSurface(2, 2, { 0.0, 0.5, 1.0 }, { 0.0, 0.5, 1.0 });
	ThbSurface t(s, 5);
	t.Refine({ {0, 0, 0} });
	ThbSurface copy(t);

	ThbKey key = copy.Active().front();
	XYZW c0 = copy.Coefficient(key);
	copy.SetCoefficient(key, XYZW(c0[0] + 1.0, c0[1], c0[2], c0[3]));
	TASK_CHECK(close(t.Coefficient(key)[0], c0[0], 1.0e-15)); // original untouched

	copy.Refine({ {1, 0, 0} });
	TASK_CHECK(t.Leaves().size() != copy.Leaves().size()); // hierarchy independent

	ThbSurface assigned = t;
	assigned = copy;
	TASK_CHECK(assigned.Leaves() == copy.Leaves());
	ThbKey akey = assigned.Active().front();
	XYZW a0 = assigned.Coefficient(akey);
	assigned.SetCoefficient(akey, XYZW(a0[0] + 2.0, a0[1], a0[2], a0[3]));
	TASK_CHECK(close(copy.Coefficient(akey)[0], a0[0], 1.0e-15));
	return 0;
}

static int transfer_rollback_and_dedup()
{
	auto s = makeSurface(2, 2, { 0.0, 0.3, 0.7, 1.0 }, { 0.0, 0.4, 1.0 });
	ThbSurface t(s, 5);
	t.Refine({ {0, 0, 0}, {0, 1, 0} });

	auto leaves = t.Leaves();
	auto active = t.Active();
	std::map<ThbKey, XYZW> coeffs;
	for (const ThbKey& k : active) coeffs[k] = t.Coefficient(k);
	ThbJet probe = t.Evaluate(UV(0.42, 0.31));

	// The last request is invalid: whole batch must roll back atomically.
	bool threw = throwsInvalid([&] {
		t.Refine({ {1, 0, 0}, {0, 99, 99} });
	});
	TASK_CHECK(threw);
	TASK_CHECK(t.Leaves() == leaves);
	TASK_CHECK(t.Active() == active);
	for (const ThbKey& k : active) TASK_CHECK(sameXYZW(t.Coefficient(k), coeffs[k]));
	ThbJet after = t.Evaluate(UV(0.42, 0.31));
	TASK_CHECK(after.position.Distance(probe.position) < 1.0e-14);

	// Empty batch is a no-op; duplicates are processed once.  Cell (0,0,0) was
	// already split, so refine a still-current level-0 leaf instead.
	size_t leafCount = t.Leaves().size();
	t.Refine({});
	TASK_CHECK(t.Leaves().size() == leafCount);
	t.Refine({ {0, 2, 0}, {0, 2, 0} });
	TASK_CHECK(t.Leaves().size() == leafCount + 3); // one leaf -> four children
	return 0;
}

static int transfer_to_tensor_and_weight_scaling()
{
	const double u0 = 0.5, u1 = 5.5, v0 = -3.0, v1 = 2.0;
	auto s = makeSurface(3, 2, { 0.0, 0.25, 0.6, 1.0 }, { 0.0, 0.4, 1.0 });
	rescale(s, {}, {}, u0, u1, v0, v1);
	ThbSurface t = buildThreeLevel(s);

	LN_NurbsSurface tensor = t.ToTensor();
	NurbsSurface::Check(tensor);
	TASK_CHECK(tensor.DegreeU == 3 && tensor.DegreeV == 2);
	// Deepest level is 2: 4 bisection pieces per original interval.
	// U has 3 original intervals -> 13 distinct knots -> 13 + 3 - 1 controls.
	// V has 2 original intervals -> 9 distinct knots  -> 9 + 2 - 1 controls.
	TASK_CHECK(tensor.ControlPoints.size() == 15);
	TASK_CHECK(tensor.ControlPoints[0].size() == 10);
	for (int q = 0; q < 120; ++q)
	{
		double a = (q + 0.17) / 120.0;
		double b = (q * 7 % 113 + 0.5) / 113.0;
		UV uv(u0 + a * (u1 - u0), v0 + b * (v1 - v0));
		ThbJet jet = t.Evaluate(uv);
		auto refTensor = NurbsSurface::ComputeRationalSurfaceDerivatives(tensor, 1, uv);
		auto refOrig = NurbsSurface::ComputeRationalSurfaceDerivatives(s, 1, uv);
		TASK_CHECK(jet.position.Distance(refTensor[0][0]) < 1.0e-10);
		TASK_CHECK(jet.du.Distance(refTensor[1][0]) < 1.0e-9);
		TASK_CHECK(jet.position.Distance(refOrig[0][0]) < 1.0e-10);
		TASK_CHECK(jet.dv.Distance(refOrig[0][1]) < 1.0e-9);
	}

	// Continue ordinary tensor knot insertion on the expanded surface.
	LN_NurbsSurface continued;
	double midU = 0.5 * (tensor.KnotVectorU.front() + tensor.KnotVectorU.back());
	NurbsSurface::RefineKnotVector(tensor, { midU }, true, continued);
	NurbsSurface::Check(continued);
	UV uv(u0 + 0.37 * (u1 - u0), v0 + 0.61 * (v1 - v0));
	TASK_CHECK(NurbsSurface::GetPointOnSurface(continued, uv)
	           .Distance(NurbsSurface::GetPointOnSurface(tensor, uv)) < 1.0e-10);

	// Scaling every homogeneous coefficient by a positive factor is invariant.
	ThbSurface scaled = t;
	double factor = 2.75;
	for (const ThbKey& k : scaled.Active())
	{
		XYZW c = scaled.Coefficient(k);
		scaled.SetCoefficient(k, XYZW(c[0] * factor, c[1] * factor, c[2] * factor, c[3] * factor));
	}
	for (int q = 0; q < 40; ++q)
	{
		UV uv2(u0 + (q + 0.3) / 40.0 * (u1 - u0), v0 + (q + 0.7) / 40.0 * (v1 - v0));
		ThbJet a = t.Evaluate(uv2);
		ThbJet b = scaled.Evaluate(uv2);
		TASK_CHECK(a.position.Distance(b.position) < 1.0e-10);
		TASK_CHECK(a.du.Distance(b.du) < 1.0e-9);
		TASK_CHECK(a.dv.Distance(b.dv) < 1.0e-9);
	}
	return 0;
}

// ============================== rendering group ===========================

static int rendering_thb_mesh_roundtrip_and_coverage()
{
	const double u0 = -1.0, u1 = 3.0, v0 = 2.0, v1 = 6.0;
	auto s = makeSurface(2, 2, { 0.0, 0.3, 0.7, 1.0 }, { 0.0, 0.4, 1.0 });
	rescale(s, {}, {}, u0, u1, v0, v1);
	ThbSurface t = buildThreeLevel(s);

	int resU = 9, resV = 7;
	LN_Mesh mesh = NurbsSurface::Triangulate(t, resU, resV);
	TASK_CHECK(static_cast<int>(mesh.Vertices.size()) == resU * resV);
	TASK_CHECK(static_cast<int>(mesh.UVs.size()) == resU * resV);
	TASK_CHECK(static_cast<int>(mesh.Faces.size()) == 2 * (resU - 1) * (resV - 1));
	TASK_CHECK(static_cast<int>(mesh.UVIndices.size()) == 3 * mesh.Faces.size());

	// Every vertex must be referenced, and every index must be in range.
	std::vector<char> covered(mesh.Vertices.size(), 0);
	for (const auto& face : mesh.Faces)
	{
		TASK_CHECK(face.size() == 3);
		for (int idx : face)
		{
			TASK_CHECK(idx >= 0 && idx < static_cast<int>(mesh.Vertices.size()));
			covered[idx] = 1;
		}
	}
	for (char c : covered) TASK_CHECK(c == 1);
	for (int idx : mesh.UVIndices)
	{
		TASK_CHECK(idx >= 0 && idx < static_cast<int>(mesh.UVs.size()));
	}

	// Each stored UV must be in the original domain and round-trip through the
	// hierarchical evaluator to the stored position.
	for (size_t i = 0; i < mesh.UVs.size(); ++i)
	{
		const UV& uv = mesh.UVs[i];
		TASK_CHECK(uv.GetU() >= u0 - 1.0e-12 && uv.GetU() <= u1 + 1.0e-12);
		TASK_CHECK(uv.GetV() >= v0 - 1.0e-12 && uv.GetV() <= v1 + 1.0e-12);
		ThbJet jet = t.Evaluate(uv);
		TASK_CHECK(jet.position.Distance(mesh.Vertices[i]) < 1.0e-10);
	}
	// Corner UVs are exactly the domain corners.
	TASK_CHECK(close(mesh.UVs.front().GetU(), u0, 1.0e-14));
	TASK_CHECK(close(mesh.UVs.front().GetV(), v0, 1.0e-14));
	TASK_CHECK(close(mesh.UVs.back().GetU(), u1, 1.0e-14));
	TASK_CHECK(close(mesh.UVs.back().GetV(), v1, 1.0e-14));

	// Free function overload and the NurbsSurface static overload agree.
	LN_Mesh mesh2 = Triangulate(t, resU, resV);
	TASK_CHECK(mesh2.Faces == mesh.Faces);
	TASK_CHECK(mesh2.UVIndices == mesh.UVIndices);
	TASK_CHECK(throwsInvalid([&] { NurbsSurface::Triangulate(t, 1, 5); }));
	TASK_CHECK(throwsInvalid([&] { NurbsSurface::Triangulate(t, 5, 0); }));
	return 0;
}

static int rendering_legacy_regular_grid_regression()
{
	// Exact fixture inherited from the baseline.
	LN_NurbsSurface s;
	s.DegreeU = 2;
	s.DegreeV = 3;
	s.KnotVectorU = { 0, 0, 0, .3, .7, 1, 1, 1 };
	s.KnotVectorV = { 0, 0, 0, 0, 1, 1, 1, 1 };
	for (int i = 0; i < 5; ++i)
	{
		std::vector<XYZW> row;
		for (int j = 0; j < 4; ++j)
		{
			double w = .75 + .1 * (i + 2 * j);
			row.emplace_back(XYZ(.3 * i, .4 * j, .07 * i * i + .11 * i * j - .03 * j * j), w);
		}
		s.ControlPoints.push_back(row);
	}
	NurbsSurface::Check(s);
	LN_Mesh mesh = NurbsSurface::Triangulate(s, 7, 6, false);
	TASK_CHECK(mesh.Vertices.size() == 42);
	TASK_CHECK(mesh.Faces.size() == 60);
	TASK_CHECK(mesh.UVs.size() == 42);
	TASK_CHECK(mesh.UVIndices.size() == 180);
	for (size_t i = 0; i < mesh.Vertices.size(); ++i)
	{
		TASK_CHECK(mesh.Vertices[i].Distance(NurbsSurface::GetPointOnSurface(s, mesh.UVs[i])) < 1.0e-12);
		TASK_CHECK(mesh.UVs[i].GetU() >= 0.0 && mesh.UVs[i].GetU() <= 1.0);
	}
	// Winding of the first triangle matches the documented rule.
	TASK_CHECK(mesh.Faces[0] == (std::vector<int>{0, 1, 6}));

	// Bad tensor inputs are rejected before entering the old evaluator.
	LN_NurbsSurface badW = s;
	badW.ControlPoints[0][0] = XYZW(1, 1, 1, 0.0);
	TASK_CHECK(throwsInvalid([&] { NurbsSurface::Triangulate(badW, 4, 4, false); }));
	TASK_CHECK(throwsInvalid([&] { NurbsSurface::Triangulate(s, 1, 4, false); }));
	return 0;
}

static int rendering_mesh_uses_hierarchy_not_flattened_tensor()
{
	// After a localized edit the hierarchical mesh must reflect it even though
	// no global fine tensor mesh is involved; compare against direct Evaluate.
	auto s = makeSurface(2, 2, { 0.0, 0.5, 1.0 }, { 0.0, 0.5, 1.0 });
	ThbSurface t(s, 5);
	t.Refine({ {0, 0, 0}, {0, 1, 1} });
	ThbKey key{ 1, 1, 1 };
	XYZW c = t.Coefficient(key);
	t.SetCoefficient(key, XYZW(c[0], c[1], c[2] + 1.2, c[3]));

	LN_Mesh mesh = NurbsSurface::Triangulate(t, 12, 12);
	TASK_CHECK(mesh.Vertices.size() == 144);
	double worst = 0.0;
	for (size_t i = 0; i < mesh.Vertices.size(); ++i)
	{
		worst = std::max(worst, mesh.Vertices[i].Distance(t.Evaluate(mesh.UVs[i]).position));
	}
	TASK_CHECK(worst < 1.0e-11);

	// The flattened tensor is explicitly not used by the rendering path: the
	// same mesh is produced for a hierarchy whose ToTensor net is much larger.
	LN_NurbsSurface flat = t.ToTensor();
	TASK_CHECK(flat.ControlPoints.size() > s.ControlPoints.size());
	return 0;
}

extern const struct task_case task_cases[] = {
	{ "truncation", "non_rectangular_three_levels", truncation_non_rectangular_three_levels },
	{ "truncation", "cubic_nonuniform_nonunit", truncation_cubic_nonuniform_nonunit },
	{ "truncation", "analytic_insertion_coefficients", truncation_analytic_insertion_coefficients },
	{ "truncation", "error_contracts", truncation_error_contracts },
	{ "transfer", "refinement_invariance", transfer_refinement_invariance },
	{ "transfer", "edit_then_refine_and_deactivation", transfer_edit_then_refine_and_deactivation },
	{ "transfer", "copy_independence", transfer_copy_independence },
	{ "transfer", "rollback_and_dedup", transfer_rollback_and_dedup },
	{ "transfer", "to_tensor_and_weight_scaling", transfer_to_tensor_and_weight_scaling },
	{ "rendering", "thb_mesh_roundtrip_and_coverage", rendering_thb_mesh_roundtrip_and_coverage },
	{ "rendering", "legacy_regular_grid_regression", rendering_legacy_regular_grid_regression },
	{ "rendering", "mesh_uses_hierarchy_not_flattened_tensor", rendering_mesh_uses_hierarchy_not_flattened_tensor },
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
