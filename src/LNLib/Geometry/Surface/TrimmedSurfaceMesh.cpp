/*
 * Trimmed surface triangulation for LNLib.
 *
 * The trimmed region is laid out in the unfolded parameter domain: the loops
 * are used exactly as supplied (their U coordinates may lie outside the
 * effective U domain).  A planar arrangement is built from
 *
 *   - the rectangle bounding every loop,
 *   - every periodic copy U = a + k * P of the U seam,
 *   - every periodic copy of an interior U knot line,
 *   - every interior V knot line,
 *   - the U and V lines through every loop vertex (this connects the loops to
 *     the grid so that holes become real arrangement faces),
 *   - every loop edge.
 *
 * The bounded faces of that arrangement are classified against the trimmed
 * region (the outer loop is counter-clockwise and the holes are clockwise, so
 * the region lies on the left of every directed loop edge) and every inside
 * face is triangulated.  The resulting initial constrained triangles are
 * refined adaptively (red-green refinement, which keeps the mesh conforming)
 * until the chord error at the three edge midpoints and the centroid of every
 * triangle is within tolerance.
 *
 * Only after the unfolded topology is fixed are the per-corner UVs mapped back
 * into the effective U domain.  A vertex on a seam is shared by both sides and
 * evaluated at the a-side representative, while the triangles on the left and
 * right of the seam use b and a respectively as the U coordinate of that
 * corner.  Vertices are merged by unfolded parameter coordinates only, never
 * by spatial position, so distinct unfolded points remain distinct vertices
 * even when they coincide in space.
 */

#include "TrimmedSurfaceMesh.h"

