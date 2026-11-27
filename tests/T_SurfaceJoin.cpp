#include "support/checks.hpp"

#include "BsplineSurface.h"
#include "LNObject.h"
#include "NurbsSurface.h"
#include "UV.h"
#include "XYZ.h"
#include "XYZW.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace LNLib;

namespace
{
	const double kTolerance = 1e-10;
	const double kPointTolerance = 1e-8;
	const double kDerivativeTolerance = 1e-7;

	// Greville abscissae of the normalized ([0,1]) image of a clamped knot vector.
	// Placing a unit-weight seam row at (1, greville_j, 0) makes the boundary curve
	// interpolate the straight line (1, v, 0) exactly in the normalized parameter.
	std::vector<double> NormalizedGreville(int degree, const std::vector<double>& knots, int count)
	{
		double lo = knots.front();
		double hi = knots.back();
		std::vector<double> greville(count);
		for (int j = 0; j < count; ++j)
		{
			double sum = 0.0;
			for (int k = 1; k <= degree; ++k)
			{
				sum += (knots[j + k] - lo) / (hi - lo);
			}
			greville[j] = sum / degree;
		}
		return greville;
	}

	int KnotMultiplicity(const std::vector<double>& knots, double value)
	{
		int multiplicity = 0;
		for (size_t i = 0; i < knots.size(); ++i)
		{
			if (knots[i] == value)
			{
				++multiplicity;
			}
		}
		return multiplicity;
	}

	bool SurfacesIdentical(const LN_NurbsSurface& a, const LN_NurbsSurface& b)
	{
		if (a.DegreeU != b.DegreeU || a.DegreeV != b.DegreeV) return false;
		if (a.KnotVectorU != b.KnotVectorU || a.KnotVectorV != b.KnotVectorV) return false;
		if (a.ControlPoints.size() != b.ControlPoints.size()) return false;
		for (size_t i = 0; i < a.ControlPoints.size(); ++i)
		{
			if (a.ControlPoints[i].size() != b.ControlPoints[i].size()) return false;
			for (size_t j = 0; j < a.ControlPoints[i].size(); ++j)
			{
				for (int c = 0; c < 4; ++c)
				{
					if (a.ControlPoints[i][j][c] != b.ControlPoints[i][j][c]) return false;
				}
			}
		}
		return true;
	}

	bool AllWeightsPositive(const LN_NurbsSurface& surface)
	{
		for (size_t i = 0; i < surface.ControlPoints.size(); ++i)
		{
			for (size_t j = 0; j < surface.ControlPoints[i].size(); ++j)
			{
				double w = surface.ControlPoints[i][j].GetW();
				if (!(w > 0.0) || !std::isfinite(w)) return false;
			}
		}
		return true;
	}

	// ---------------- basis material ----------------
	// Left:  degU=2 on [2,5] with interior knot 3.5 (mult 1),
	//        degV=2 on [-1,2] with interior knot 0.5 (mult 2, repeated).
	// Right: degU=3 on [0,10] with interior knot 4 (mult 2, repeated),
	//        degV=3 on [1,4] with interior knot 2 (mult 1).
	// Both seams are the straight line (1, v, 0) in the normalized parameter; all
	// weights are 1, so the two weight functions are both identically 1.

	LN_NurbsSurface MakeBasisLeft()
	{
		LN_NurbsSurface surface;
		surface.DegreeU = 2;
		surface.DegreeV = 2;
		surface.KnotVectorU = { 2, 2, 2, 3.5, 5, 5, 5 };
		surface.KnotVectorV = { -1, -1, -1, 0.5, 0.5, 2, 2, 2 };
		int rows = 4;
		int columns = 5;
		std::vector<double> greville = NormalizedGreville(2, surface.KnotVectorV, columns);
		surface.ControlPoints.assign(rows, std::vector<XYZW>(columns));
		for (int i = 0; i < rows; ++i)
		{
			double x = i / 3.0;
			for (int j = 0; j < columns; ++j)
			{
				double z = (1.0 - x) * (0.3 + 0.2 * std::sin(2.0 * j));
				surface.ControlPoints[i][j] = XYZW(x, greville[j], z, 1.0);
			}
		}
		return surface;
	}

