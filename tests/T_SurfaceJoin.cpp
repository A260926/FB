/*
 * Contract tests for LNLib::NurbsSurface::JoinAlongU.
 *
 * The reference geometry never comes from the new entry itself: expected
 * points and derivatives are taken from the original input evaluators, with
 * derivatives converted by the chain rule of the parameter normalization.
 *
 * The offline harness (support/CMakeLists.txt) defines LNLIB_JOIN_CONTRACTS
 * and provides the support/ include path. The main library test target globs
 * this file too but lacks that harness, so the whole translation unit is
 * compiled out there.
 */

#if defined(LNLIB_JOIN_CONTRACTS)

#include "support/checks.hpp"
#include "NurbsSurface.h"
#include "NurbsCurve.h"
#include "MathUtils.h"
#include "XYZW.h"
#include "UV.h"

#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace LNLib;

namespace
{
	constexpr double kTol = 1e-10;

	XYZW HomogeneousProduct(const XYZW& a, const XYZW& b)
	{
		return XYZW(a.GetWX() * b.GetWX(), a.GetWY() * b.GetWY(),
			a.GetWZ() * b.GetWZ(), a.GetW() * b.GetW());
	}

	// Bernstein product of two homogeneous Bezier control polygons.
	std::vector<XYZW> BezierMultiply(const std::vector<XYZW>& a, const std::vector<XYZW>& b)
	{
		int degreeA = static_cast<int>(a.size()) - 1;
		int degreeB = static_cast<int>(b.size()) - 1;
		int degreeResult = degreeA + degreeB;
		std::vector<XYZW> result(degreeResult + 1, XYZW(0.0, 0.0, 0.0, 0.0));
		for (int k = 0; k <= degreeResult; ++k)
		{
			int jLow = std::max(0, k - degreeB);
			int jHigh = std::min(degreeA, k);
			for (int j = jLow; j <= jHigh; ++j)
			{
				double alpha = MathUtils::Binomial(degreeA, j) *
					MathUtils::Binomial(degreeB, k - j) /
					MathUtils::Binomial(degreeResult, k);
				result[k] += alpha * HomogeneousProduct(a[j], b[k - j]);
			}
		}
		return result;
	}

	// Refine every interior knot to full multiplicity, then read off the
	// consecutive degree+1 control blocks as Bezier segments. This avoids the
	// library decomposition routine's full-multiplicity edge case.
	std::vector<std::vector<XYZW>> ExtractBezierSegments(const LN_NurbsCurve& curve)
	{
		int degree = curve.Degree;
		const std::vector<double>& knots = curve.KnotVector;
		std::vector<double> inserts;
		int distinctInterior = 0;
		for (size_t i = static_cast<size_t>(degree + 1); i < knots.size() - (degree + 1);)
		{
			++distinctInterior;
			double knot = knots[i];
			size_t j = i + 1;
			while (j < knots.size() - (degree + 1) && knots[j] == knot) ++j;
			int multiplicity = static_cast<int>(j - i);
			for (int k = 0; k < degree - multiplicity; ++k) inserts.push_back(knot);
			i = j;
		}
		LN_NurbsCurve refined = curve;
		if (!inserts.empty())
		{
			LN_NurbsCurve temporary;
			NurbsCurve::RefineKnotVector(curve, inserts, temporary);
			refined = temporary;
		}
		int segmentCount = distinctInterior + 1;
		std::vector<std::vector<XYZW>> segments(segmentCount);
		for (int s = 0; s < segmentCount; ++s)
		{
			for (int k = 0; k <= degree; ++k)
			{
				segments[s].push_back(refined.ControlPoints[s * degree + k]);
			}
		}
		return segments;
	}

