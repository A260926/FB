/*
 * Author:
 * 2023/06/08 - Yuqing Liang (BIMCoder Liang)
 * bim.frankliang@foxmail.com
 * 
 *
 * Use of this source code is governed by a LGPL-2.1 license that can be found in
 * the LICENSE file.
 */

#pragma once

#include "LNLibDefinitions.h"
#include <vector>

namespace LNLib
{
	class LNLIB_EXPORT Constants
	{

	public:

		static constexpr  double DoubleEpsilon = 1E-6;
		static constexpr  double DistanceEpsilon = 1E-4;
		static constexpr  double AngleEpsilon = 1E-2;
		static constexpr  double MaxDistance = 1E9;
		static constexpr  double Pi = 3.14159265358979323846;
		// Raised from 7 to 12 so surfaces produced by NurbsSurface::JoinAlongU,
		// whose V degree is the sum of the two input V degrees (at most 5+5),
		// can still be evaluated by the stock point/derivative evaluators.
		static constexpr  int NURBSMaxDegree = 12;
	};
}


