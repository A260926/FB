/*
 * Truncated Hierarchical B-spline (THB) rational surface implementation.
 *
 * Level spaces are nested tensor NURBS spaces: level l+1 is obtained from
 * level l by inserting the midpoint of every non-zero knot interval once on
 * each axis.  Refinement matrices (knot insertion operators) are precomputed
 * per axis and level.  Truncation removes every finer tensor component whose
 * complete support lies inside the next hierarchical domain.
 */

#include "ThbSurface.h"
#include "NurbsSurface.h"
#include "LNObject.h"
#include "Polynomials.h"
#include "ValidationUtils.h"
#include "Constants.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace LNLib
{
	namespace
	{
		constexpr double kZeroTol = 1.0e-13;

		bool finiteXYZW(const XYZW& p)
		{
			return std::isfinite(p[0]) && std::isfinite(p[1]) &&
			       std::isfinite(p[2]) && std::isfinite(p[3]);
		}

		// Distinct knot values of an open clamped knot vector.
		std::vector<double> distinctKnots(const std::vector<double>& knots)
		{
			std::vector<double> result;
			for (double k : knots)
			{
				if (result.empty() || result.back() != k)
				{
					result.push_back(k);
				}
			}
			return result;
		}

		// Full open clamped knot vector for a level built on distinct values.
		std::vector<double> clampedKnots(int degree, const std::vector<double>& distinct)
		{
			std::vector<double> knots;
			knots.reserve(distinct.size() + 2 * degree);
			for (int i = 0; i <= degree; ++i) knots.push_back(distinct.front());
			for (size_t i = 1; i + 1 < distinct.size(); ++i) knots.push_back(distinct[i]);
			for (int i = 0; i <= degree; ++i) knots.push_back(distinct.back());
			return knots;
		}

		// Distinct level-l knot values: every level-0 interval split into 2^l.
		std::vector<double> levelDistinct(const std::vector<double>& base, unsigned level)
		{
			size_t pieces = static_cast<size_t>(1) << level;
			std::vector<double> result;
			result.reserve((base.size() - 1) * pieces + 1);
			for (size_t i = 0; i + 1 < base.size(); ++i)
			{
				for (size_t k = 0; k < pieces; ++k)
				{
					result.push_back(base[i] + (base[i + 1] - base[i]) *
					                 (static_cast<double>(k) / static_cast<double>(pieces)));
				}
			}
			result.push_back(base.back());
			return result;
		}

		// Single knot insertion matrix A (new control points = A * old control
		// points), built by inserting every element of insertedKnots once.
		std::vector<std::vector<double>> insertionMatrix(
			int degree,
			const std::vector<double>& oldKnots,
			const std::vector<double>& insertedKnots)
		{
			std::vector<std::vector<double>> matrix(oldKnots.size() - degree - 1,
			                                        std::vector<double>(oldKnots.size() - degree - 1, 0.0));
			for (size_t i = 0; i < matrix.size(); ++i) matrix[i][i] = 1.0;

			std::vector<double> knots = oldKnots;
			for (double t : insertedKnots)
			{
				int span = Polynomials::GetKnotSpanIndex(degree, knots, t);
				std::vector<std::vector<double>> next(matrix.size() + 1,
				                                       std::vector<double>(matrix[0].size(), 0.0));
				for (int i = 0; i <= span - degree; ++i) next[i] = matrix[i];
				next[span + 1] = matrix[span];
				for (int i = span + 2; i < static_cast<int>(next.size()); ++i) next[i] = matrix[i - 1];
				for (int i = span - degree + 1; i <= span; ++i)
				{
					double alpha = (t - knots[i]) / (knots[i + degree] - knots[i]);
					for (size_t c = 0; c < next[i].size(); ++c)
					{
						next[i][c] = alpha * matrix[i][c] + (1.0 - alpha) * matrix[i - 1][c];
					}
				}
				knots.insert(knots.begin() + span + 1, t);
				matrix.swap(next);
			}
			return matrix;
		}

		ThbSparseRows sparsify(const std::vector<std::vector<double>>& dense)
		{
			ThbSparseRows sparse(dense.size());
			for (size_t r = 0; r < dense.size(); ++r)
			{
				for (size_t c = 0; c < dense[r].size(); ++c)
				{
					if (std::abs(dense[r][c]) > kZeroTol)
					{
						sparse[r].emplace_back(static_cast<int>(c), dense[r][c]);
					}
				}
			}
			return sparse;
		}

		ThbSparseRows transposeSparse(const ThbSparseRows& rows, int columns)
		{
			ThbSparseRows result(columns);
			for (size_t r = 0; r < rows.size(); ++r)
			{
				for (const auto& entry : rows[r])
				{
					result[entry.first].emplace_back(static_cast<int>(r), entry.second);
				}
			}
			return result;
		}

		std::vector<ThbSupportRange> supportRanges(int degree,
		                                        const std::vector<double>& knots,
		                                        const std::vector<double>& distinct)
		{
			std::vector<ThbSupportRange> ranges(knots.size() - degree - 1);
			for (size_t i = 0; i < ranges.size(); ++i)
			{
				double left = knots[i];
				double right = knots[i + degree + 1];
				int lo = static_cast<int>(std::lower_bound(distinct.begin(), distinct.end(), left) - distinct.begin());
				int hi = static_cast<int>(std::lower_bound(distinct.begin(), distinct.end(), right) - distinct.begin());
				ranges[i] = { lo, hi };
			}
			return ranges;
		}
	}

	ThbSurface::ThbSurface(const LN_NurbsSurface& surface, unsigned maxLevel)
	{
		if (maxLevel > 6)
		{
			throw std::invalid_argument("ThbSurface: maxLevel must be between 0 and 6.");
		}
		if (surface.DegreeU != 2 && surface.DegreeU != 3)
		{
			throw std::invalid_argument("ThbSurface: degree U must be 2 or 3.");
		}
		if (surface.DegreeV != 2 && surface.DegreeV != 3)
		{
			throw std::invalid_argument("ThbSurface: degree V must be 2 or 3.");
		}
		if (surface.ControlPoints.empty() || surface.ControlPoints[0].empty())
		{
			throw std::invalid_argument("ThbSurface: control net is empty.");
		}
		if (!ValidationUtils::IsValidNurbs(surface.DegreeU,
		        static_cast<int>(surface.KnotVectorU.size()),
		        static_cast<int>(surface.ControlPoints.size())) ||
		    !ValidationUtils::IsValidNurbs(surface.DegreeV,
		        static_cast<int>(surface.KnotVectorV.size()),
		        static_cast<int>(surface.ControlPoints[0].size())))
		{
			throw std::invalid_argument("ThbSurface: knot vector/control net dimensions are inconsistent.");
		}
		if (!ValidationUtils::IsValidKnotVector(surface.KnotVectorU) ||
		    !ValidationUtils::IsValidKnotVector(surface.KnotVectorV))
		{
			throw std::invalid_argument("ThbSurface: knot vectors must be nondecreasing.");
		}

		auto checkAxis = [](int degree, const std::vector<double>& knots) {
			if (static_cast<int>(knots.size()) < 2 * (degree + 1))
			{
				throw std::invalid_argument("ThbSurface: knot vector is too short.");
			}
			double low = knots.front();
			double high = knots.back();
			if (!(std::isfinite(low) && std::isfinite(high)) || !(high > low))
			{
				throw std::invalid_argument("ThbSurface: parameter domain is invalid.");
			}
			for (int i = 0; i <= degree; ++i)
			{
				if (knots[i] != low || knots[knots.size() - 1 - i] != high)
				{
					throw std::invalid_argument("ThbSurface: knot vector must be open clamped.");
				}
			}
			std::vector<double> distinct = distinctKnots(knots);
			for (size_t i = 1; i < distinct.size(); ++i)
			{
				if (!(distinct[i] > distinct[i - 1]))
				{
					throw std::invalid_argument("ThbSurface: interior knots must be simple.");
				}
			}
			std::vector<double> canonical = clampedKnots(degree, distinct);
			if (canonical != knots)
			{
				throw std::invalid_argument("ThbSurface: interior knots must have multiplicity one.");
			}
		};
		checkAxis(surface.DegreeU, surface.KnotVectorU);
		checkAxis(surface.DegreeV, surface.KnotVectorV);

		for (const auto& row : surface.ControlPoints)
		{
			if (row.size() != surface.ControlPoints[0].size())
			{
				throw std::invalid_argument("ThbSurface: control net must be rectangular.");
			}
			for (const XYZW& p : row)
			{
				if (!finiteXYZW(p) || !(p.GetW() > 0.0))
				{
					throw std::invalid_argument("ThbSurface: weights must be finite and strictly positive.");
				}
			}
		}

		m_maxLevel = maxLevel;

		std::vector<double> baseDistinctU = distinctKnots(surface.KnotVectorU);
		std::vector<double> baseDistinctV = distinctKnots(surface.KnotVectorV);

		m_levels.reserve(maxLevel + 1);
		for (unsigned l = 0; l <= maxLevel; ++l)
		{
			LevelData data;
			data.distinctU = levelDistinct(baseDistinctU, l);
			data.distinctV = levelDistinct(baseDistinctV, l);
			data.knotsU = clampedKnots(surface.DegreeU, data.distinctU);
			data.knotsV = clampedKnots(surface.DegreeV, data.distinctV);
			data.supportU = supportRanges(surface.DegreeU, data.knotsU, data.distinctU);
			data.supportV = supportRanges(surface.DegreeV, data.knotsV, data.distinctV);
			m_levels.push_back(std::move(data));
		}

		// Level-0 coefficients are an independent copy of the input surface.
		m_degrees[0] = surface.DegreeU;
		m_degrees[1] = surface.DegreeV;
		m_coefficients.clear();
		for (size_t i = 0; i < surface.ControlPoints.size(); ++i)
		{
			for (size_t j = 0; j < surface.ControlPoints[0].size(); ++j)
			{
				m_coefficients[ThbKey{ 0, i, j }] = surface.ControlPoints[i][j];
			}
		}

		size_t intervalsU = baseDistinctU.size() - 1;
		size_t intervalsV = baseDistinctV.size() - 1;
		m_refined.resize(maxLevel);
		for (unsigned l = 0; l < maxLevel; ++l)
		{
			size_t cu = intervalsU * (static_cast<size_t>(1) << l);
			size_t cv = intervalsV * (static_cast<size_t>(1) << l);
			m_refined[l].assign(cu, std::vector<char>(cv, 0));
		}

		m_leaves.clear();
		for (size_t i = 0; i < intervalsU; ++i)
		{
			for (size_t j = 0; j < intervalsV; ++j)
			{
				m_leaves.push_back(ThbCell{ 0, i, j });
			}
		}
		std::sort(m_leaves.begin(), m_leaves.end());

		// Precompute bisection knot-insertion operators for every transition.
		m_refineU.clear();
		m_refineV.clear();
		m_expandU.clear();
		m_expandV.clear();
		for (unsigned l = 0; l < maxLevel; ++l)
		{
			std::vector<double> midU;
			std::vector<double> midV;
			for (size_t i = 0; i + 1 < m_levels[l].distinctU.size(); ++i)
			{
				midU.push_back(0.5 * (m_levels[l].distinctU[i] + m_levels[l].distinctU[i + 1]));
			}
			for (size_t i = 0; i + 1 < m_levels[l].distinctV.size(); ++i)
			{
				midV.push_back(0.5 * (m_levels[l].distinctV[i] + m_levels[l].distinctV[i + 1]));
			}
			ThbSparseRows rowsU = sparsify(insertionMatrix(m_degrees[0], m_levels[l].knotsU, midU));
			ThbSparseRows rowsV = sparsify(insertionMatrix(m_degrees[1], m_levels[l].knotsV, midV));
			int oldCountU = static_cast<int>(m_levels[l].knotsU.size()) - m_degrees[0] - 1;
			int oldCountV = static_cast<int>(m_levels[l].knotsV.size()) - m_degrees[1] - 1;
			m_refineU.push_back(rowsU);
			m_refineV.push_back(rowsV);
			m_expandU.push_back(transposeSparse(rowsU, oldCountU));
			m_expandV.push_back(transposeSparse(rowsV, oldCountV));
		}

		rebuildActive();
	}

	double ThbSurface::MinU() const { return m_levels[0].knotsU.front(); }
	double ThbSurface::MaxU() const { return m_levels[0].knotsU.back(); }
	double ThbSurface::MinV() const { return m_levels[0].knotsV.front(); }
	double ThbSurface::MaxV() const { return m_levels[0].knotsV.back(); }
	unsigned ThbSurface::MaxLevel() const { return m_maxLevel; }

	const ThbSurface::LevelData& ThbSurface::level(unsigned l) const { return m_levels.at(l); }
	ThbSurface::LevelData& ThbSurface::level(unsigned l) { return m_levels.at(l); }

	void ThbSurface::validateInDomain(UV uv) const
	{
		if (!std::isfinite(uv.GetU()) || !std::isfinite(uv.GetV()) ||
		    uv.GetU() < MinU() - Constants::DoubleEpsilon || uv.GetU() > MaxU() + Constants::DoubleEpsilon ||
		    uv.GetV() < MinV() - Constants::DoubleEpsilon || uv.GetV() > MaxV() + Constants::DoubleEpsilon)
		{
			throw std::invalid_argument("ThbSurface: parameter is outside the original domain.");
		}
	}

	bool ThbSurface::cellInOmega(unsigned level, size_t cu, size_t cv) const
	{
		if (level == 0) return true;
		if (level > m_maxLevel) return false;
		const auto& grid = m_refined[level - 1];
		size_t parentU = cu / 2;
		size_t parentV = cv / 2;
		if (parentU >= grid.size() || parentV >= grid[0].size()) return false;
		return grid[parentU][parentV] != 0;
	}

	bool ThbSurface::supportInOmega(unsigned l, size_t i, size_t j) const
	{
		if (l == 0) return true;
		if (l > m_maxLevel) return false;
		const LevelData& data = m_levels[l];
		const ThbSupportRange& ru = data.supportU[i];
		const ThbSupportRange& rv = data.supportV[j];
		for (int cu = ru.loCell; cu < ru.hiCell; ++cu)
		{
			for (int cv = rv.loCell; cv < rv.hiCell; ++cv)
			{
				if (!cellInOmega(l, static_cast<size_t>(cu), static_cast<size_t>(cv)))
				{
					return false;
				}
			}
		}
		return true;
	}

	// Whether the complete level-l tensor support of control (i,j) lies in
	// Omega_{l+1}, i.e. every level-l cell of its support was refined.
	bool ThbSurface::supportInNextOmega(unsigned l, size_t i, size_t j) const
	{
		if (l >= m_maxLevel) return false; // Omega_{maxLevel+1} is empty
		const LevelData& data = m_levels[l];
		const ThbSupportRange& ru = data.supportU[i];
		const ThbSupportRange& rv = data.supportV[j];
		const auto& grid = m_refined[l];
		for (int cu = ru.loCell; cu < ru.hiCell; ++cu)
		{
			for (int cv = rv.loCell; cv < rv.hiCell; ++cv)
			{
				if (grid[cu][cv] == 0)
				{
					return false;
				}
			}
		}
		return true;
	}

	void ThbSurface::rebuildActive()
	{
		m_active.clear();
		m_activeSet.clear();
		for (unsigned l = 0; l <= m_maxLevel; ++l)
		{
			size_t countU = m_levels[l].knotsU.size() - m_degrees[0] - 1;
			size_t countV = m_levels[l].knotsV.size() - m_degrees[1] - 1;
			for (size_t i = 0; i < countU; ++i)
			{
				for (size_t j = 0; j < countV; ++j)
				{
					if (supportInOmega(l, i, j) && !supportInNextOmega(l, i, j))
					{
						ThbKey key{ l, i, j };
						m_active.push_back(key);
						m_activeSet.insert(key);
					}
				}
			}
		}
		std::sort(m_active.begin(), m_active.end());
	}

	std::vector<ThbCell> ThbSurface::Leaves() const
	{
		return m_leaves;
	}

	std::vector<ThbKey> ThbSurface::Active() const
	{
		return m_active;
	}

	XYZW ThbSurface::Coefficient(ThbKey key) const
	{
		auto it = m_coefficients.find(key);
		if (it == m_coefficients.end() || m_activeSet.find(key) == m_activeSet.end())
		{
			throw std::invalid_argument("ThbSurface: key is unknown or has been deactivated.");
		}
		return it->second;
	}

	void ThbSurface::SetCoefficient(ThbKey key, const XYZW& coefficient)
	{
		if (m_activeSet.find(key) == m_activeSet.end())
		{
			throw std::invalid_argument("ThbSurface: key is unknown or has been deactivated.");
		}
		if (!finiteXYZW(coefficient) || !(coefficient.GetW() > 0.0))
		{
			throw std::invalid_argument("ThbSurface: coefficient must be finite with a positive weight.");
		}
		m_coefficients[key] = coefficient;
	}

	namespace
	{
		// Refine a homogeneous coefficient grid one level along both axes.
		std::vector<std::vector<XYZW>> refineGrid(
			const std::vector<std::vector<XYZW>>& grid,
			const ThbSparseRows& refineU,
			const ThbSparseRows& refineV)
		{
			std::vector<std::vector<XYZW>> result(refineU.size(),
			                                      std::vector<XYZW>(refineV.size()));
			for (size_t a = 0; a < refineU.size(); ++a)
			{
				for (size_t b = 0; b < refineV.size(); ++b)
				{
					XYZW sum(0.0, 0.0, 0.0, 0.0);
					for (const auto& uEntry : refineU[a])
					{
						for (const auto& vEntry : refineV[b])
						{
							sum += grid[uEntry.first][vEntry.first] * (uEntry.second * vEntry.second);
						}
					}
					result[a][b] = sum;
				}
			}
			return result;
		}
	}

	// Snapshot of the current homogeneous function F in every level tensor
	// space, assembled from the active THB coefficients with truncation.
	std::vector<std::vector<std::vector<XYZW>>> ThbSurface::assembleGrids(unsigned upToLevel) const
	{
		std::vector<std::vector<std::vector<XYZW>>> snapshots;
		size_t countU0 = m_levels[0].knotsU.size() - m_degrees[0] - 1;
		size_t countV0 = m_levels[0].knotsV.size() - m_degrees[1] - 1;
		std::vector<std::vector<XYZW>> grid(countU0, std::vector<XYZW>(countV0));
		for (size_t i = 0; i < countU0; ++i)
		{
			for (size_t j = 0; j < countV0; ++j)
			{
				ThbKey key{ 0, i, j };
				auto it = m_coefficients.find(key);
				if (it != m_coefficients.end()) grid[i][j] = it->second;
			}
		}
		snapshots.push_back(grid);

		for (unsigned l = 0; l < upToLevel; ++l)
		{
			std::vector<std::vector<XYZW>> refined = refineGrid(grid, m_refineU[l], m_refineV[l]);
			size_t countU = refined.size();
			size_t countV = refined[0].size();
			// Truncate: drop fine components fully supported inside Omega_{l+1}.
			for (size_t i = 0; i < countU; ++i)
			{
				for (size_t j = 0; j < countV; ++j)
				{
					if (supportInOmega(l + 1, i, j))
					{
						refined[i][j] = XYZW(0.0, 0.0, 0.0, 0.0);
					}
				}
			}
			// Add the active fine-level coefficients.
			for (size_t i = 0; i < countU; ++i)
			{
				for (size_t j = 0; j < countV; ++j)
				{
					ThbKey key{ l + 1, i, j };
					auto it = m_coefficients.find(key);
					if (it != m_coefficients.end())
					{
						refined[i][j] += it->second;
					}
				}
			}
			snapshots.push_back(refined);
			grid.swap(refined);
		}
		return snapshots;
	}

	void ThbSurface::Refine(const std::vector<ThbCell>& cells)
	{
		if (cells.empty()) return;

		std::set<ThbCell> requested;
		std::set<ThbCell> leafSet(m_leaves.begin(), m_leaves.end());
		for (const ThbCell& cell : cells)
		{
			if (cell.level >= m_maxLevel)
			{
				throw std::invalid_argument("ThbSurface: refinement exceeds the maximum level.");
			}
			if (leafSet.find(cell) == leafSet.end())
			{
				throw std::invalid_argument("ThbSurface: refinement request is not a current leaf cell.");
			}
			requested.insert(cell);
		}

		unsigned targetLevel = 0;
		for (const ThbCell& cell : requested) targetLevel = std::max(targetLevel, cell.level + 1);

		// Exact homogeneous transfer built from the current (possibly edited)
		// hierarchical function; no resampling or refitting.
		std::vector<std::vector<std::vector<XYZW>>> snapshots = assembleGrids(targetLevel);

		// Work on temporary structures so a failure leaves the state intact.
		std::vector<ThbCell> newLeaves = m_leaves;
		auto newRefined = m_refined;
		for (const ThbCell& cell : requested)
		{
			newRefined[cell.level][cell.u][cell.v] = 1;
			auto it = std::lower_bound(newLeaves.begin(), newLeaves.end(), cell);
			if (it != newLeaves.end() && *it == cell)
			{
				it = newLeaves.erase(it);
				ThbCell children[4] = {
					{ cell.level + 1, 2 * cell.u,     2 * cell.v },
					{ cell.level + 1, 2 * cell.u + 1, 2 * cell.v },
					{ cell.level + 1, 2 * cell.u,     2 * cell.v + 1 },
					{ cell.level + 1, 2 * cell.u + 1, 2 * cell.v + 1 },
				};
				for (const ThbCell& child : children)
				{
					newLeaves.insert(std::lower_bound(newLeaves.begin(), newLeaves.end(), child), child);
				}
			}
		}

		std::map<ThbKey, XYZW> newCoefficients;
		for (unsigned l = 0; l <= targetLevel; ++l)
		{
			size_t countU = m_levels[l].knotsU.size() - m_degrees[0] - 1;
			size_t countV = m_levels[l].knotsV.size() - m_degrees[1] - 1;
			for (size_t i = 0; i < countU; ++i)
			{
				for (size_t j = 0; j < countV; ++j)
				{
					ThbKey key{ l, i, j };
					auto old = m_coefficients.find(key);
					if (old != m_coefficients.end() && m_activeSet.count(key) != 0)
					{
						newCoefficients[key] = old->second;
					}
					else
					{
						const XYZW& transferred = snapshots[l][i][j];
						if (std::isfinite(transferred[0]) && std::isfinite(transferred[1]) &&
						    std::isfinite(transferred[2]) && std::isfinite(transferred[3]) &&
						    transferred.GetW() > 0.0)
						{
							newCoefficients[key] = transferred;
						}
					}
				}
			}
		}

		// Commit tentatively, then verify every active key obtained a valid
		// positive-weight coefficient; roll back otherwise.
		auto oldRefined = std::move(m_refined);
		auto oldLeaves = std::move(m_leaves);
		auto oldCoefficients = std::move(m_coefficients);
		auto oldActive = std::move(m_active);
		auto oldActiveSet = std::move(m_activeSet);

		m_refined = std::move(newRefined);
		m_leaves = std::move(newLeaves);
		m_coefficients = std::move(newCoefficients);
		rebuildActive();

		bool valid = true;
		for (const ThbKey& key : m_active)
		{
			auto it = m_coefficients.find(key);
			if (it == m_coefficients.end() || !finiteXYZW(it->second) || !(it->second.GetW() > 0.0))
			{
				valid = false;
				break;
			}
		}
		if (!valid)
		{
			m_refined = std::move(oldRefined);
			m_leaves = std::move(oldLeaves);
			m_coefficients = std::move(oldCoefficients);
			m_active = std::move(oldActive);
			m_activeSet = std::move(oldActiveSet);
			throw std::invalid_argument("ThbSurface: refinement produced an invalid coefficient; state unchanged.");
		}

		// Drop coefficients that no longer correspond to an active key.
		for (auto it = m_coefficients.begin(); it != m_coefficients.end();)
		{
			if (m_activeSet.count(it->first) == 0)
			{
				it = m_coefficients.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	std::vector<ThbBasisSample> ThbSurface::evaluateBasis(UV uv) const
	{
		validateInDomain(uv);
		double u = std::clamp(uv.GetU(), MinU(), MaxU());
		double v = std::clamp(uv.GetV(), MinV(), MaxV());

		// Depth of the leaf whose closure contains the parameter.  At a knot
		// line shared by a refined and an unrefined cell the deeper side wins,
		// because only it carries the truncation.
		auto cellsContaining = [](const std::vector<double>& distinct, double x) {
			int last = static_cast<int>(distinct.size()) - 1;
			int c = static_cast<int>(std::upper_bound(distinct.begin(), distinct.end(), x) - distinct.begin()) - 1;
			c = std::clamp(c, 0, last - 1);
			std::vector<size_t> result{ static_cast<size_t>(c) };
			if (c + 1 < last && distinct[c + 1] == x)
			{
				result.push_back(static_cast<size_t>(c + 1));
			}
			return result;
		};

		unsigned depth = 0;
		for (unsigned l = 0; l < m_maxLevel; ++l)
		{
			std::vector<size_t> cellsU = cellsContaining(m_levels[l].distinctU, u);
			std::vector<size_t> cellsV = cellsContaining(m_levels[l].distinctV, v);
			bool refined = false;
			for (size_t cu : cellsU)
			{
				for (size_t cv : cellsV)
				{
					if (m_refined[l][cu][cv] != 0)
					{
						refined = true;
					}
				}
			}
			if (refined)
			{
				depth = l + 1;
			}
			else
			{
				break;
			}
		}

		struct TensorBasis
		{
			int index;
			double value;
			double derivative;
		};
		std::vector<std::vector<TensorBasis>> basesU(depth + 1);
		std::vector<std::vector<TensorBasis>> basesV(depth + 1);
		for (unsigned l = 0; l <= depth; ++l)
		{
			const LevelData& data = m_levels[l];
			int spanU = Polynomials::GetKnotSpanIndex(m_degrees[0], data.knotsU, u);
			int spanV = Polynomials::GetKnotSpanIndex(m_degrees[1], data.knotsV, v);
			auto derivU = Polynomials::BasisFunctionsDerivatives(spanU, m_degrees[0], 1, data.knotsU, u);
			auto derivV = Polynomials::BasisFunctionsDerivatives(spanV, m_degrees[1], 1, data.knotsV, v);
			for (int k = 0; k <= m_degrees[0]; ++k)
			{
				basesU[l].push_back({ spanU - m_degrees[0] + k, derivU[0][k], derivU[1][k] });
			}
			for (int k = 0; k <= m_degrees[1]; ++k)
			{
				basesV[l].push_back({ spanV - m_degrees[1] + k, derivV[0][k], derivV[1][k] });
			}
		}

		// Fast lookup of the fine-level nonzero tensor basis by global index.
		std::map<int, const TensorBasis*> fineU;
		std::map<int, const TensorBasis*> fineV;
		for (const TensorBasis& b : basesU[depth]) fineU[b.index] = &b;
		for (const TensorBasis& b : basesV[depth]) fineV[b.index] = &b;

		std::vector<ThbBasisSample> samples;
		for (unsigned l = 0; l <= depth; ++l)
		{
			for (const TensorBasis& bu : basesU[l])
			{
				for (const TensorBasis& bv : basesV[l])
				{
					ThbKey origin{ l, static_cast<size_t>(bu.index), static_cast<size_t>(bv.index) };
					if (m_activeSet.find(origin) == m_activeSet.end()) continue;

					// Expand this truncated basis down to the leaf level,
					// dropping fine components fully inside Omega_{k+1}.
					// Basis expansion uses the transpose of the (coefficient)
					// knot-insertion operator, hence m_expand*.
					std::map<std::pair<int, int>, double> weights;
					weights[{ bu.index, bv.index }] = 1.0;
					for (unsigned k = l; k < depth; ++k)
					{
						std::map<std::pair<int, int>, double> next;
						for (const auto& entry : weights)
						{
							int oldI = entry.first.first;
							int oldJ = entry.first.second;
							double w = entry.second;
							for (const auto& uEntry : m_expandU[k][oldI])
							{
								for (const auto& vEntry : m_expandV[k][oldJ])
								{
									next[{ uEntry.first, vEntry.first }] += w * uEntry.second * vEntry.second;
								}
							}
						}
						for (auto it = next.begin(); it != next.end();)
						{
							if (supportInOmega(k + 1, static_cast<size_t>(it->first.first),
							                                  static_cast<size_t>(it->first.second)))
							{
								it = next.erase(it);
							}
							else
							{
								++it;
							}
						}
						weights.swap(next);
					}

					double value = 0.0;
					double du = 0.0;
					double dv = 0.0;
					for (const auto& entry : weights)
					{
						int fineI = entry.first.first;
						int fineJ = entry.first.second;
						double w = entry.second;
						auto iu = fineU.find(fineI);
						auto iv = fineV.find(fineJ);
						// Components outside the local nonzero set vanish here.
						if (iu == fineU.end() || iv == fineV.end()) continue;
						const TensorBasis* fu = iu->second;
						const TensorBasis* fv = iv->second;
						value += w * fu->value * fv->value;
						du += w * fu->derivative * fv->value;
						dv += w * fu->value * fv->derivative;
					}

					if (std::abs(value) > kZeroTol || std::abs(du) > kZeroTol || std::abs(dv) > kZeroTol)
					{
						samples.push_back(ThbBasisSample{ origin, value, du, dv });
					}
				}
			}
		}
		std::sort(samples.begin(), samples.end(),
		          [](const ThbBasisSample& a, const ThbBasisSample& b) { return a.key < b.key; });
		return samples;
	}

	std::vector<ThbBasisSample> ThbSurface::Basis(UV uv) const
	{
		return evaluateBasis(uv);
	}

	ThbJet ThbSurface::Evaluate(UV uv) const
	{
		std::vector<ThbBasisSample> samples = evaluateBasis(uv);
		XYZW numerator(0.0, 0.0, 0.0, 0.0);
		XYZW numeratorU(0.0, 0.0, 0.0, 0.0);
		XYZW numeratorV(0.0, 0.0, 0.0, 0.0);
		for (const ThbBasisSample& sample : samples)
		{
			const XYZW& c = m_coefficients.at(sample.key);
			numerator += c * sample.value;
			numeratorU += c * sample.du;
			numeratorV += c * sample.dv;
		}
		double w = numerator.GetW();
		if (!std::isfinite(w) || !(w > 0.0))
		{
			throw std::invalid_argument("ThbSurface: non-finite or non-positive denominator during evaluation.");
		}
		XYZ position = numerator.ToXYZ(false) / w;
		XYZ du = (numeratorU.ToXYZ(false) - position * numeratorU.GetW()) / w;
		XYZ dv = (numeratorV.ToXYZ(false) - position * numeratorV.GetW()) / w;
		if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]) ||
		    !std::isfinite(du[0]) || !std::isfinite(dv[0]))
		{
			throw std::invalid_argument("ThbSurface: evaluation produced non-finite values.");
		}
		return ThbJet{ position, du, dv };
	}

	LN_NurbsSurface ThbSurface::ToTensor() const
	{
		unsigned deepest = 0;
		for (const ThbCell& cell : m_leaves) deepest = std::max(deepest, cell.level);

		std::vector<std::vector<std::vector<XYZW>>> snapshots = assembleGrids(deepest);
		const LevelData& fine = m_levels[deepest];
		LN_NurbsSurface result;
		result.DegreeU = m_degrees[0];
		result.DegreeV = m_degrees[1];
		result.KnotVectorU = fine.knotsU;
		result.KnotVectorV = fine.knotsV;
		result.ControlPoints = snapshots.back();
		return result;
	}

	LN_Mesh AssembleRegularGridMesh(int resolutionU, int resolutionV,
	                             const std::vector<XYZ>& positions,
	                             const std::vector<UV>& domainUVs)
	{
		if (resolutionU < 2 || resolutionV < 2)
		{
			throw std::invalid_argument("Triangulate: resolutions must be at least 2.");
		}
		if (static_cast<int>(positions.size()) != resolutionU * resolutionV ||
		    static_cast<int>(domainUVs.size()) != resolutionU * resolutionV)
		{
			throw std::invalid_argument("Triangulate: grid data does not match resolutions.");
		}
		LN_Mesh mesh;
		mesh.Vertices = positions;
		mesh.UVs = domainUVs;
		for (int i = 0; i < resolutionU - 1; ++i)
		{
			for (int j = 0; j < resolutionV - 1; ++j)
			{
				int idx0 = i * resolutionV + j;
				int idx1 = i * resolutionV + j + 1;
				int idx2 = (i + 1) * resolutionV + j;
				int idx3 = (i + 1) * resolutionV + j + 1;

				mesh.Faces.push_back({ idx0, idx1, idx2 });
				mesh.UVIndices.push_back(idx0);
				mesh.UVIndices.push_back(idx1);
				mesh.UVIndices.push_back(idx2);

				mesh.Faces.push_back({ idx1, idx3, idx2 });
				mesh.UVIndices.push_back(idx1);
				mesh.UVIndices.push_back(idx3);
				mesh.UVIndices.push_back(idx2);
			}
		}
		return mesh;
	}

	LN_Mesh Triangulate(const ThbSurface& surface, int resolutionU, int resolutionV)
	{
		if (resolutionU < 2 || resolutionV < 2)
		{
			throw std::invalid_argument("Triangulate: resolutions must be at least 2.");
		}
		std::vector<XYZ> positions;
		std::vector<UV> domainUVs;
		positions.reserve(resolutionU * resolutionV);
		domainUVs.reserve(resolutionU * resolutionV);
		double minU = surface.MinU();
		double maxU = surface.MaxU();
		double minV = surface.MinV();
		double maxV = surface.MaxV();
		for (int i = 0; i < resolutionU; ++i)
		{
			double u = minU + (maxU - minU) * static_cast<double>(i) / static_cast<double>(resolutionU - 1);
			for (int j = 0; j < resolutionV; ++j)
			{
				double v = minV + (maxV - minV) * static_cast<double>(j) / static_cast<double>(resolutionV - 1);
				UV uv(u, v);
				positions.push_back(surface.Evaluate(uv).position);
				domainUVs.push_back(uv);
			}
		}
		return AssembleRegularGridMesh(resolutionU, resolutionV, positions, domainUVs);
	}
}