	// Return the curve scalar(v) * curve(v), with scalar a positive linear
	// function c0 + c1*v, as a piecewise-Bezier clamped NURBS curve. Optional
	// extra partition knots are inserted first. The Euclidean geometry of the
	// original curve is preserved exactly because the multiplier is a common
	// scalar on every homogeneous component.
	LN_NurbsCurve ScaleSeamCurve(const LN_NurbsCurve& seam, double c0, double c1,
		const std::vector<double>& extraBreaks = {})
	{
		LN_NurbsCurve refined = seam;
		if (!extraBreaks.empty())
		{
			LN_NurbsCurve inserted;
			NurbsCurve::RefineKnotVector(seam, extraBreaks, inserted);
			refined = inserted;
		}
		std::vector<std::vector<XYZW>> beziers = ExtractBezierSegments(refined);
		int baseDegree = refined.Degree;
		int resultDegree = baseDegree + 1;

		std::vector<XYZW> merged;
		std::vector<double> breakValues;
		for (int s = 0; s < static_cast<int>(beziers.size()); ++s)
		{
			// Recover the distinct partition values from the refined knot vector.
			std::vector<double> distinct;
			for (double knot : refined.KnotVector)
			{
				if (distinct.empty() || distinct.back() != knot)
				{
					distinct.push_back(knot);
				}
			}
			double segmentStart = distinct[s];
			double segmentEnd = distinct[s + 1];

			XYZW scalarStart(c0 + c1 * segmentStart, c0 + c1 * segmentStart,
				c0 + c1 * segmentStart, c0 + c1 * segmentStart);
			XYZW scalarEnd(c0 + c1 * segmentEnd, c0 + c1 * segmentEnd,
				c0 + c1 * segmentEnd, c0 + c1 * segmentEnd);
			std::vector<XYZW> scalarBezier = { scalarStart, scalarEnd };

			std::vector<XYZW> product = BezierMultiply(beziers[s], scalarBezier);
			if (s == 0)
			{
				merged = product;
			}
			else
			{
				breakValues.push_back(segmentStart);
				for (int k = 1; k <= resultDegree; ++k)
				{
					merged.push_back(product[k]);
				}
			}
		}

		LN_NurbsCurve result;
		result.Degree = resultDegree;
		result.ControlPoints = merged;
		for (int i = 0; i <= resultDegree; ++i) result.KnotVector.push_back(0.0);
		for (double breakValue : breakValues)
		{
			for (int i = 0; i < resultDegree; ++i) result.KnotVector.push_back(breakValue);
		}
		for (int i = 0; i <= resultDegree; ++i) result.KnotVector.push_back(1.0);
		return result;
	}

	LN_NurbsSurface BuildSurface(int degreeU, int degreeV,
		const std::vector<double>& knotsU, const std::vector<double>& knotsV,
		const std::function<XYZW(int, int)>& generator)
	{
		LN_NurbsSurface surface;
		surface.DegreeU = degreeU;
		surface.DegreeV = degreeV;
		surface.KnotVectorU = knotsU;
		surface.KnotVectorV = knotsV;
		int rows = static_cast<int>(knotsU.size()) - degreeU - 1;
		int columns = static_cast<int>(knotsV.size()) - degreeV - 1;
		surface.ControlPoints.assign(rows, std::vector<XYZW>(columns));
		for (int i = 0; i < rows; ++i)
		{
			for (int j = 0; j < columns; ++j)
			{
				surface.ControlPoints[i][j] = generator(i, j);
			}
		}
		return surface;
	}

	double Weight(int i, int j, int seed)
	{
		return 0.5 + 0.05 * ((i * 5 + j * 3 + seed * 7) % 19);
	}

	// A non-planar rational surface with deliberately non-constant weights.
	LN_NurbsSurface GenericLeft(int seed = 1)
	{
		// U degree 2, single Bezier span. V degree 3 with a repeated (mult 2)
		// interior knot and a simple one.
		return BuildSurface(2, 3,
			{ 0.0, 0.0, 0.0, 1.0, 1.0, 1.0 },
			{ 0.0, 0.0, 0.0, 0.0, 0.4, 0.4, 0.7, 1.0, 1.0, 1.0, 1.0 },
			[seed](int i, int j)
			{
				double w = Weight(i, j, seed);
				XYZ point(0.45 * i, 0.18 * j - 0.3,
					0.25 * std::sin(0.7 * i + 0.5 * j) + 0.08 * i * j);
				return XYZW(point, w);
			});
	}