	LN_NurbsSurface MakeBasisRight()
	{
		LN_NurbsSurface surface;
		surface.DegreeU = 3;
		surface.DegreeV = 3;
		surface.KnotVectorU = { 0, 0, 0, 0, 4, 4, 10, 10, 10, 10 };
		surface.KnotVectorV = { 1, 1, 1, 1, 2, 4, 4, 4, 4 };
		int rows = 6;
		int columns = 5;
		std::vector<double> greville = NormalizedGreville(3, surface.KnotVectorV, columns);
		surface.ControlPoints.assign(rows, std::vector<XYZW>(columns));
		for (int i = 0; i < rows; ++i)
		{
			double x = 1.0 + i / 5.0;
			for (int j = 0; j < columns; ++j)
			{
				double z = (x - 1.0) * (0.4 + 0.1 * std::cos(2.0 * j));
				surface.ControlPoints[i][j] = XYZW(x, greville[j], z, 1.0);
			}
		}
		return surface;
	}

	// Same surface as MakeBasisRight but with the V direction reversed in the data.
	LN_NurbsSurface MakeBasisRightReversed()
	{
		LN_NurbsSurface surface = MakeBasisRight();
		std::vector<double> knots = surface.KnotVectorV;
		for (size_t i = 0; i < knots.size(); ++i)
		{
			surface.KnotVectorV[i] = 1.0 + 4.0 - knots[knots.size() - 1 - i];
		}
		for (size_t i = 0; i < surface.ControlPoints.size(); ++i)
		{
			std::reverse(surface.ControlPoints[i].begin(), surface.ControlPoints[i].end());
		}
		return surface;
	}

	// ---------------- rational material ----------------
	// Both seams trace the straight line (1, v, 0) in the normalized parameter, but the
	// left seam weight function is identically 1 while the right one is wR(v) = 1 + 0.5v,
	// so the weight ratio along the seam is genuinely non-constant.
	// Left:  degU=2 on [0.5,2.5], degV=2 on [-2,4], Bezier in both axes.
	// Right: degU=3 on [10,20], degV=2 on [5,9], Bezier in both axes.

	LN_NurbsSurface MakeRationalLeft()
	{
		LN_NurbsSurface surface;
		surface.DegreeU = 2;
		surface.DegreeV = 2;
		surface.KnotVectorU = { 0.5, 0.5, 0.5, 2.5, 2.5, 2.5 };
		surface.KnotVectorV = { -2, -2, -2, 4, 4, 4 };
		surface.ControlPoints.assign(3, std::vector<XYZW>(3));
		for (int i = 0; i < 3; ++i)
		{
			double x = 0.5 * i;
			for (int j = 0; j < 3; ++j)
			{
				double y = 0.5 * j;
				double z = (1.0 - x) * (0.3 + 0.1 * j);
				double w = (i < 2) ? (0.8 + 0.4 * i + 0.15 * j) : 1.0;
				surface.ControlPoints[i][j] = XYZW(x * w, y * w, z * w, w);
			}
		}
		return surface;
	}

	LN_NurbsSurface MakeRationalRight()
	{
		LN_NurbsSurface surface;
		surface.DegreeU = 3;
		surface.DegreeV = 2;
		surface.KnotVectorU = { 10, 10, 10, 10, 20, 20, 20, 20 };
		surface.KnotVectorV = { 5, 5, 5, 9, 9, 9 };
		surface.ControlPoints.assign(4, std::vector<XYZW>(3));
		// Seam row: homogeneous form of (1, v, 0) with weight function 1 + 0.5v.
		surface.ControlPoints[0][0] = XYZW(1.0, 0.0, 0.0, 1.0);
		surface.ControlPoints[0][1] = XYZW(1.25, 0.5, 0.0, 1.25);
		surface.ControlPoints[0][2] = XYZW(1.5, 1.5, 0.0, 1.5);
		for (int i = 1; i < 4; ++i)
		{
			double x = 1.0 + i / 3.0;
			for (int j = 0; j < 3; ++j)
			{
				double y = 0.5 * j;
				double z = (x - 1.0) * (0.2 + 0.1 * j);
				double w = 0.6 + 0.3 * i + 0.1 * j;
				surface.ControlPoints[i][j] = XYZW(x * w, y * w, z * w, w);
			}
		}
		return surface;
	}