#include "NurbsSurface.h"
#include "UV.h"
#include "XYZ.h"
#include "XYZW.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace LNLib
{
namespace
{
	struct Vec2
	{
		double u = 0.0;
		double v = 0.0;
	};

	inline bool IsFinite(double value)
	{
		return std::isfinite(value);
	}

	inline Vec2 operator -(const Vec2& a, const Vec2& b)
	{
		return Vec2{ a.u - b.u, a.v - b.v };
	}

	inline Vec2 operator +(const Vec2& a, const Vec2& b)
	{
		return Vec2{ a.u + b.u, a.v + b.v };
	}

	inline Vec2 operator *(const Vec2& a, double s)
	{
		return Vec2{ a.u * s, a.v * s };
	}

	inline double Dot(const Vec2& a, const Vec2& b)
	{
		return a.u * b.u + a.v * b.v;
	}

	inline double Cross(const Vec2& a, const Vec2& b)
	{
		return a.u * b.v - a.v * b.u;
	}

	inline double Cross(const Vec2& a, const Vec2& b, const Vec2& c)
	{
		return Cross(b - a, c - a);
	}

	inline double Length(const Vec2& a)
	{
		return std::sqrt(Dot(a, a));
	}

	inline bool SamePoint(const Vec2& a, const Vec2& b)
	{
		return a.u == b.u && a.v == b.v;
	}

	inline double PolygonSignedArea(const std::vector<Vec2>& polygon)
	{
		double area = 0.0;
		const std::size_t n = polygon.size();
		for (std::size_t i = 0; i < n; ++i)
		{
			area += Cross(polygon[i], polygon[(i + 1) % n]);
		}
		return area * 0.5;
	}

	inline double PositiveModulo(double value, double modulus)
	{
		double result = std::fmod(value, modulus);
		if (result < 0.0)
		{
			result += modulus;
		}
		return result;
	}

	// ---------------------------------------------------------------------
	// Validation
	// ---------------------------------------------------------------------

	struct LoopInfo
	{
		std::vector<Vec2> points;
		double area = 0.0;
	};

	struct Region
	{
		std::vector<LoopInfo> loops;
		double minU = 0.0;
		double maxU = 0.0;
		double minV = 0.0;
		double maxV = 0.0;
		double eps = 0.0;
		double scale = 1.0;
	};

	struct SurfaceInfo
	{
		double a = 0.0;
		double b = 0.0;
		double vMin = 0.0;
		double vMax = 0.0;
		double P = 1.0;
	};

	bool PointOnSegment(const Vec2& point, const Vec2& a, const Vec2& b, double eps)
	{
		const Vec2 ab = b - a;
		const Vec2 ap = point - a;
		const double length = Length(ab);
		if (std::fabs(Cross(ab, ap)) > eps * std::max(1.0, length))
		{
			return false;
		}
		const double dot = Dot(ap, ab);
		const double lengthSquared = Dot(ab, ab);
		const double slack = eps * std::max(1.0, lengthSquared);
		return dot >= -slack && dot <= lengthSquared + slack;
	}

	enum class PointClass
	{
		Outside,
		Inside,
		Boundary
	};

	PointClass ClassifyPoint(const std::vector<Vec2>& polygon, const Vec2& point, double eps)
	{
		bool inside = false;
		const std::size_t n = polygon.size();
		for (std::size_t i = 0; i < n; ++i)
		{
			const Vec2& a = polygon[i];
			const Vec2& b = polygon[(i + 1) % n];
			if (PointOnSegment(point, a, b, eps))
			{
				return PointClass::Boundary;
			}
			if ((a.v > point.v) != (b.v > point.v))
			{
				const double intersection = a.u + (point.v - a.v) * (b.u - a.u) / (b.v - a.v);
				if (point.u < intersection)
				{
					inside = !inside;
				}
			}
		}
		return inside ? PointClass::Inside : PointClass::Outside;
	}

	bool SegmentsIntersect(const Vec2& a, const Vec2& b, const Vec2& c, const Vec2& d, double eps)
	{
		if (PointOnSegment(a, c, d, eps) || PointOnSegment(b, c, d, eps) ||
			PointOnSegment(c, a, b, eps) || PointOnSegment(d, a, b, eps))
		{
			return true;
		}
		const double d1 = Cross(a, b, c);
		const double d2 = Cross(a, b, d);
		const double d3 = Cross(c, d, a);
		const double d4 = Cross(c, d, b);
		return ((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) &&
			((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0));
	}

	// Validates the control net, the knot vectors, the weights and the closed
	// U direction.  The existing evaluator is invoked only after every
	// structural check passed.
	SurfaceInfo ValidateSurface(const LN_NurbsSurface& surface, double seamTolerance)
	{
		const int degreeU = surface.DegreeU;
		const int degreeV = surface.DegreeV;
		if (degreeU < 1 || degreeU > 3 || degreeV < 1 || degreeV > 3)
		{
			throw std::invalid_argument("surface degrees must be between 1 and 3 inclusive.");
		}
		if (surface.ControlPoints.empty())
		{
			throw std::invalid_argument("surface control net must not be empty.");
		}

		const std::size_t rows = surface.ControlPoints.size();
		const std::size_t columns = surface.ControlPoints[0].size();
		if (columns == 0)
		{
			throw std::invalid_argument("surface control net must not contain empty rows.");
		}

		double positionScale = 1.0;
		for (std::size_t i = 0; i < rows; ++i)
		{
			if (surface.ControlPoints[i].size() != columns)
			{
				throw std::invalid_argument("surface control net must be rectangular.");
			}
			for (std::size_t j = 0; j < columns; ++j)
			{
				const XYZW& cp = surface.ControlPoints[i][j];
				if (!IsFinite(cp.GetWX()) || !IsFinite(cp.GetWY()) || !IsFinite(cp.GetWZ()))
				{
					throw std::invalid_argument("surface control point coordinates must be finite.");
				}
				if (!IsFinite(cp.GetW()) || !(cp.GetW() > 0.0))
				{
					throw std::invalid_argument("surface control point weights must be positive and finite.");
				}
				const double w = cp.GetW();
				positionScale = std::max(positionScale, std::max(std::fabs(cp.GetWX() / w),
					std::max(std::fabs(cp.GetWY() / w), std::fabs(cp.GetWZ() / w))));
			}
		}

		const std::vector<double>& knotU = surface.KnotVectorU;
		const std::vector<double>& knotV = surface.KnotVectorV;
		if (knotU.size() != rows + static_cast<std::size_t>(degreeU) + 1)
		{
			throw std::invalid_argument("U knot vector size must equal the U control point count plus the U degree plus one.");
		}
		if (knotV.size() != columns + static_cast<std::size_t>(degreeV) + 1)
		{
			throw std::invalid_argument("V knot vector size must equal the V control point count plus the V degree plus one.");
		}

		auto validateKnots = [](const std::vector<double>& knots, int degree, const char* name)
		{
			for (std::size_t i = 0; i < knots.size(); ++i)
			{
				if (!IsFinite(knots[i]))
				{
					throw std::invalid_argument(std::string(name) + " knot values must be finite.");
				}
				if (i > 0 && knots[i] < knots[i - 1])
				{
					throw std::invalid_argument(std::string(name) + " knot vector must be nondecreasing.");
				}
			}
			if (!(knots.back() > knots.front()))
			{
				throw std::invalid_argument(std::string(name) + " parameter domain must have positive width.");
			}
			const std::size_t p = static_cast<std::size_t>(degree);
			for (std::size_t i = 0; i <= p; ++i)
			{
				if (knots[i] != knots.front() || knots[knots.size() - 1 - i] != knots.back())
				{
					throw std::invalid_argument(std::string(name) + " knot vector must be clamped.");
				}
			}
			std::size_t index = 0;
			while (index < knots.size())
			{
				std::size_t next = index + 1;
				while (next < knots.size() && knots[next] == knots[index])
				{
					++next;
				}
				const bool interior = knots[index] != knots.front() && knots[index] != knots.back();
				if (interior && next - index > p)
				{
					throw std::invalid_argument(std::string(name) + " interior knot multiplicity must not exceed the degree.");
				}
				index = next;
			}
		};
		validateKnots(knotU, degreeU, "U");
		validateKnots(knotV, degreeV, "V");

		SurfaceInfo info;
		info.a = knotU.front();
		info.b = knotU.back();
		info.vMin = knotV.front();
		info.vMax = knotV.back();
		info.P = info.b - info.a;

		// A grossly open U direction is outside the supported input range.  A
		// small mismatch is reported by the strict seam check instead.
		const double closureTolerance = std::max(1e-6 * positionScale, seamTolerance);
		std::vector<double> samplesV;
		samplesV.push_back(info.vMin);
		samplesV.push_back(info.vMax);
		for (std::size_t i = static_cast<std::size_t>(degreeV); i + 1 < knotV.size(); ++i)
		{
			samplesV.push_back(0.5 * (knotV[i] + knotV[i + 1]));
		}
		std::sort(samplesV.begin(), samplesV.end());
		samplesV.erase(std::unique(samplesV.begin(), samplesV.end()), samplesV.end());
		for (double v : samplesV)
		{
			const XYZ left = NurbsSurface::GetPointOnSurface(surface, UV(info.a, v));
			const XYZ right = NurbsSurface::GetPointOnSurface(surface, UV(info.b, v));
			if (!IsFinite(left.X()) || !IsFinite(right.X()) || left.Distance(right) > closureTolerance)
			{
				throw std::invalid_argument("surface U direction must be spatially closed.");
			}
		}
		return info;
	}

	void ValidateOptions(const TrimMeshOptions& options)
	{
		if (!IsFinite(options.chordTolerance) || !(options.chordTolerance > 0.0))
		{
			throw std::invalid_argument("chordTolerance must be positive and finite.");
		}
		if (!IsFinite(options.seamTolerance) || !(options.seamTolerance > 0.0))
		{
			throw std::invalid_argument("seamTolerance must be positive and finite.");
		}
	}

	void ValidateLoops(const std::vector<std::vector<UV>>& loops,
		const SurfaceInfo& info, Region& region)
	{
		if (loops.empty())
		{
			throw std::invalid_argument("at least one loop is required.");
		}

		double coordinateScale = 1.0;
		for (const std::vector<UV>& loop : loops)
		{
			for (const UV& uv : loop)
			{
				if (!IsFinite(uv.GetU()) || !IsFinite(uv.GetV()))
				{
					throw std::invalid_argument("loop coordinates must be finite.");
				}
				coordinateScale = std::max(coordinateScale,
					std::max(std::fabs(uv.GetU()), std::fabs(uv.GetV())));
			}
		}
		const double eps = 1e-12 * coordinateScale;

		region.loops.clear();
		region.minU = std::numeric_limits<double>::infinity();
		region.maxU = -std::numeric_limits<double>::infinity();
		region.minV = std::numeric_limits<double>::infinity();
		region.maxV = -std::numeric_limits<double>::infinity();
		region.eps = eps;
		region.scale = coordinateScale;

		for (std::size_t loopIndex = 0; loopIndex < loops.size(); ++loopIndex)
		{
			const std::vector<UV>& input = loops[loopIndex];
			if (input.size() < 3)
			{
				throw std::invalid_argument("every loop must contain at least three points.");
			}

			std::vector<Vec2> points;
			points.reserve(input.size());
			for (const UV& uv : input)
			{
				if (!(uv.GetV() > info.vMin && uv.GetV() < info.vMax))
				{
					throw std::invalid_argument("loop V coordinates must lie strictly inside the effective V domain.");
				}
				points.push_back(Vec2{ uv.GetU(), uv.GetV() });
			}

			for (std::size_t i = 0; i < points.size(); ++i)
			{
				if (SamePoint(points[i], points[(i + 1) % points.size()]))
				{
					throw std::invalid_argument("loop contains a zero-length segment.");
				}
			}

			std::vector<Vec2> distinct;
			for (const Vec2& point : points)
			{
				bool seen = false;
				for (const Vec2& other : distinct)
				{
					if (SamePoint(point, other))
					{
						seen = true;
						break;
					}
				}
				if (!seen)
				{
					distinct.push_back(point);
				}
			}
			if (distinct.size() < 3)
			{
				throw std::invalid_argument("every loop must contain at least three distinct points.");
			}

			const std::size_t n = points.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				const Vec2& p0 = points[i];
				const Vec2& p1 = points[(i + 1) % n];
				for (std::size_t j = i + 1; j < n; ++j)
				{
					const Vec2& q0 = points[j];
					const Vec2& q1 = points[(j + 1) % n];
					const bool adjacent = (j == i + 1) || (i == 0 && j == n - 1);
					if (adjacent)
					{
						const Vec2& shared = (j == i + 1) ? p1 : points[0];
						const Vec2& otherA = (j == i + 1) ? p0 : p1;
						const Vec2& otherB = (j == i + 1) ? q1 : q0;
						if (std::fabs(Cross(shared, otherA, otherB)) <= eps * eps &&
							Dot(otherA - shared, otherB - shared) > eps * eps)
						{
							throw std::invalid_argument("loop folds back onto itself.");
						}
						continue;
					}
					if (SegmentsIntersect(p0, p1, q0, q1, eps))
					{
						throw std::invalid_argument("loop must be simple (no self-intersections).");
					}
				}
			}

			LoopInfo loopInfo;
			loopInfo.points = points;
			loopInfo.area = PolygonSignedArea(points);
			if (loopIndex == 0)
			{
				if (!(loopInfo.area > eps * eps))
				{
					throw std::invalid_argument("the outer loop must be counter-clockwise with positive area.");
				}
			}
			else if (!(loopInfo.area < -eps * eps))
			{
				throw std::invalid_argument("hole loops must be clockwise with negative area.");
			}

			for (const Vec2& point : loopInfo.points)
			{
				region.minU = std::min(region.minU, point.u);
				region.maxU = std::max(region.maxU, point.u);
				region.minV = std::min(region.minV, point.v);
				region.maxV = std::max(region.maxV, point.v);
			}
			region.loops.push_back(std::move(loopInfo));
		}

		if (!(region.maxU - region.minU < info.P))
		{
			throw std::invalid_argument("the combined U span of all loops must be strictly smaller than one U period.");
		}

		const std::vector<Vec2>& outer = region.loops[0].points;
		for (std::size_t holeIndex = 1; holeIndex < region.loops.size(); ++holeIndex)
		{
			const std::vector<Vec2>& hole = region.loops[holeIndex].points;
			if (ClassifyPoint(outer, hole[0], eps) != PointClass::Inside)
			{
				throw std::invalid_argument("every hole must lie strictly inside the outer loop.");
			}
			for (std::size_t otherIndex = 1; otherIndex < region.loops.size(); ++otherIndex)
			{
				if (otherIndex == holeIndex)
				{
					continue;
				}
				const std::vector<Vec2>& other = region.loops[otherIndex].points;
				if (ClassifyPoint(other, hole[0], eps) == PointClass::Inside)
				{
					throw std::invalid_argument("holes must not overlap or contain each other.");
				}
			}
		}

		for (std::size_t i = 0; i < region.loops.size(); ++i)
		{
			for (std::size_t j = i + 1; j < region.loops.size(); ++j)
			{
				const std::vector<Vec2>& loopA = region.loops[i].points;
				const std::vector<Vec2>& loopB = region.loops[j].points;
				for (std::size_t ia = 0; ia < loopA.size(); ++ia)
				{
					const Vec2& a0 = loopA[ia];
					const Vec2& a1 = loopA[(ia + 1) % loopA.size()];
					for (std::size_t ib = 0; ib < loopB.size(); ++ib)
					{
						const Vec2& b0 = loopB[ib];
						const Vec2& b1 = loopB[(ib + 1) % loopB.size()];
						if (SegmentsIntersect(a0, a1, b0, b1, eps))
						{
							throw std::invalid_argument("loops must not intersect or touch each other.");
						}
					}
				}
			}
		}
	}

	// ---------------------------------------------------------------------
	// Vertex store keyed by the unfolded parameter coordinates
	// ---------------------------------------------------------------------

	class VertexStore
	{
	public:
		VertexStore(double tolerance, double cell)
			: m_tolerance(tolerance)
			, m_cell(std::max(cell, std::max(tolerance, 1e-300)))
		{
		}

		// Coordinates within `tolerance` of one of the canonical grid lines are
		// snapped exactly onto it.  This makes the vertices that both sides of a
		// seam compute through different arithmetic collapse to one index.
		void SetSnapLines(const std::vector<double>& uLines, const std::vector<double>& vLines)
		{
			m_snapULines = uLines;
			m_snapVLines = vLines;
			std::sort(m_snapULines.begin(), m_snapULines.end());
			std::sort(m_snapVLines.begin(), m_snapVLines.end());
		}

		int Add(Vec2 point)
		{
			Snap(point);
			const long long keyU = static_cast<long long>(std::floor(point.u / m_cell));
			const long long keyV = static_cast<long long>(std::floor(point.v / m_cell));
			for (long long du = -1; du <= 1; ++du)
			{
				for (long long dv = -1; dv <= 1; ++dv)
				{
					const auto found = m_buckets.find({ keyU + du, keyV + dv });
					if (found == m_buckets.end())
					{
						continue;
					}
					for (int id : found->second)
					{
						if (Length(m_points[id] - point) <= m_tolerance)
						{
							return id;
						}
					}
				}
			}
			const int id = static_cast<int>(m_points.size());
			m_points.push_back(point);
			m_buckets[{ keyU, keyV }].push_back(id);
			return id;
		}

		const std::vector<Vec2>& Points() const
		{
			return m_points;
		}

		Vec2 Point(int id) const
		{
			return m_points[id];
		}

		std::size_t Size() const
		{
			return m_points.size();
		}

		// Take over an already deduplicated point set.  Snapping may still merge
		// two coincident points, so the resolved indices are written to
		// `mapping` for the caller to translate its own indices.
		void Seed(const std::vector<Vec2>& points, std::vector<int>& mapping)
		{
			m_points.clear();
			m_buckets.clear();
			mapping.clear();
			mapping.reserve(points.size());
			for (const Vec2& point : points)
			{
				mapping.push_back(Add(point));
			}
		}

	private:
		void Snap(Vec2& point) const
		{
			auto snapOne = [](double value, const std::vector<double>& lines, double tolerance)
			{
				double best = value;
				double bestDistance = tolerance;
				for (double line : lines)
				{
					const double distance = std::fabs(value - line);
					if (distance <= bestDistance)
					{
						bestDistance = distance;
						best = line;
					}
				}
				return best;
			};
			point.u = snapOne(point.u, m_snapULines, m_tolerance);
			point.v = snapOne(point.v, m_snapVLines, m_tolerance);
		}

		double m_tolerance;
		double m_cell;
		std::vector<double> m_snapULines;
		std::vector<double> m_snapVLines;
		std::vector<Vec2> m_points;
		std::map<std::pair<long long, long long>, std::vector<int>> m_buckets;
	};

	// ---------------------------------------------------------------------
	// Planar arrangement
	// ---------------------------------------------------------------------

	struct RawSegment
	{
		Vec2 a;
		Vec2 b;
	};

	struct Arrangement
	{
		explicit Arrangement(double tolerance, double cell)
			: vertices(tolerance, cell)
		{
		}

		VertexStore vertices;
		std::vector<std::pair<int, int>> edges;
		std::vector<std::vector<int>> faces;
	};

	void AddIntersectionParameters(const RawSegment& s1, const RawSegment& s2, double eps,
		std::vector<double>& t1, std::vector<double>& t2)
	{
		const Vec2 r = s1.b - s1.a;
		const Vec2 s = s2.b - s2.a;
		const double rLength = Length(r);
		const double sLength = Length(s);
		if (rLength <= eps || sLength <= eps)
		{
			return;
		}

		const Vec2 qp = s2.a - s1.a;
		const double denominator = Cross(r, s);
		const double parallelTolerance = eps * rLength * sLength;
		const double parameterTolerance = 1e-12;

		if (std::fabs(denominator) > parallelTolerance)
		{
			const double t = Cross(qp, s) / denominator;
			const double u = Cross(qp, r) / denominator;
			if (t >= -parameterTolerance && t <= 1.0 + parameterTolerance &&
				u >= -parameterTolerance && u <= 1.0 + parameterTolerance)
			{
				t1.push_back(std::min(1.0, std::max(0.0, t)));
				t2.push_back(std::min(1.0, std::max(0.0, u)));
			}
			return;
		}

		if (std::fabs(Cross(qp, r)) > parallelTolerance)
		{
			return;
		}

		// Collinear segments: split both at the other segment's endpoints.
		const double rSquared = Dot(r, r);
		const double sSquared = Dot(s, s);
		auto addProjection = [](const Vec2& point, const Vec2& origin, const Vec2& direction,
			double directionSquared, std::vector<double>& values)
		{
			const double t = Dot(point - origin, direction) / directionSquared;
			if (t > 1e-12 && t < 1.0 - 1e-12)
			{
				values.push_back(t);
			}
		};
		addProjection(s2.a, s1.a, r, rSquared, t1);
		addProjection(s2.b, s1.a, r, rSquared, t1);
		addProjection(s1.a, s2.a, s, sSquared, t2);
		addProjection(s1.b, s2.a, s, sSquared, t2);
	}

	Vec2 Lerp(const Vec2& a, const Vec2& b, double t)
	{
		return Vec2{ a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t };
	}

	Arrangement BuildArrangement(const std::vector<RawSegment>& raw, double eps, double scale)
	{
		const double tolerance = 1e-13 * scale;
		const double cell = 1e-9 * scale;
		Arrangement arrangement(tolerance, cell);

		std::vector<std::vector<double>> parameters(raw.size());
		for (std::size_t i = 0; i < raw.size(); ++i)
		{
			parameters[i].push_back(0.0);
			parameters[i].push_back(1.0);
		}
		for (std::size_t i = 0; i < raw.size(); ++i)
		{
			for (std::size_t j = i + 1; j < raw.size(); ++j)
			{
				AddIntersectionParameters(raw[i], raw[j], eps, parameters[i], parameters[j]);
			}
		}

		std::set<std::pair<int, int>> seenEdges;
		for (std::size_t i = 0; i < raw.size(); ++i)
		{
			std::vector<double>& values = parameters[i];
			std::sort(values.begin(), values.end());
			values.erase(std::unique(values.begin(), values.end(),
				[](double x, double y) { return std::fabs(x - y) <= 1e-11; }), values.end());

			int previous = -1;
			for (double t : values)
			{
				const Vec2 point = Lerp(raw[i].a, raw[i].b, t);
				const int id = arrangement.vertices.Add(point);
				if (previous >= 0 && previous != id)
				{
					const auto key = std::minmax(previous, id);
					if (seenEdges.insert(key).second)
					{
						arrangement.edges.push_back({ key.first, key.second });
					}
				}
				previous = id;
			}
		}

		// Extract bounded faces: walk each directed half-edge and take the
		// clockwise-next edge at the arriving vertex, which keeps the face on
		// the left of the traversed edge.
		const std::vector<Vec2>& points = arrangement.vertices.Points();
		const int vertexCount = static_cast<int>(points.size());
		std::vector<std::vector<int>> adjacency(static_cast<std::size_t>(vertexCount));
		for (const auto& edge : arrangement.edges)
		{
			adjacency[edge.first].push_back(edge.second);
			adjacency[edge.second].push_back(edge.first);
		}
		for (int i = 0; i < vertexCount; ++i)
		{
			std::vector<int>& neighbours = adjacency[i];
			std::sort(neighbours.begin(), neighbours.end(), [&points, i](int lhs, int rhs)
			{
				const double angleL = std::atan2(points[lhs].v - points[i].v, points[lhs].u - points[i].u);
				const double angleR = std::atan2(points[rhs].v - points[i].v, points[rhs].u - points[i].u);
				if (angleL != angleR)
				{
					return angleL < angleR;
				}
				return lhs < rhs;
			});
		}

		std::set<std::pair<int, int>> visited;
		const std::size_t directedLimit = arrangement.edges.size() * 2 + 8;
		for (int start = 0; start < vertexCount; ++start)
		{
			for (int next : adjacency[start])
			{
				if (visited.count({ start, next }))
				{
					continue;
				}
				std::vector<int> face;
				int current = start;
				int following = next;
				std::size_t guard = 0;
				do
				{
					visited.insert({ current, following });
					face.push_back(current);
					const std::vector<int>& neighbours = adjacency[following];
					const auto position = std::find(neighbours.begin(), neighbours.end(), current);
					if (position == neighbours.end())
					{
						throw std::runtime_error("inconsistent arrangement during face extraction.");
					}
					const std::size_t index = static_cast<std::size_t>(position - neighbours.begin());
					const std::size_t previousIndex = (index + neighbours.size() - 1) % neighbours.size();
					const int nextVertex = neighbours[previousIndex];
					current = following;
					following = nextVertex;
					if (++guard > directedLimit)
					{
						throw std::runtime_error("arrangement face extraction did not terminate.");
					}
				} while (!(current == start && following == next));

				if (face.size() < 3)
				{
					continue;
				}
				double area = 0.0;
				for (std::size_t i = 0; i < face.size(); ++i)
				{
					area += Cross(points[face[i]], points[face[(i + 1) % face.size()]]);
				}
				if (area > 0.0)
				{
					arrangement.faces.push_back(std::move(face));
				}
			}
		}
		return arrangement;
	}

	// ---------------------------------------------------------------------
	// Ear clipping
	// ---------------------------------------------------------------------

	bool StrictlyInsideTriangle(const Vec2& p, const Vec2& a, const Vec2& b, const Vec2& c)
	{
		return Cross(b - a, p - a) > 0.0 && Cross(c - b, p - b) > 0.0 && Cross(a - c, p - c) > 0.0;
	}

	bool OnSegment(const Vec2& p, const Vec2& a, const Vec2& b)
	{
		if (Cross(b - a, p - a) != 0.0)
		{
			return false;
		}
		return Dot(p - a, b - a) >= 0.0 && Dot(p - b, a - b) >= 0.0;
	}

	void EarClip(const std::vector<int>& polygon, const std::vector<Vec2>& points,
		std::vector<std::array<int, 3>>& triangles)
	{
		std::vector<int> work = polygon;
		if (work.size() < 3)
		{
			return;
		}

		double area = 0.0;
		for (std::size_t i = 0; i < work.size(); ++i)
		{
			area += Cross(points[work[i]], points[work[(i + 1) % work.size()]]);
		}
		if (area < 0.0)
		{
			std::reverse(work.begin(), work.end());
		}

		// Drop collinear or duplicated vertices; they carry no shape and would
		// otherwise produce zero-area ears and ambiguous diagonals.
		bool removed = true;
		while (removed && work.size() >= 3)
		{
			removed = false;
			for (std::size_t i = 0; i < work.size(); ++i)
			{
				const Vec2& a = points[work[(i + work.size() - 1) % work.size()]];
				const Vec2& b = points[work[i]];
				const Vec2& c = points[work[(i + 1) % work.size()]];
				if (SamePoint(a, b) || SamePoint(b, c))
				{
					work.erase(work.begin() + static_cast<std::ptrdiff_t>(i));
					removed = true;
					break;
				}
				if (Cross(a, b, c) == 0.0 && Dot(b - a, c - b) > 0.0)
				{
					work.erase(work.begin() + static_cast<std::ptrdiff_t>(i));
					removed = true;
					break;
				}
			}
		}
		if (work.size() < 3)
		{
			return;
		}

		std::size_t guard = 0;
		const std::size_t guardLimit = work.size() * work.size() + 32;
		while (work.size() > 3)
		{
			const std::size_t n = work.size();
			bool clipped = false;

			for (std::size_t i = 0; i < n && !clipped; ++i)
			{
				const int previous = work[(i + n - 1) % n];
				const int current = work[i];
				const int next = work[(i + 1) % n];
				const Vec2& a = points[previous];
				const Vec2& b = points[current];
				const Vec2& c = points[next];
				if (!(Cross(a, b, c) > 0.0))
				{
					continue;
				}
				bool blocked = false;
				for (std::size_t j = 0; j < n && !blocked; ++j)
				{
					const int other = work[j];
					if (other == previous || other == current || other == next)
					{
						continue;
					}
					blocked = StrictlyInsideTriangle(points[other], a, b, c)
						|| OnSegment(points[other], a, c);
				}
				if (blocked)
				{
					continue;
				}
				triangles.push_back({ previous, current, next });
				work.erase(work.begin() + static_cast<std::ptrdiff_t>(i));
				clipped = true;
			}

			if (!clipped)
			{
				throw std::runtime_error("failed to triangulate a trimmed region face.");
			}
			if (++guard > guardLimit)
			{
				throw std::runtime_error("triangulation of a trimmed region face did not terminate.");
			}
		}

		if (work.size() == 3 && Cross(points[work[0]], points[work[1]], points[work[2]]) > 0.0)
		{
			triangles.push_back({ work[0], work[1], work[2] });
		}
	}

	// A face walk can revisit a vertex when the arrangement touches itself;
	// split such a walk into simple cycles before ear clipping.
	void TriangulateFace(const std::vector<int>& face, const std::vector<Vec2>& points,
		std::vector<std::array<int, 3>>& triangles)
	{
		std::vector<std::vector<int>> pending;
		pending.push_back(face);
		std::size_t guard = 0;
		const std::size_t guardLimit = face.size() * 2 + 32;

		while (!pending.empty())
		{
			std::vector<int> polygon = std::move(pending.back());
			pending.pop_back();
			if (polygon.size() < 3)
			{
				continue;
			}

			std::map<int, std::size_t> firstOccurrence;
			bool split = false;
			for (std::size_t i = 0; i < polygon.size(); ++i)
			{
				const auto found = firstOccurrence.find(polygon[i]);
				if (found == firstOccurrence.end())
				{
					firstOccurrence[polygon[i]] = i;
					continue;
				}
				const std::size_t first = found->second;
				std::vector<int> inner(polygon.begin() + static_cast<std::ptrdiff_t>(first),
					polygon.begin() + static_cast<std::ptrdiff_t>(i));
				std::vector<int> outer;
				outer.insert(outer.end(), polygon.begin(), polygon.begin() + static_cast<std::ptrdiff_t>(first));
				outer.insert(outer.end(), polygon.begin() + static_cast<std::ptrdiff_t>(i), polygon.end());
				if (inner.size() >= 3)
				{
					pending.push_back(std::move(inner));
				}
				if (outer.size() >= 3)
				{
					pending.push_back(std::move(outer));
				}
				split = true;
				break;
			}
			if (split)
			{
				if (++guard > guardLimit)
				{
					throw std::runtime_error("failed to split a trimmed region face.");
				}
				continue;
			}
			EarClip(polygon, points, triangles);
		}
	}

	// ---------------------------------------------------------------------
	// Mesh builder
	// ---------------------------------------------------------------------

	struct MeshFace
	{
		int v[3] = { 0, 0, 0 };
		unsigned depth = 0;
	};

	class Builder
	{
	public:
		Builder(const LN_NurbsSurface& surface, const SurfaceInfo& info, const Region& region,
			const TrimMeshOptions& options)
			: m_surface(surface)
			, m_info(info)
			, m_region(region)
			, m_options(options)
		{
			m_uvTolerance = 1e-9 * std::max(1.0, std::max(std::fabs(info.a), std::fabs(info.b)));
			m_seamSnap = 1e-10 * std::max(1.0, std::max(std::fabs(info.a), std::fabs(info.b)));
			m_vertexStore.reset(new VertexStore(1e-13 * region.scale, 1e-9 * region.scale));
		}

		LN_Mesh Build()
		{
			BuildInitialTriangles();
			if (m_initialFaces.empty())
			{
				throw std::runtime_error("trimmed region produced no triangles.");
			}
			Refine();
			return Emit();
		}

	private:
		// --- unfolded layout ------------------------------------------------

		void BuildInitialTriangles()
		{
			std::vector<RawSegment> raw;
			auto addSegment = [&raw](const Vec2& a, const Vec2& b)
			{
				if (!SamePoint(a, b))
				{
					raw.push_back(RawSegment{ a, b });
				}
			};

			const Vec2 bottomLeft{ m_region.minU, m_region.minV };
			const Vec2 bottomRight{ m_region.maxU, m_region.minV };
			const Vec2 topRight{ m_region.maxU, m_region.maxV };
			const Vec2 topLeft{ m_region.minU, m_region.maxV };
			addSegment(bottomLeft, bottomRight);
			addSegment(bottomRight, topRight);
			addSegment(topRight, topLeft);
			addSegment(topLeft, bottomLeft);

			const double a = m_info.a;
			const double P = m_info.P;
			const double eps = m_region.eps;

			std::set<double> uLines;
			std::set<double> vLines;
			std::vector<double> seamLines;
			auto addLine = [&](std::set<double>& lines, double value, double lower, double upper)
			{
				if (value >= lower - eps && value <= upper + eps)
				{
					lines.insert(std::min(upper, std::max(lower, value)));
				}
			};
			auto addPeriodicU = [&](double value)
			{
				const long long kMin = static_cast<long long>(std::floor((m_region.minU - value) / P)) - 1;
				const long long kMax = static_cast<long long>(std::ceil((m_region.maxU - value) / P)) + 1;
				for (long long k = kMin; k <= kMax; ++k)
				{
					addLine(uLines, value + static_cast<double>(k) * P, m_region.minU, m_region.maxU);
				}
			};

			// Periodic seams and periodically replicated interior U knots.
			addPeriodicU(a);
			seamLines.push_back(a);
			const long long seamKMin = static_cast<long long>(std::floor((m_region.minU - a) / P)) - 1;
			const long long seamKMax = static_cast<long long>(std::ceil((m_region.maxU - a) / P)) + 1;
			for (long long k = seamKMin; k <= seamKMax; ++k)
			{
				seamLines.push_back(a + static_cast<double>(k) * P);
			}
			std::vector<double> uKnots;
			for (double knot : m_surface.KnotVectorU)
			{
				if (knot > a && knot < m_info.b)
				{
					uKnots.push_back(knot);
				}
			}
			std::sort(uKnots.begin(), uKnots.end());
			uKnots.erase(std::unique(uKnots.begin(), uKnots.end()), uKnots.end());
			for (double knot : uKnots)
			{
				addPeriodicU(knot);
			}

			// Interior V knots.
			for (double knot : m_surface.KnotVectorV)
			{
				if (knot > m_info.vMin && knot < m_info.vMax)
				{
					addLine(vLines, knot, m_region.minV, m_region.maxV);
				}
			}

			// Lines through every loop vertex connect the loops to the grid.
			for (const LoopInfo& loop : m_region.loops)
			{
				for (const Vec2& point : loop.points)
				{
					addLine(uLines, point.u, m_region.minU, m_region.maxU);
					addLine(vLines, point.v, m_region.minV, m_region.maxV);
				}
			}
			addLine(uLines, m_region.minU, m_region.minU, m_region.maxU);
			addLine(uLines, m_region.maxU, m_region.minU, m_region.maxU);
			addLine(vLines, m_region.minV, m_region.minV, m_region.maxV);
			addLine(vLines, m_region.maxV, m_region.minV, m_region.maxV);

			const std::vector<double> vLineList(vLines.begin(), vLines.end());

			for (double line : uLines)
			{
				addSegment(Vec2{ line, m_region.minV }, Vec2{ line, m_region.maxV });
			}
			for (double line : vLines)
			{
				addSegment(Vec2{ m_region.minU, line }, Vec2{ m_region.maxU, line });
			}
			for (const LoopInfo& loop : m_region.loops)
			{
				for (std::size_t i = 0; i < loop.points.size(); ++i)
				{
					addSegment(loop.points[i], loop.points[(i + 1) % loop.points.size()]);
				}
			}

			Arrangement arrangement = BuildArrangement(raw, eps, m_region.scale);
			m_vertexStore.reset(new VertexStore(1e-13 * m_region.scale, 1e-9 * m_region.scale));
			m_vertexStore->SetSnapLines(seamLines, vLineList);
			std::vector<int> mapping;
			m_vertexStore->Seed(arrangement.vertices.Points(), mapping);
			m_points = m_vertexStore->Points();
			m_positions.assign(m_points.size(), XYZ());
			m_positionValid.assign(m_points.size(), 0);

			std::vector<std::array<int, 3>> triangles;
			for (const std::vector<int>& face : arrangement.faces)
			{
				const std::vector<Vec2>& points = arrangement.vertices.Points();
				std::vector<std::array<int, 3>> faceTriangles;
				TriangulateFace(face, points, faceTriangles);
				for (std::array<int, 3> triangle : faceTriangles)
				{
					triangle[0] = mapping[triangle[0]];
					triangle[1] = mapping[triangle[1]];
					triangle[2] = mapping[triangle[2]];
					if (triangle[0] == triangle[1] || triangle[1] == triangle[2] || triangle[2] == triangle[0])
					{
						continue;
					}
					triangles.push_back(triangle);
				}
			}

			m_initialFaces.clear();
			m_initialFaces.reserve(triangles.size());
			for (const std::array<int, 3>& triangle : triangles)
			{
				if (!InsideTrimmedRegion(triangle))
				{
					continue;
				}
				MeshFace face;
				face.v[0] = triangle[0];
				face.v[1] = triangle[1];
				face.v[2] = triangle[2];
				face.depth = 0;
				m_initialFaces.push_back(face);
			}
		}

		bool InsideTrimmedRegion(const std::array<int, 3>& triangle) const
		{
			const Vec2 centroid{
				(m_points[triangle[0]].u + m_points[triangle[1]].u + m_points[triangle[2]].u) / 3.0,
				(m_points[triangle[0]].v + m_points[triangle[1]].v + m_points[triangle[2]].v) / 3.0 };
			const double eps = m_region.eps;
			if (ClassifyPoint(m_region.loops[0].points, centroid, eps) != PointClass::Inside)
			{
				return false;
			}
			for (std::size_t i = 1; i < m_region.loops.size(); ++i)
			{
				if (ClassifyPoint(m_region.loops[i].points, centroid, eps) != PointClass::Outside)
				{
					return false;
				}
			}
			return true;
		}

		// --- per-corner UV mapping ------------------------------------------

		long long ChoosePeriod(const MeshFace& face) const
		{
			const double uc = (m_points[face.v[0]].u + m_points[face.v[1]].u + m_points[face.v[2]].u) / 3.0;
			const long long base = static_cast<long long>(std::floor((uc - m_info.a) / m_info.P));
			long long best = base;
			double bestViolation = std::numeric_limits<double>::infinity();
			for (long long delta = -1; delta <= 1; ++delta)
			{
				const long long candidate = base + delta;
				double violation = 0.0;
				for (int corner = 0; corner < 3; ++corner)
				{
					const Vec2& point = m_points[face.v[corner]];
					const double mapped = m_info.a + (point.u - m_info.a - static_cast<double>(candidate) * m_info.P);
					violation = std::max(violation, m_info.a - mapped);
					violation = std::max(violation, mapped - m_info.b);
				}
				if (violation < bestViolation)
				{
					bestViolation = violation;
					best = candidate;
				}
			}
			if (bestViolation > m_uvTolerance)
			{
				throw std::runtime_error("a triangle crosses a periodic seam of the surface.");
			}
			return best;
		}

		UV CornerUV(const Vec2& point, long long period) const
		{
			double u = m_info.a + (point.u - m_info.a - static_cast<double>(period) * m_info.P);
			if (u < m_info.a && m_info.a - u <= m_uvTolerance)
			{
				u = m_info.a;
			}
			if (u > m_info.b && u - m_info.b <= m_uvTolerance)
			{
				u = m_info.b;
			}
			if (u < m_info.a || u > m_info.b)
			{
				throw std::runtime_error("failed to map a corner into the effective U domain.");
			}
			return UV(u, point.v);
		}

		// --- surface evaluation ---------------------------------------------

		bool IsSeamVertex(int vertex, long long& period) const
		{
			const Vec2& point = m_points[vertex];
			const long long candidate = static_cast<long long>(std::llround((point.u - m_info.a) / m_info.P));
			const double seamU = m_info.a + static_cast<double>(candidate) * m_info.P;
			if (std::fabs(point.u - seamU) <= m_seamSnap)
			{
				period = candidate;
				return true;
			}
			return false;
		}

		UV VertexUV(int vertex) const
		{
			const Vec2& point = m_points[vertex];
			long long period = 0;
			if (IsSeamVertex(vertex, period))
			{
				return UV(m_info.a, point.v);
			}
			double u = m_info.a + PositiveModulo(point.u - m_info.a, m_info.P);
			if (u <= m_info.a + m_uvTolerance)
			{
				u = m_info.a;
			}
			if (u >= m_info.b - m_uvTolerance)
			{
				u = m_info.b;
			}
			return UV(u, point.v);
		}

		void EnsureCapacity()
		{
			while (m_points.size() < m_vertexStore->Size())
			{
				m_points.push_back(m_vertexStore->Point(static_cast<int>(m_points.size())));
			}
			if (m_positions.size() < m_points.size())
			{
				m_positions.resize(m_points.size(), XYZ());
				m_positionValid.resize(m_points.size(), 0);
			}
		}

		XYZ Position(int vertex)
		{
			EnsureCapacity();
			if (m_positionValid[vertex])
			{
				return m_positions[vertex];
			}

			const UV uv = VertexUV(vertex);
			const XYZ position = NurbsSurface::GetPointOnSurface(m_surface, uv);
			if (!IsFinite(position.X()) || !IsFinite(position.Y()) || !IsFinite(position.Z()))
			{
				throw std::runtime_error("surface evaluation returned a non-finite position.");
			}

			long long period = 0;
			if (IsSeamVertex(vertex, period))
			{
				const XYZ other = NurbsSurface::GetPointOnSurface(m_surface, UV(m_info.b, uv.GetV()));
				if (!IsFinite(other.X()) || position.Distance(other) > m_options.seamTolerance)
				{
					throw std::runtime_error("surface positions on the two sides of the seam do not match within seamTolerance.");
				}
			}

			m_positions[vertex] = position;
			m_positionValid[vertex] = 1;
			return position;
		}

		// --- chord error -----------------------------------------------------

		bool TriangleWithinTolerance(const MeshFace& face, long long period, double& maximumError)
		{
			const XYZ p0 = Position(face.v[0]);
			const XYZ p1 = Position(face.v[1]);
			const XYZ p2 = Position(face.v[2]);

			const UV uv0 = CornerUV(m_points[face.v[0]], period);
			const UV uv1 = CornerUV(m_points[face.v[1]], period);
			const UV uv2 = CornerUV(m_points[face.v[2]], period);

			struct Sample
			{
				UV uv;
				XYZ linear;
			};
			const Sample samples[4] = {
				{ UV(0.5 * (uv0.GetU() + uv1.GetU()), 0.5 * (uv0.GetV() + uv1.GetV())), 0.5 * (p0 + p1) },
				{ UV(0.5 * (uv1.GetU() + uv2.GetU()), 0.5 * (uv1.GetV() + uv2.GetV())), 0.5 * (p1 + p2) },
				{ UV(0.5 * (uv2.GetU() + uv0.GetU()), 0.5 * (uv2.GetV() + uv0.GetV())), 0.5 * (p2 + p0) },
				{ UV((uv0.GetU() + uv1.GetU() + uv2.GetU()) / 3.0,
					(uv0.GetV() + uv1.GetV() + uv2.GetV()) / 3.0),
					(p0 + p1 + p2) / 3.0 }
			};

			maximumError = 0.0;
			for (const Sample& sample : samples)
			{
				const XYZ exact = NurbsSurface::GetPointOnSurface(m_surface, sample.uv);
				const double error = exact.Distance(sample.linear);
				if (!IsFinite(error))
				{
					throw std::runtime_error("surface evaluation returned a non-finite chord error.");
				}
				maximumError = std::max(maximumError, error);
			}
			return maximumError <= m_options.chordTolerance;
		}

		// --- refinement ------------------------------------------------------

		int Midpoint(int first, int second)
		{
			const auto key = std::minmax(first, second);
			const auto found = m_midpoints.find({ key.first, key.second });
			if (found != m_midpoints.end())
			{
				return found->second;
			}
			const Vec2 midpoint = (m_points[first] + m_points[second]) * 0.5;
			const int id = m_vertexStore->Add(midpoint);
			EnsureCapacity();
			m_midpoints[{ key.first, key.second }] = id;
			return id;
		}

		void Refine()
		{
			std::vector<MeshFace> faces = m_initialFaces;
			const unsigned maxDepth = m_options.maxDepth;
			std::size_t iteration = 0;
			const std::size_t iterationLimit = 8 * static_cast<std::size_t>(maxDepth) + 32;

			while (true)
			{
				std::vector<char> needsRefinement(faces.size(), 0);
				bool anyNeedsRefinement = false;
				for (std::size_t i = 0; i < faces.size(); ++i)
				{
					const long long period = ChoosePeriod(faces[i]);
					double error = 0.0;
					if (!TriangleWithinTolerance(faces[i], period, error))
					{
						if (faces[i].depth >= maxDepth)
						{
							throw std::runtime_error("adaptive refinement exhausted maxDepth before reaching chordTolerance.");
						}
						needsRefinement[i] = 1;
						anyNeedsRefinement = true;
					}
				}
				if (!anyNeedsRefinement)
				{
					m_finalFaces = std::move(faces);
					return;
				}

				std::set<std::pair<int, int>> marked;
				auto mark = [&marked](int first, int second)
				{
					const auto key = std::minmax(first, second);
					marked.insert({ key.first, key.second });
				};
				for (std::size_t i = 0; i < faces.size(); ++i)
				{
					if (!needsRefinement[i])
					{
						continue;
					}
					mark(faces[i].v[0], faces[i].v[1]);
					mark(faces[i].v[1], faces[i].v[2]);
					mark(faces[i].v[2], faces[i].v[0]);
				}

				std::vector<MeshFace> next;
				next.reserve(faces.size() * 2);
				bool splitAny = false;
				for (const MeshFace& face : faces)
				{
					auto isMarked = [&marked](int x, int y)
					{
						return marked.count({ std::min(x, y), std::max(x, y) }) != 0;
					};
					const bool ab = isMarked(face.v[0], face.v[1]);
					const bool bc = isMarked(face.v[1], face.v[2]);
					const bool ca = isMarked(face.v[2], face.v[0]);
					const int count = (ab ? 1 : 0) + (bc ? 1 : 0) + (ca ? 1 : 0);
					if (count == 0)
					{
						next.push_back(face);
						continue;
					}
					if (face.depth + 1 > maxDepth)
					{
						throw std::runtime_error("conforming refinement exhausted maxDepth.");
					}
					splitAny = true;
					const unsigned depth = face.depth + 1;
					const int a = face.v[0];
					const int b = face.v[1];
					const int c = face.v[2];

					if (count == 3)
					{
						const int mab = Midpoint(a, b);
						const int mbc = Midpoint(b, c);
						const int mca = Midpoint(c, a);
						next.push_back(MeshFace{ { a, mab, mca }, depth });
						next.push_back(MeshFace{ { mab, b, mbc }, depth });
						next.push_back(MeshFace{ { mca, mbc, c }, depth });
						next.push_back(MeshFace{ { mab, mbc, mca }, depth });
					}
					else if (count == 2)
					{
						if (ab && bc)
						{
							const int mab = Midpoint(a, b);
							const int mbc = Midpoint(b, c);
							next.push_back(MeshFace{ { mab, b, mbc }, depth });
							next.push_back(MeshFace{ { a, mab, mbc }, depth });
							next.push_back(MeshFace{ { a, mbc, c }, depth });
						}
						else if (ab && ca)
						{
							const int mab = Midpoint(a, b);
							const int mca = Midpoint(c, a);
							next.push_back(MeshFace{ { a, mab, mca }, depth });
							next.push_back(MeshFace{ { mab, b, c }, depth });
							next.push_back(MeshFace{ { mab, c, mca }, depth });
						}
						else
						{
							const int mbc = Midpoint(b, c);
							const int mca = Midpoint(c, a);
							next.push_back(MeshFace{ { a, b, mbc }, depth });
							next.push_back(MeshFace{ { a, mbc, mca }, depth });
							next.push_back(MeshFace{ { mca, mbc, c }, depth });
						}
					}
					else if (ab)
					{
						const int mab = Midpoint(a, b);
						next.push_back(MeshFace{ { a, mab, c }, depth });
						next.push_back(MeshFace{ { mab, b, c }, depth });
					}
					else if (bc)
					{
						const int mbc = Midpoint(b, c);
						next.push_back(MeshFace{ { a, b, mbc }, depth });
						next.push_back(MeshFace{ { a, mbc, c }, depth });
					}
					else
					{
						const int mca = Midpoint(c, a);
						next.push_back(MeshFace{ { a, b, mca }, depth });
						next.push_back(MeshFace{ { mca, b, c }, depth });
					}
				}

				if (!splitAny || next.size() <= faces.size())
				{
					throw std::runtime_error("adaptive refinement made no progress.");
				}
				faces.swap(next);
				if (++iteration > iterationLimit)
				{
					throw std::runtime_error("adaptive refinement did not terminate.");
				}
			}
		}

		// --- output ----------------------------------------------------------

		void CheckSeamEdgeMidpoints(const std::vector<MeshFace>& faces)
		{
			std::set<std::pair<int, int>> edges;
			for (const MeshFace& face : faces)
			{
				for (int corner = 0; corner < 3; ++corner)
				{
					const int first = face.v[corner];
					const int second = face.v[(corner + 1) % 3];
					const auto key = std::minmax(first, second);
					edges.insert({ key.first, key.second });
				}
			}

			for (const auto& edge : edges)
			{
				long long periodA = 0;
				long long periodB = 0;
				if (!IsSeamVertex(edge.first, periodA) || !IsSeamVertex(edge.second, periodB))
				{
					continue;
				}
				if (periodA != periodB)
				{
					continue;
				}
				const double midpointV = 0.5 * (m_points[edge.first].v + m_points[edge.second].v);
				const XYZ sideA = NurbsSurface::GetPointOnSurface(m_surface, UV(m_info.a, midpointV));
				const XYZ sideB = NurbsSurface::GetPointOnSurface(m_surface, UV(m_info.b, midpointV));
				if (!IsFinite(sideA.X()) || !IsFinite(sideB.X()) || sideA.Distance(sideB) > m_options.seamTolerance)
				{
					throw std::runtime_error("surface positions at a seam edge midpoint do not match within seamTolerance.");
				}
			}
		}

		LN_Mesh Emit()
		{
			CheckSeamEdgeMidpoints(m_finalFaces);

			std::set<int> used;
			for (const MeshFace& face : m_finalFaces)
			{
				used.insert(face.v[0]);
				used.insert(face.v[1]);
				used.insert(face.v[2]);
			}

			std::map<int, int> remap;
			LN_Mesh mesh;
			for (int vertex : used)
			{
				const int index = static_cast<int>(mesh.Vertices.size());
				remap[vertex] = index;
				mesh.Vertices.push_back(Position(vertex));
			}

			for (const MeshFace& face : m_finalFaces)
			{
				const long long period = ChoosePeriod(face);
				const UV uvs[3] = {
					CornerUV(m_points[face.v[0]], period),
					CornerUV(m_points[face.v[1]], period),
					CornerUV(m_points[face.v[2]], period)
				};

				const double orientation = (uvs[1].GetU() - uvs[0].GetU()) * (uvs[2].GetV() - uvs[0].GetV())
					- (uvs[1].GetV() - uvs[0].GetV()) * (uvs[2].GetU() - uvs[0].GetU());
				if (!(orientation > 0.0))
				{
					throw std::runtime_error("output triangle is degenerate in its local UV coordinates.");
				}

				const int base = static_cast<int>(mesh.UVs.size());
				for (int corner = 0; corner < 3; ++corner)
				{
					mesh.UVs.push_back(uvs[corner]);
					mesh.UVIndices.push_back(base + corner);
				}
				mesh.Faces.push_back({ remap[face.v[0]], remap[face.v[1]], remap[face.v[2]] });
			}
			return mesh;
		}

		// --- member data -----------------------------------------------------

		const LN_NurbsSurface& m_surface;
		SurfaceInfo m_info;
		Region m_region;
		const TrimMeshOptions& m_options;

		std::unique_ptr<VertexStore> m_vertexStore;
		std::vector<Vec2> m_points;
		std::vector<XYZ> m_positions;
		std::vector<char> m_positionValid;
		std::vector<MeshFace> m_initialFaces;
		std::vector<MeshFace> m_finalFaces;
		std::map<std::pair<int, int>, int> m_midpoints;

		double m_uvTolerance = 1e-9;
		double m_seamSnap = 1e-10;
	};
} // namespace

LN_Mesh TrimmedSurfaceMesh::Build(const LN_NurbsSurface& surface,
	const std::vector<std::vector<UV>>& loops,
	const TrimMeshOptions& options)
{
	ValidateOptions(options);
	const SurfaceInfo info = ValidateSurface(surface, options.seamTolerance);

	Region region;
	ValidateLoops(loops, info, region);

	Builder builder(surface, info, region, options);
	return builder.Build();
}
} // namespace LNLib