	// Build a right surface whose min-U seam is scalar(v) times the left seam
	// (optionally after reversing the left seam in V). The two Euclidean seam
	// curves are identical but the homogeneous weight ratio is non-constant.
	LN_NurbsSurface MatchingRight(const LN_NurbsSurface& left, bool reverseSeamV,
		double c0, double c1, const std::vector<double>& extraBreaks)
	{
		LN_NurbsCurve leftSeam;
		leftSeam.Degree = left.DegreeV;
		leftSeam.KnotVector = left.KnotVectorV;
		for (const XYZW& point : left.ControlPoints.back())
		{
			leftSeam.ControlPoints.push_back(point);
		}
		if (reverseSeamV)
		{
			LN_NurbsCurve reversed;
			NurbsCurve::Reverse(leftSeam, reversed);
			leftSeam = reversed;
		}
		LN_NurbsCurve rightSeam = ScaleSeamCurve(leftSeam, c0, c1, extraBreaks);

		// Different U structure: degree 3 with an interior knot and 4 spans.
		int degreeU = 3;
		std::vector<double> knotsU = { 0.0, 0.0, 0.0, 0.0, 0.35, 1.0, 1.0, 1.0, 1.0 };
		int rows = static_cast<int>(knotsU.size()) - degreeU - 1;
		int columns = static_cast<int>(rightSeam.ControlPoints.size());

		LN_NurbsSurface right;
		right.DegreeU = degreeU;
		right.DegreeV = rightSeam.Degree;
		right.KnotVectorU = knotsU;
		right.KnotVectorV = rightSeam.KnotVector;
		right.ControlPoints.assign(rows, std::vector<XYZW>(columns));
		for (int j = 0; j < columns; ++j)
		{
			right.ControlPoints[0][j] = rightSeam.ControlPoints[j];
		}
		for (int i = 1; i < rows; ++i)
		{
			for (int j = 0; j < columns; ++j)
			{
				double w = 0.6 + 0.04 * ((i * 3 + j * 7) % 17);
				XYZ point(1.0 + 0.4 * i, 0.16 * j - 0.3,
					0.2 * std::cos(0.6 * i + 0.4 * j) - 0.07 * i * j);
				right.ControlPoints[i][j] = XYZW(point, w);
			}
		}
		return right;
	}

	struct Domain
	{
		double MinU, MaxU, MinV, MaxV;
	};

	void AffineRescale(LN_NurbsSurface& surface, const Domain& domain)
	{
		for (double& knot : surface.KnotVectorU)
		{
			knot = domain.MinU + knot * (domain.MaxU - domain.MinU);
		}
		for (double& knot : surface.KnotVectorV)
		{
			knot = domain.MinV + knot * (domain.MaxV - domain.MinV);
		}
	}

	// Reference point from the original (possibly non-unit, possibly V-reversed)
	// inputs at joined parameters (uJoined, vJoined), each in [0,1] locally.
	XYZ ReferencePoint(const LN_NurbsSurface& left, const LN_NurbsSurface& right,
		const Domain& leftDomain, const Domain& rightDomain,
		bool reverseRightV, double uJoined, double vJoined)
	{
		if (uJoined <= 1.0)
		{
			double u = leftDomain.MinU + uJoined * (leftDomain.MaxU - leftDomain.MinU);
			double v = leftDomain.MinV + vJoined * (leftDomain.MaxV - leftDomain.MinV);
			return NurbsSurface::GetPointOnSurface(left, UV(u, v));
		}
		double localU = uJoined - 1.0;
		double u = rightDomain.MinU + localU * (rightDomain.MaxU - rightDomain.MinU);
		double normalizedV = reverseRightV ? 1.0 - vJoined : vJoined;
		double v = rightDomain.MinV + normalizedV * (rightDomain.MaxV - rightDomain.MinV);
		return NurbsSurface::GetPointOnSurface(right, UV(u, v));
	}

	// Reference first derivatives with respect to the joined parameter frame,
	// obtained from the original evaluators and the normalization chain rule.
	void ReferenceDerivatives(const LN_NurbsSurface& left, const LN_NurbsSurface& right,
		const Domain& leftDomain, const Domain& rightDomain,
		bool reverseRightV, double uJoined, double vJoined, XYZ& su, XYZ& sv)
	{
		if (uJoined <= 1.0)
		{
			double u = leftDomain.MinU + uJoined * (leftDomain.MaxU - leftDomain.MinU);
			double v = leftDomain.MinV + vJoined * (leftDomain.MaxV - leftDomain.MinV);
			auto derivatives = NurbsSurface::ComputeRationalSurfaceDerivatives(left, 1, UV(u, v));
			su = derivatives[1][0] * (leftDomain.MaxU - leftDomain.MinU);
			sv = derivatives[0][1] * (leftDomain.MaxV - leftDomain.MinV);
			return;
		}
		double localU = uJoined - 1.0;
		double u = rightDomain.MinU + localU * (rightDomain.MaxU - rightDomain.MinU);
		double normalizedV = reverseRightV ? 1.0 - vJoined : vJoined;
		double v = rightDomain.MinV + normalizedV * (rightDomain.MaxV - rightDomain.MinV);
		auto derivatives = NurbsSurface::ComputeRationalSurfaceDerivatives(right, 1, UV(u, v));
		su = derivatives[1][0] * (rightDomain.MaxU - rightDomain.MinU);
		double vSign = reverseRightV ? -1.0 : 1.0;
		sv = derivatives[0][1] * (vSign * (rightDomain.MaxV - rightDomain.MinV));
	}

