/*
 * Truncated Hierarchical B-spline (THB) rational surface.
 *
 * The hierarchy is built on top of an ordinary open-clamped tensor-product
 * NURBS surface.  Level 0 is the original tensor space; every finer level is
 * obtained by inserting the midpoint of every non-zero knot interval once on
 * each axis.  Refinement is driven by leaf cells, and the same hierarchical
 * state backs local coefficient editing, hierarchical evaluation and the
 * display mesh.
 *
 * The basis functions exposed here are polynomial truncated THB basis
 * functions (they are non-negative, form a partition of unity and their
 * first derivatives sum to zero).  Rational evaluation happens in
 * homogeneous coefficient space with the quotient rule.
 */

#pragma once

#include "LNLibDefinitions.h"
#include "LNObject.h"
#include "UV.h"
#include "XYZ.h"
#include "XYZW.h"
#include <cstddef>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace LNLib
{
	/// Control coefficient location inside one level's tensor basis.
	/// u and v index the (u,v) tensor control grid of that level, from 0.
	struct LNLIB_EXPORT ThbKey
	{
		unsigned level = 0;
		size_t u = 0;
		size_t v = 0;

		bool operator==(const ThbKey& other) const
		{
			return level == other.level && u == other.u && v == other.v;
		}
		bool operator!=(const ThbKey& other) const
		{
			return !(*this == other);
		}
		bool operator<(const ThbKey& other) const
		{
			if (level != other.level) return level < other.level;
			if (u != other.u) return u < other.u;
			return v < other.v;
		}
	};

	/// Non-zero knot interval location inside one level.
	/// u and v index the two axes' non-zero knot intervals, from 0.
	struct LNLIB_EXPORT ThbCell
	{
		unsigned level = 0;
		size_t u = 0;
		size_t v = 0;

		bool operator==(const ThbCell& other) const
		{
			return level == other.level && u == other.u && v == other.v;
		}
		bool operator!=(const ThbCell& other) const
		{
			return !(*this == other);
		}
		bool operator<(const ThbCell& other) const
		{
			if (level != other.level) return level < other.level;
			if (u != other.u) return u < other.u;
			return v < other.v;
		}
	};

	/// One active truncated basis function at a parameter point.
	struct LNLIB_EXPORT ThbBasisSample
	{
		ThbKey key;
		double value = 0.0;
		double du = 0.0;
		double dv = 0.0;
	};

		/// Rational surface jet at a parameter point.
	struct LNLIB_EXPORT ThbJet
	{
		XYZ position;
		XYZ du;
		XYZ dv;
	};

	/// Sparse knot-insertion operator rows: (column, coefficient) pairs.
	using ThbSparseRows = std::vector<std::vector<std::pair<int, double>>>;

	struct ThbSupportRange
	{
		int loCell;
		int hiCell; // exclusive
	};

	class LNLIB_EXPORT ThbSurface
	{
	public:

		/// Build a hierarchy over an independent copy of the given surface.
		/// Degrees on both axes must be 2 or 3, knot vectors must be open
		/// clamped with simple interior knots, and every weight must be
		/// finite and strictly positive.
		explicit ThbSurface(const LN_NurbsSurface& surface, unsigned maxLevel = 6);

		ThbSurface(const ThbSurface&) = default;
		ThbSurface(ThbSurface&&) = default;
		ThbSurface& operator=(const ThbSurface&) = default;
		ThbSurface& operator=(ThbSurface&&) = default;
		~ThbSurface() = default;

		/// All current leaf cells, sorted by (level,u,v).
		std::vector<ThbCell> Leaves() const;

		/// All currently active (editable) basis keys, sorted by (level,u,v).
		std::vector<ThbKey> Active() const;

		/// Homogeneous coefficient of an active key.  Throws std::invalid_argument
		/// for unknown or deactivated keys.
		XYZW Coefficient(ThbKey key) const;

		/// Replace the homogeneous coefficient of an active key.  The first
		/// three components are already weighted; w must be finite and positive.
		/// Throws std::invalid_argument for unknown, deactivated or invalid input.
		void SetCoefficient(ThbKey key, const XYZW& coefficient);

		/// Split each requested current leaf into four children.  Duplicate
		/// requests inside the batch are processed once; all other leaves stay
		/// untouched.  Throws std::invalid_argument when a request is not a
		/// current leaf, exceeds maxLevel, or is malformed; in that case the
		/// whole state is left unchanged.  An empty batch is a no-op.
		void Refine(const std::vector<ThbCell>& cells);

		/// Active truncated basis functions at uv, sorted by (level,u,v).
		/// Only entries whose value or first derivative is nonzero are listed.
		/// Throws std::invalid_argument outside the original domain.
		std::vector<ThbBasisSample> Basis(UV uv) const;

		/// Rational position and first derivatives, evaluated directly on the
		/// hierarchical state (no global fine tensor mesh).
		ThbJet Evaluate(UV uv) const;

		/// Exact expansion into a single tensor NURBS surface.  Degrees are
		/// unchanged; the knot vectors are the full bisection-refined vectors
		/// corresponding to the deepest current leaves.
		LN_NurbsSurface ToTensor() const;

		/// Original parameter domain.
		double MinU() const;
		double MaxU() const;
		double MinV() const;
		double MaxV() const;
		unsigned MaxLevel() const;

	private:

		struct LevelData
		{
			std::vector<double> knotsU;
			std::vector<double> knotsV;
			std::vector<double> distinctU;
			std::vector<double> distinctV;
			std::vector<ThbSupportRange> supportU;
			std::vector<ThbSupportRange> supportV;
		};

		std::vector<LevelData> m_levels;
		unsigned m_maxLevel = 0;
		int m_degrees[2] = { 0, 0 };

		// Refined-cell grid per transition l -> l+1 (indexed by level-l cells).
		std::vector<std::vector<std::vector<char>>> m_refined;
		std::vector<ThbCell> m_leaves;

		// Sparse bisection knot-insertion operators per transition and axis.
		// m_refine* stores rows (fine control point from old control points);
		// m_expand* stores columns (old tensor basis expanded in fine bases).
		std::vector<ThbSparseRows> m_refineU;
		std::vector<ThbSparseRows> m_refineV;
		std::vector<ThbSparseRows> m_expandU;
		std::vector<ThbSparseRows> m_expandV;

		// Homogeneous coefficients of the active truncated basis functions.
		std::map<ThbKey, XYZW> m_coefficients;
		std::vector<ThbKey> m_active;
		std::set<ThbKey> m_activeSet;

		bool cellInOmega(unsigned level, size_t cu, size_t cv) const;
		bool supportInOmega(unsigned l, size_t i, size_t j) const;
		bool supportInNextOmega(unsigned l, size_t i, size_t j) const;
		void rebuildActive();
		std::vector<std::vector<std::vector<XYZW>>> assembleGrids(unsigned upToLevel) const;
		std::vector<ThbBasisSample> evaluateBasis(UV uv) const;
		void validateInDomain(UV uv) const;
		const LevelData& level(unsigned l) const;
		LevelData& level(unsigned l);
	};

	/// Regular triangulation over the original domain, sampling the hierarchy
	/// directly.  Both resolutions must be at least 2.  Vertices, domain UVs
	/// and triangle indices share the same grid assembly as the tensor rule.
	LN_Mesh LNLIB_EXPORT Triangulate(const ThbSurface& surface, int resolutionU, int resolutionV);

	/// Shared regular-grid index assembly used by both the tensor NURBS rule
	/// and the hierarchical overload.  positions/domainUVs are flattened in the
	/// (u-major, v-minor) order used by the triangulation rules.
	LN_Mesh LNLIB_EXPORT AssembleRegularGridMesh(int resolutionU, int resolutionV,
	        const std::vector<XYZ>& positions,
	        const std::vector<UV>& domainUVs);
}