	LN_NurbsSurface Scaled(const LN_NurbsSurface& surface, double factor)
	{
		LN_NurbsSurface scaled = surface;
		for (size_t i = 0; i < scaled.ControlPoints.size(); ++i)
		{
			for (size_t j = 0; j < scaled.ControlPoints[i].size(); ++j)
			{
				scaled.ControlPoints[i][j] = scaled.ControlPoints[i][j] * factor;
			}
		}
		return scaled;
	}

	double MaxPointError(const LN_NurbsSurface& joined, const LN_NurbsSurface& left, const LN_NurbsSurface& right,
		double leftU0, double leftU1, double leftV0, double leftV1,
		double rightU0, double rightU1, double rightV0, double rightV1,
		bool reverseRightV, int samplesU, int samplesV)
	{
		double error = 0.0;
		for (int i = 0; i <= samplesU; ++i)
		{
			double u = 2.0 * i / samplesU;
			for (int j = 0; j <= samplesV; ++j)
			{
				double v = static_cast<double>(j) / samplesV;
				XYZ point = NurbsSurface::GetPointOnSurface(joined, UV(u, v));
				XYZ reference;
				if (u <= 1.0)
				{
					reference = NurbsSurface::GetPointOnSurface(left, UV(leftU0 + u * (leftU1 - leftU0), leftV0 + v * (leftV1 - leftV0)));
				}
				else
				{
					double rv = reverseRightV ? (1.0 - v) : v;
					reference = NurbsSurface::GetPointOnSurface(right, UV(rightU0 + (u - 1.0) * (rightU1 - rightU0), rightV0 + rv * (rightV1 - rightV0)));
				}
				error = std::max(error, point.Distance(reference));
			}
		}
		return error;
	}