	double MaxPointError(const LN_NurbsSurface& joined, const LN_NurbsSurface& left,
		const LN_NurbsSurface& right, const Domain& leftDomain, const Domain& rightDomain,
		bool reverseRightV)
	{
		double error = 0.0;
		for (int i = 0; i <= 20; ++i)
		{
			for (int j = 0; j <= 20; ++j)
			{
				double uJoined = 2.0 * i / 20.0;
				double vJoined = static_cast<double>(j) / 20.0;
				XYZ expected = ReferencePoint(left, right, leftDomain, rightDomain,
					reverseRightV, uJoined, vJoined);
				XYZ actual = NurbsSurface::GetPointOnSurface(joined, UV(uJoined, vJoined));
				error = std::max(error, expected.Distance(actual));
			}
		}
		return error;
	}

	int KnotMultiplicity(const std::vector<double>& knots, double value)
	{
		int count = 0;
		for (double knot : knots)
		{
			if (knot == value) ++count;
		}
		return count;
	}

	void RunBasisGroup()
	{
		// Mixed degrees on both axes, repeated and distinct internal knots,
		// non-unit parameter domains, right-side V reversal.
		LN_NurbsSurface left = GenericLeft();
		LN_NurbsSurface right = MatchingRight(left, false, 0.8, 0.6, { 0.55 });
		// Different non-unit U domains; a shared non-unit V domain keeps the
		// common partition breaks bit-identical across both inputs.
		Domain leftDomain{ 2.0, 7.0, -3.0, 4.0 };
		Domain rightDomain{ 11.0, 13.0, -3.0, 4.0 };
		AffineRescale(left, leftDomain);
		AffineRescale(right, rightDomain);

		LN_NurbsSurface joined;
		bool ok = NurbsSurface::JoinAlongU(left, right, false, kTol, joined);
		join_checks::require(ok, "basis: mixed-degree join must succeed");

		join_checks::require(joined.DegreeU == std::max(left.DegreeU, right.DegreeU),
			"basis: U degree is the maximum of both sides");
		join_checks::require(joined.DegreeV == 3 + 4,
			"basis: V degree is the sum of both original V degrees");
		join_checks::require(KnotMultiplicity(joined.KnotVectorU, 1.0) == joined.DegreeU,
			"basis: seam knot multiplicity equals U degree");
		join_checks::require(KnotMultiplicity(joined.KnotVectorV, 0.4) == joined.DegreeV &&
			KnotMultiplicity(joined.KnotVectorV, 0.55) == joined.DegreeV &&
			KnotMultiplicity(joined.KnotVectorV, 0.7) == joined.DegreeV,
			"basis: every common internal V knot has multiplicity q");

		int expectedRows = static_cast<int>(joined.KnotVectorU.size()) - joined.DegreeU - 1;
		int expectedColumns = static_cast<int>(joined.KnotVectorV.size()) - joined.DegreeV - 1;
		join_checks::require(static_cast<int>(joined.ControlPoints.size()) == expectedRows &&
			static_cast<int>(joined.ControlPoints[0].size()) == expectedColumns,
			"basis: rectangular control net matches both knot vectors");
		join_checks::require(joined.KnotVectorU.front() == 0.0 && joined.KnotVectorU.back() == 2.0 &&
			joined.KnotVectorV.front() == 0.0 && joined.KnotVectorV.back() == 1.0,
			"basis: result parameter domain is [0,2]x[0,1]");

		double pointError = MaxPointError(joined, left, right, leftDomain, rightDomain, false);
		join_checks::require(pointError < 1e-8, "basis: in-plane points agree on both patches");

		// Reversed right V, unit domains this time.
		LN_NurbsSurface leftReverse = GenericLeft(2);
		LN_NurbsSurface rightReverse = MatchingRight(leftReverse, true, 1.1, 0.5, { 0.25 });
		LN_NurbsSurface joinedReverse;
		bool reverseOk = NurbsSurface::JoinAlongU(leftReverse, rightReverse, true, kTol, joinedReverse);
		join_checks::require(reverseOk, "basis: reversed-V join must succeed");
		Domain unit{ 0.0, 1.0, 0.0, 1.0 };
		double reverseError = MaxPointError(joinedReverse, leftReverse, rightReverse,
			unit, unit, true);
		join_checks::require(reverseError < 1e-8, "basis: reversed-V in-plane points agree");

		// The shared seam is one physical control row, not two duplicated rows.
		// Account for the left patch's U degree elevation (single Bezier span).
		int reversedDegreeU = std::max(leftReverse.DegreeU, rightReverse.DegreeU);
		int reversedElevatedLeftRows = static_cast<int>(leftReverse.ControlPoints.size()) +
			(reversedDegreeU - leftReverse.DegreeU);
		join_checks::require(static_cast<int>(joinedReverse.ControlPoints.size()) ==
			reversedElevatedLeftRows +
			static_cast<int>(rightReverse.ControlPoints.size()) - 1,
			"basis: control net shares exactly one seam row");

		// Close but genuinely distinct V knots must be kept apart, not snapped.
		LN_NurbsSurface distinctLeft;
		distinctLeft.DegreeU = 1;
		distinctLeft.DegreeV = 2;
		distinctLeft.KnotVectorU = { 0.0, 0.0, 1.0, 1.0 };
		distinctLeft.KnotVectorV = { 0.0, 0.0, 0.0, 0.4, 1.0, 1.0, 1.0 };
		for (int i = 0; i < 2; ++i)
		{
			std::vector<XYZW> row;
			for (int j = 0; j < 4; ++j)
			{
				row.emplace_back(XYZW(XYZ(i, 0.2 * j, 0.05 * i * j), 1.0 + 0.1 * j));
			}
			distinctLeft.ControlPoints.push_back(row);
		}
		LN_NurbsSurface distinctRefined;
		NurbsSurface::RefineKnotVector(distinctLeft, { 0.40002 }, false, distinctRefined);
		LN_NurbsSurface distinctRight;
		distinctRight.DegreeU = 2;
		distinctRight.DegreeV = distinctRefined.DegreeV;
		distinctRight.KnotVectorV = distinctRefined.KnotVectorV;
		distinctRight.KnotVectorU = { 0.0, 0.0, 0.0, 1.0, 1.0, 1.0 };
		int distinctColumns = static_cast<int>(distinctRefined.ControlPoints[0].size());
		for (int i = 0; i < 3; ++i)
		{
			std::vector<XYZW> row;
			for (int j = 0; j < distinctColumns; ++j)
			{
				if (i == 0)
				{
					row.push_back(distinctRefined.ControlPoints.back()[j]);
				}
				else
				{
					row.emplace_back(XYZW(XYZ(1.0 + 0.5 * i, 0.2 * j, 0.05 * j - 0.1 * i),
						distinctRefined.ControlPoints.back()[j].GetW()));
				}
			}
			distinctRight.ControlPoints.push_back(row);
		}
		LN_NurbsSurface distinctJoined;
		bool distinctOk = NurbsSurface::JoinAlongU(distinctLeft, distinctRight,
			false, kTol, distinctJoined);
		join_checks::require(distinctOk, "basis: join with distinct close knots succeeds");
		join_checks::require(KnotMultiplicity(distinctJoined.KnotVectorV, 0.4) == distinctJoined.DegreeV &&
			KnotMultiplicity(distinctJoined.KnotVectorV, 0.40002) == distinctJoined.DegreeV,
			"basis: close but distinct V knots are not merged together");

		// U axis: different and repeated internal knots on both sides. Degree
		// elevation raises every U knot multiplicity by the elevation amount,
		// the seam keeps multiplicity p, and right-side knots shift to [1,2].
		auto makeUPatch = [](int degreeU, const std::vector<double>& knotsU, int seed)
		{
			LN_NurbsSurface surface;
			surface.DegreeU = degreeU;
			surface.DegreeV = 2;
			surface.KnotVectorU = knotsU;
			surface.KnotVectorV = { 0.0, 0.0, 0.0, 1.0, 1.0, 1.0 };
			int rows = static_cast<int>(knotsU.size()) - degreeU - 1;
			for (int i = 0; i < rows; ++i)
			{
				std::vector<XYZW> row;
				for (int j = 0; j < 3; ++j)
				{
					double w = 0.8 + 0.05 * j;
					XYZ point(0.3 * i, 0.4 * j, 0.2 * std::sin(i + j + seed));
					row.emplace_back(XYZW(point, w));
				}
				surface.ControlPoints.push_back(row);
			}
			return surface;
		};
		LN_NurbsSurface uLeft = makeUPatch(2, { 0.0, 0.0, 0.0, 0.5, 0.5, 1.0, 1.0, 1.0 }, 1);
		LN_NurbsSurface uRight = makeUPatch(3,
			{ 0.0, 0.0, 0.0, 0.0, 0.3, 0.3, 0.7, 1.0, 1.0, 1.0, 1.0 }, 2);
		for (int j = 0; j < 3; ++j)
		{
			uRight.ControlPoints[0][j] = uLeft.ControlPoints.back()[j];
		}
		LN_NurbsSurface uJoined;
		bool uOk = NurbsSurface::JoinAlongU(uLeft, uRight, false, kTol, uJoined);
		join_checks::require(uOk && uJoined.DegreeU == 3,
			"basis: mixed U-degree join with internal knots succeeds");
		join_checks::require(KnotMultiplicity(uJoined.KnotVectorU, 0.5) == 3 &&
			KnotMultiplicity(uJoined.KnotVectorU, 1.3) == 2 &&
			KnotMultiplicity(uJoined.KnotVectorU, 1.7) == 1 &&
			KnotMultiplicity(uJoined.KnotVectorU, 1.0) == 3,
			"basis: U internal and seam knot multiplicities are correct");
	}

	void RunRationalGroup()
	{
		LN_NurbsSurface left = GenericLeft(3);
		// Non-constant seam gauge: c(v) = 0.9 + 0.7v.
		LN_NurbsSurface right = MatchingRight(left, false, 0.9, 0.7, { 0.5 });

		// The homogeneous seam weight ratio is genuinely non-constant.
		double ratioStart = right.ControlPoints[0][0].GetW() / left.ControlPoints.back()[0].GetW();
		double ratioEnd = right.ControlPoints[0].back().GetW() / left.ControlPoints.back().back().GetW();
		join_checks::require(std::abs(ratioEnd - ratioStart) > 1e-6,
			"rational: seam weight ratio is not constant");

		LN_NurbsSurface joined;
		bool ok = NurbsSurface::JoinAlongU(left, right, false, kTol, joined);
		join_checks::require(ok, "rational: join with non-constant seam weights succeeds");

		// The whole seam must agree, not just its ends.
		double seamError = 0.0;
		for (int k = 0; k <= 40; ++k)
		{
			double v = static_cast<double>(k) / 40.0;
			XYZ leftPoint = NurbsSurface::GetPointOnSurface(left, UV(1.0, v));
			XYZ rightPoint = NurbsSurface::GetPointOnSurface(right, UV(0.0, v));
			XYZ joinedPoint = NurbsSurface::GetPointOnSurface(joined, UV(1.0, v));
			seamError = std::max(seamError, leftPoint.Distance(rightPoint));
			seamError = std::max(seamError, joinedPoint.Distance(leftPoint));
		}
		join_checks::require(seamError < 1e-8, "rational: the entire seam curve agrees");

		Domain unit{ 0.0, 1.0, 0.0, 1.0 };
		double pointError = MaxPointError(joined, left, right, unit, unit, false);
		join_checks::require(pointError < 1e-8, "rational: in-plane points are preserved");

		// All output weights stay positive and finite.
		bool weightsValid = true;
		for (const auto& row : joined.ControlPoints)
		{
			for (const XYZW& point : row)
			{
				weightsValid &= std::isfinite(point.GetW()) && point.GetW() > 0.0;
			}
		}
		join_checks::require(weightsValid, "rational: output weights are positive and finite");

		// Multiplying every homogeneous control point of one input by a positive
		// constant must not change the resulting Euclidean geometry.
		LN_NurbsSurface scaledLeft = left;
		for (auto& row : scaledLeft.ControlPoints)
		{
			for (XYZW& point : row) point = 3.25 * point;
		}
		LN_NurbsSurface joinedScaled;
		bool scaledOk = NurbsSurface::JoinAlongU(scaledLeft, right, false, kTol, joinedScaled);
		join_checks::require(scaledOk, "rational: globally rescaled input still joins");
		double scaleError = 0.0;
		for (int i = 0; i <= 20; ++i)
		{
			for (int j = 0; j <= 20; ++j)
			{
				UV uv(2.0 * i / 20.0, static_cast<double>(j) / 20.0);
				scaleError = std::max(scaleError,
					NurbsSurface::GetPointOnSurface(joined, uv).Distance(
						NurbsSurface::GetPointOnSurface(joinedScaled, uv)));
			}
		}
		join_checks::require(scaleError < 1e-8, "rational: global weight scaling is geometry invariant");

		// Analytic (original-evaluator plus chain rule) first derivatives away
		// from knots on both patches.
		Domain leftDomain{ -1.0, 2.5, 0.5, 3.5 };
		Domain rightDomain{ 4.0, 9.0, -2.0, 6.0 };
		LN_NurbsSurface leftDomainSurface = GenericLeft(4);
		LN_NurbsSurface rightDomainSurface = MatchingRight(leftDomainSurface, false, 1.2, 0.4, { 0.6 });
		AffineRescale(leftDomainSurface, leftDomain);
		AffineRescale(rightDomainSurface, rightDomain);
		LN_NurbsSurface domainJoined;
		bool domainOk = NurbsSurface::JoinAlongU(leftDomainSurface, rightDomainSurface,
			false, kTol, domainJoined);
		join_checks::require(domainOk, "rational: non-unit domain rational join succeeds");

		const double probeU[] = { 0.23, 0.47, 0.81, 1.23, 1.57, 1.86 };
		const double probeV[] = { 0.19, 0.52, 0.83 };
		double derivativeError = 0.0;
		for (double u : probeU)
		{
			for (double v : probeV)
			{
				XYZ expectedSu, expectedSv, point, actualSu, actualSv;
				ReferenceDerivatives(leftDomainSurface, rightDomainSurface,
					leftDomain, rightDomain, false, u, v, expectedSu, expectedSv);
				NurbsSurface::ComputeRationalSurfaceFirstOrderDerivative(
					domainJoined, UV(u, v), point, actualSu, actualSv);
				derivativeError = std::max(derivativeError, expectedSu.Distance(actualSu));
				derivativeError = std::max(derivativeError, expectedSv.Distance(actualSv));
			}
		}
		join_checks::require(derivativeError < 1e-7, "rational: first derivatives match away from knots");
	}