	void RunBasisTests()
	{
		LN_NurbsSurface left = MakeBasisLeft();
		LN_NurbsSurface right = MakeBasisRight();

		LN_NurbsSurface joined;
		bool ok = NurbsSurface::JoinAlongU(left, right, false, kTolerance, joined);
		join_checks::require(ok, "basis: join of mixed-degree surfaces must succeed");

		join_checks::require(joined.DegreeU == 3, "basis: output U degree must be max(2,3)");
		join_checks::require(joined.DegreeV == 5, "basis: output V degree must be the sum 2+3");

		// U knots: [0 x4, 0.5 x2, 1 x3, (0.1*4+1) x2, 2 x4].
		const double leftInteriorU = (1.0 / 3.0) * (3.5 - 2.0);
		const double rightInteriorU = 0.1 * 4.0 + 1.0;
		join_checks::require(KnotMultiplicity(joined.KnotVectorU, 0.0) == 4, "basis: clamped U start multiplicity");
		join_checks::require(KnotMultiplicity(joined.KnotVectorU, leftInteriorU) == 2, "basis: left interior U knot keeps elevated multiplicity 1+1");
		join_checks::require(KnotMultiplicity(joined.KnotVectorU, 1.0) == 3, "basis: seam knot multiplicity must be exactly degreeU");
		join_checks::require(KnotMultiplicity(joined.KnotVectorU, rightInteriorU) == 2, "basis: right interior U knot keeps multiplicity 2");
		join_checks::require(KnotMultiplicity(joined.KnotVectorU, 2.0) == 4, "basis: clamped U end multiplicity");
		join_checks::require(joined.KnotVectorU.size() == 15, "basis: U knot vector size");

		// V knots: [0 x6, 1/3 x5, 0.5 x5, 1 x6] on the union of both interior knot lists.
		const double rightInteriorV = 1.0 / 3.0;
		const double leftInteriorV = (1.0 / 3.0) * 1.5;
		join_checks::require(KnotMultiplicity(joined.KnotVectorV, 0.0) == 6, "basis: clamped V start multiplicity");
		join_checks::require(KnotMultiplicity(joined.KnotVectorV, rightInteriorV) == 5, "basis: right interior V knot has multiplicity degreeV");
		join_checks::require(KnotMultiplicity(joined.KnotVectorV, leftInteriorV) == 5, "basis: repeated left interior V knot appears once per segment");
		join_checks::require(KnotMultiplicity(joined.KnotVectorV, 1.0) == 6, "basis: clamped V end multiplicity");

		join_checks::require(joined.ControlPoints.size() == 11, "basis: control net rows = 6+6-1");
		join_checks::require(joined.ControlPoints[0].size() == 16, "basis: control net columns = 3*5+1");
		join_checks::require(AllWeightsPositive(joined), "basis: result weights must stay positive and finite");

		double error = MaxPointError(joined, left, right, 2, 5, -1, 2, 0, 10, 1, 4, false, 24, 24);
		join_checks::require(error < kPointTolerance, "basis: interior points on both sides match the original evaluators");

		// Seam points from both sides and from the joined surface must coincide (C0 only).
		double seamError = 0.0;
		for (int k = 0; k <= 16; ++k)
		{
			double v = k / 16.0;
			XYZ onJoin = NurbsSurface::GetPointOnSurface(joined, UV(1.0, v));
			XYZ onLeft = NurbsSurface::GetPointOnSurface(left, UV(5.0, -1.0 + 3.0 * v));
			XYZ onRight = NurbsSurface::GetPointOnSurface(right, UV(0.0, 1.0 + 3.0 * v));
			seamError = std::max(seamError, onJoin.Distance(onLeft));
			seamError = std::max(seamError, onJoin.Distance(onRight));
		}
		join_checks::require(seamError < kPointTolerance, "basis: whole seam agrees with both original surfaces");

		// Right patch with reversed V data must join with reverseRightV=true and give the
		// same geometry; references are evaluated on the reversed input itself.
		LN_NurbsSurface reversedRight = MakeBasisRightReversed();
		LN_NurbsSurface joinedReversed;
		bool okReversed = NurbsSurface::JoinAlongU(left, reversedRight, true, kTolerance, joinedReversed);
		join_checks::require(okReversed, "basis: join with reverseRightV=true must succeed");
		join_checks::require(joinedReversed.DegreeU == 3 && joinedReversed.DegreeV == 5, "basis: reversed join keeps the same degrees");
		double errorReversed = MaxPointError(joinedReversed, left, reversedRight, 2, 5, -1, 2, 0, 10, 1, 4, true, 24, 24);
		join_checks::require(errorReversed < kPointTolerance, "basis: reversed join matches the original evaluators");
	}