	void RunOwnershipGroup()
	{
		LN_NurbsSurface left = GenericLeft(5);
		LN_NurbsSurface right = MatchingRight(left, false, 1.0, 0.8, { 0.45 });

		// After U degree elevation both patches share degreeU; the left patch
		// grows one U control row (degree 2 -> 3, single Bezier span: 3 -> 4).
		int joinedDegreeU = std::max(left.DegreeU, right.DegreeU);
		int elevatedLeftRows = static_cast<int>(left.ControlPoints.size()) +
			(joinedDegreeU - left.DegreeU);
		int expectedRows = elevatedLeftRows +
			static_cast<int>(right.ControlPoints.size()) - 1;

		// Aliasing: result and left are the same object.
		LN_NurbsSurface aliasedLeft = left;
		LN_NurbsSurface rightForLeftAlias = right;
		bool aliasLeftOk = NurbsSurface::JoinAlongU(aliasedLeft, rightForLeftAlias,
			false, kTol, aliasedLeft);
		join_checks::require(aliasLeftOk &&
			static_cast<int>(aliasedLeft.ControlPoints.size()) == expectedRows,
			"ownership: result may alias the left input");

		// Aliasing: result and right are the same object.
		LN_NurbsSurface leftForRightAlias = left;
		LN_NurbsSurface aliasedRight = right;
		bool aliasRightOk = NurbsSurface::JoinAlongU(leftForRightAlias, aliasedRight,
			false, kTol, aliasedRight);
		join_checks::require(aliasRightOk &&
			static_cast<int>(aliasedRight.ControlPoints.size()) == expectedRows,
			"ownership: result may alias the right input");

		// The untouched input keeps its original content.
		join_checks::require(rightForLeftAlias.ControlPoints.size() == right.ControlPoints.size() &&
			rightForLeftAlias.ControlPoints[0][0].Distance(right.ControlPoints[0][0]) == 0.0,
			"ownership: the other input is not modified");

		// Independent storage: mutating the result does not touch the inputs.
		LN_NurbsSurface independentLeft = left;
		LN_NurbsSurface independentRight = right;
		LN_NurbsSurface joined;
		NurbsSurface::JoinAlongU(independentLeft, independentRight, false, kTol, joined);
		XYZW originalLeftPoint = independentLeft.ControlPoints[0][0];
		joined.ControlPoints[0][0] = XYZW(42.0, 42.0, 42.0, 2.0);
		join_checks::require(independentLeft.ControlPoints[0][0].Distance(originalLeftPoint) == 0.0,
			"ownership: joined control net has independent storage");

		// Legal but geometrically incompatible: seam ends match, the middle does
		// not. This must return false and leave result untouched.
		LN_NurbsSurface separated = right;
		int middle = static_cast<int>(separated.ControlPoints[0].size()) / 2;
		separated.ControlPoints[0][middle] = XYZW(
			separated.ControlPoints[0][middle].ToXYZ(true) + XYZ(0.0, 0.0, 0.08),
			separated.ControlPoints[0][middle].GetW());
		LN_NurbsSurface preserved = joined;
		bool rejected = NurbsSurface::JoinAlongU(left, separated, false, kTol, preserved);
		join_checks::require(!rejected, "ownership: a seam separated in the middle is rejected");
		join_checks::require(preserved.ControlPoints.size() == joined.ControlPoints.size() &&
			preserved.KnotVectorU.size() == joined.KnotVectorU.size(),
			"ownership: rejected join leaves result unchanged");

		// Bad inputs throw std::invalid_argument and leave result unchanged.
		auto expectThrow = [&](LN_NurbsSurface bad, const LN_NurbsSurface& good,
			double tolerance, const char* name)
		{
			LN_NurbsSurface target = joined;
			bool threw = false;
			try
			{
				NurbsSurface::JoinAlongU(bad, good, false, tolerance, target);
			}
			catch (const std::invalid_argument&)
			{
				threw = true;
			}
			join_checks::require(threw, name);
			join_checks::require(target.ControlPoints.size() == joined.ControlPoints.size(),
				"ownership: result preserved when an exception is thrown");
		};

		LN_NurbsSurface badDegree = left;
		badDegree.DegreeU = 6;
		expectThrow(badDegree, right, kTol, "ownership: degree above 5 throws");

		LN_NurbsSurface badClamp = left;
		badClamp.KnotVectorU[0] = 0.05;
		expectThrow(badClamp, right, kTol, "ownership: unclamped knot vector throws");

		LN_NurbsSurface badWeight = left;
		badWeight.ControlPoints[0][0] = XYZW(1.0, 1.0, 1.0, -0.4);
		expectThrow(badWeight, right, kTol, "ownership: non-positive weight throws");

		LN_NurbsSurface badFinite = left;
		badFinite.ControlPoints[1][1] = XYZW(1.0, std::nan(""), 1.0, 1.0);
		expectThrow(badFinite, right, kTol, "ownership: non-finite coordinate throws");

		LN_NurbsSurface badSize = left;
		badSize.KnotVectorU = { 0.0, 0.0, 1.0, 1.0, 1.0, 1.0 };
		expectThrow(badSize, right, kTol, "ownership: knot/net length mismatch throws");

		// A structurally consistent surface whose internal V multiplicity
		// exceeds its V degree (mult 3 with degree 2).
		LN_NurbsSurface badMultiplicity = BuildSurface(2, 2,
			{ 0.0, 0.0, 0.0, 1.0, 1.0, 1.0 },
			{ 0.0, 0.0, 0.0, 0.5, 0.5, 0.5, 1.0, 1.0, 1.0 },
			[](int, int) { return XYZW(XYZ(0.1, 0.2, 0.3), 1.0); });
		expectThrow(badMultiplicity, right, kTol,
			"ownership: internal multiplicity above degree throws");

		expectThrow(left, right, 0.0, "ownership: zero tolerance throws");
		expectThrow(left, right, std::numeric_limits<double>::infinity(),
			"ownership: infinite tolerance throws");
	}
}

void run_join_tests(const std::string& group)
{
	if (group == "basis")
	{
		RunBasisGroup();
	}
	else if (group == "rational")
	{
		RunRationalGroup();
	}
	else if (group == "ownership")
	{
		RunOwnershipGroup();
	}
	else
	{
		throw std::invalid_argument("unknown join test group: " + group);
	}
}

#endif // LNLIB_JOIN_CONTRACTS