	void RunRationalTests()
	{
		LN_NurbsSurface left = MakeRationalLeft();
		LN_NurbsSurface right = MakeRationalRight();

		// Material witness: the seam weight functions differ and their ratio is not constant.
		double wL0 = BsplineSurface::GetPointOnSurface(left, UV(2.5, -2.0 + 6.0 * 0.3)).GetW();
		double wL1 = BsplineSurface::GetPointOnSurface(left, UV(2.5, -2.0 + 6.0 * 0.7)).GetW();
		double wR0 = BsplineSurface::GetPointOnSurface(right, UV(10.0, 5.0 + 4.0 * 0.3)).GetW();
		double wR1 = BsplineSurface::GetPointOnSurface(right, UV(10.0, 5.0 + 4.0 * 0.7)).GetW();
		join_checks::require(std::abs(wL0 - 1.0) < 1e-12 && std::abs(wL1 - 1.0) < 1e-12, "rational: left seam weight function is constant 1");
		join_checks::require(std::abs(wR0 - 1.15) < 1e-12 && std::abs(wR1 - 1.35) < 1e-12, "rational: right seam weight function is 1+0.5v");
		join_checks::require(std::abs(wR0 / wL0 - wR1 / wL1) > 0.1, "rational: seam weight ratio is non-constant");

		LN_NurbsSurface joined;
		bool ok = NurbsSurface::JoinAlongU(left, right, false, kTolerance, joined);
		join_checks::require(ok, "rational: join with non-constant seam weight ratio must succeed");
		join_checks::require(joined.DegreeU == 3 && joined.DegreeV == 4, "rational: output degrees are max(2,3) and 2+2");
		join_checks::require(AllWeightsPositive(joined), "rational: result weights must stay positive and finite");

		// Whole seam, checked against both original evaluators.
		double seamError = 0.0;
		for (int k = 0; k <= 32; ++k)
		{
			double v = k / 32.0;
			XYZ onJoin = NurbsSurface::GetPointOnSurface(joined, UV(1.0, v));
			XYZ onLeft = NurbsSurface::GetPointOnSurface(left, UV(2.5, -2.0 + 6.0 * v));
			XYZ onRight = NurbsSurface::GetPointOnSurface(right, UV(10.0, 5.0 + 4.0 * v));
			seamError = std::max(seamError, onJoin.Distance(onLeft));
			seamError = std::max(seamError, onJoin.Distance(onRight));
		}
		join_checks::require(seamError < kPointTolerance, "rational: whole seam agrees with both original surfaces");

		double error = MaxPointError(joined, left, right, 0.5, 2.5, -2, 4, 10, 20, 5, 9, false, 24, 24);
		join_checks::require(error < kPointTolerance, "rational: interior points on both sides match the original evaluators");

		// Analytic first derivatives away from knots, referenced against the original
		// evaluators with the domain scaling applied.
		std::vector<std::vector<XYZ>> djL = NurbsSurface::ComputeRationalSurfaceDerivatives(joined, 1, UV(0.37, 0.29));
		std::vector<std::vector<XYZ>> daL = NurbsSurface::ComputeRationalSurfaceDerivatives(left, 1, UV(0.5 + 2.0 * 0.37, -2.0 + 6.0 * 0.29));
		join_checks::require(djL[1][0].Distance(2.0 * daL[1][0]) < kDerivativeTolerance, "rational: du derivative on the left half matches the scaled original");
		join_checks::require(djL[0][1].Distance(6.0 * daL[0][1]) < kDerivativeTolerance, "rational: dv derivative on the left half matches the scaled original");

		std::vector<std::vector<XYZ>> djR = NurbsSurface::ComputeRationalSurfaceDerivatives(joined, 1, UV(1.53, 0.71));
		std::vector<std::vector<XYZ>> daR = NurbsSurface::ComputeRationalSurfaceDerivatives(right, 1, UV(10.0 + 10.0 * 0.53, 5.0 + 4.0 * 0.71));
		join_checks::require(djR[1][0].Distance(10.0 * daR[1][0]) < kDerivativeTolerance, "rational: du derivative on the right half matches the scaled original");
		join_checks::require(djR[0][1].Distance(4.0 * daR[0][1]) < kDerivativeTolerance, "rational: dv derivative on the right half matches the scaled original");

		// Scaling all homogeneous control points of either input by a positive number must
		// not change the output geometry.
		LN_NurbsSurface joinedScaledLeft;
		bool okScaledLeft = NurbsSurface::JoinAlongU(Scaled(left, 2.5), right, false, kTolerance, joinedScaledLeft);
		join_checks::require(okScaledLeft, "rational: join with a scaled left input must succeed");
		double errorScaledLeft = MaxPointError(joinedScaledLeft, left, right, 0.5, 2.5, -2, 4, 10, 20, 5, 9, false, 16, 16);
		join_checks::require(errorScaledLeft < kPointTolerance, "rational: scaling the left input does not change the output geometry");

		LN_NurbsSurface joinedScaledRight;
		bool okScaledRight = NurbsSurface::JoinAlongU(left, Scaled(right, 0.4), false, kTolerance, joinedScaledRight);
		join_checks::require(okScaledRight, "rational: join with a scaled right input must succeed");
		double errorScaledRight = MaxPointError(joinedScaledRight, left, right, 0.5, 2.5, -2, 4, 10, 20, 5, 9, false, 16, 16);
		join_checks::require(errorScaledRight < kPointTolerance, "rational: scaling the right input does not change the output geometry");
	}

	void ExpectInvalidArgument(const LN_NurbsSurface& left, const LN_NurbsSurface& right, double tolerance, const LN_NurbsSurface& sentinel, const char* message)
	{
		LN_NurbsSurface result = sentinel;
		bool threw = false;
		try
		{
			NurbsSurface::JoinAlongU(left, right, false, tolerance, result);
		}
		catch (const std::invalid_argument&)
		{
			threw = true;
		}
		join_checks::require(threw, message);
		join_checks::require(SurfacesIdentical(result, sentinel), "ownership: failed calls must leave result untouched");
	}

	void RunOwnershipTests()
	{
		LN_NurbsSurface left = MakeRationalLeft();
		LN_NurbsSurface right = MakeRationalRight();
		LN_NurbsSurface sentinel = MakeBasisLeft();

		// Aliasing: result may be the same object as the left input; the other input is unchanged.
		LN_NurbsSurface aliasLeft = left;
		bool okAliasLeft = NurbsSurface::JoinAlongU(aliasLeft, right, false, kTolerance, aliasLeft);
		join_checks::require(okAliasLeft, "ownership: aliasing result with the left input must work");
		join_checks::require(aliasLeft.DegreeU == 3 && aliasLeft.ControlPoints.size() == 7, "ownership: aliased result holds the joined surface");
		XYZ aliasPoint = NurbsSurface::GetPointOnSurface(aliasLeft, UV(0.37, 0.29));
		XYZ aliasReference = NurbsSurface::GetPointOnSurface(left, UV(0.5 + 2.0 * 0.37, -2.0 + 6.0 * 0.29));
		join_checks::require(aliasPoint.Distance(aliasReference) < kPointTolerance, "ownership: aliased result evaluates like the original left half");
		join_checks::require(SurfacesIdentical(right, MakeRationalRight()), "ownership: the non-aliased input is unchanged");

		// Aliasing with the right input.
		LN_NurbsSurface aliasRight = right;
		bool okAliasRight = NurbsSurface::JoinAlongU(left, aliasRight, false, kTolerance, aliasRight);
		join_checks::require(okAliasRight, "ownership: aliasing result with the right input must work");
		join_checks::require(SurfacesIdentical(left, MakeRationalLeft()), "ownership: the non-aliased left input is unchanged");

		// Independent storage: the result owns its own net and knots; inputs are not touched.
		LN_NurbsSurface joined;
		bool ok = NurbsSurface::JoinAlongU(left, right, false, kTolerance, joined);
		join_checks::require(ok, "ownership: plain join must succeed");
		join_checks::require(SurfacesIdentical(left, MakeRationalLeft()) && SurfacesIdentical(right, MakeRationalRight()), "ownership: inputs are not modified by a successful join");
		joined.ControlPoints[0][0] = XYZW(999.0, 999.0, 999.0, 1.0);
		joined.KnotVectorU[0] = -7.0;
		join_checks::require(left.ControlPoints[0][0].GetWX() != 999.0 && left.KnotVectorU[0] != -7.0, "ownership: mutating the result does not reach into the inputs");

		// Valid but incompatible: moving one interior seam control point off the shared
		// curve must be rejected with false, and result keeps its previous value.
		LN_NurbsSurface incompatible = MakeRationalRight();
		incompatible.ControlPoints[0][1].WX() += 0.3;
		LN_NurbsSurface result = sentinel;
		bool okIncompatible = NurbsSurface::JoinAlongU(left, incompatible, false, kTolerance, result);
		join_checks::require(!okIncompatible, "ownership: an incompatible seam must return false");
		join_checks::require(SurfacesIdentical(result, sentinel), "ownership: result is unchanged after a false return");

		// Seam endpoints coincide but the middle separates: must also be rejected.
		LN_NurbsSurface separated = MakeRationalRight();
		separated.ControlPoints[0][0] = XYZW(1.0, 0.0, 0.0, 1.0);
		separated.ControlPoints[0][1] = XYZW(1.4, 0.5, 0.0, 1.0);
		separated.ControlPoints[0][2] = XYZW(1.0, 1.0, 0.0, 1.0);
		result = sentinel;
		bool okSeparated = NurbsSurface::JoinAlongU(left, separated, false, kTolerance, result);
		join_checks::require(!okSeparated, "ownership: a seam matching only at the endpoints must return false");
		join_checks::require(SurfacesIdentical(result, sentinel), "ownership: result is unchanged after the endpoint-only seam fails");

		// Invalid tolerance values.
		ExpectInvalidArgument(left, right, 0.0, sentinel, "ownership: zero tolerance must throw");
		ExpectInvalidArgument(left, right, -1e-10, sentinel, "ownership: negative tolerance must throw");
		ExpectInvalidArgument(left, right, std::nan(""), sentinel, "ownership: NaN tolerance must throw");

		// Structurally invalid inputs.
		LN_NurbsSurface negativeWeight = MakeRationalLeft();
		negativeWeight.ControlPoints[1][1].W() = -0.5;
		ExpectInvalidArgument(negativeWeight, right, kTolerance, sentinel, "ownership: a non-positive weight must throw");

		LN_NurbsSurface unclamped = MakeRationalLeft();
		unclamped.KnotVectorU = { 0.5, 0.6, 0.7, 2.0, 2.4, 2.5 };
		ExpectInvalidArgument(unclamped, right, kTolerance, sentinel, "ownership: a non-clamped knot vector must throw");

		LN_NurbsSurface ragged = MakeRationalLeft();
		ragged.ControlPoints[1].pop_back();
		ExpectInvalidArgument(ragged, right, kTolerance, sentinel, "ownership: a non-rectangular control net must throw");

		LN_NurbsSurface mismatched = MakeRationalLeft();
		mismatched.KnotVectorU.pop_back();
		ExpectInvalidArgument(mismatched, right, kTolerance, sentinel, "ownership: knot/control count mismatch must throw");

		LN_NurbsSurface nanPoint = MakeRationalLeft();
		nanPoint.ControlPoints[0][0].WX() = std::nan("");
		ExpectInvalidArgument(nanPoint, right, kTolerance, sentinel, "ownership: a NaN coordinate must throw");

		LN_NurbsSurface excessiveMultiplicity;
		excessiveMultiplicity.DegreeU = 1;
		excessiveMultiplicity.DegreeV = 2;
		excessiveMultiplicity.KnotVectorU = { 0, 0, 1, 1 };
		excessiveMultiplicity.KnotVectorV = { 0, 0, 0, 0.5, 0.5, 0.5, 1, 1, 1 };
		excessiveMultiplicity.ControlPoints.assign(2, std::vector<XYZW>(6, XYZW(0, 0, 0, 1)));
		ExpectInvalidArgument(excessiveMultiplicity, right, kTolerance, sentinel, "ownership: interior multiplicity above the degree must throw");
	}
}

void run_join_tests(const std::string& group)
{
	if (group == "basis")
	{
		RunBasisTests();
	}
	else if (group == "rational")
	{
		RunRationalTests();
	}
	else if (group == "ownership")
	{
		RunOwnershipTests();
	}
	else
	{
		throw std::invalid_argument("unknown join test group: " + group);
	}
}
