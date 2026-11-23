/*
MIT License

Copyright (c) 2018-2020 Jonathan Young

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// Generic periodic constraint elimination engine.
//
// Relations U_b = Q * U_a + k are eliminated with a union-find that carries
// the accumulated signed permutation transform and integer translation of
// every variable relative to its component representative. Closed loops are
// not treated as errors: their composed transform either conflicts
// (identity transform with a nonzero translation), pins the representative
// to a point (rotation loops) or restricts it to a line (reflection loops).
// The remaining independent coordinates parameterize a reduced sparse least
// squares system, solved with a Jacobi preconditioned conjugate gradient.

#include "periodic_constraints.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

namespace xatlas {

const char *StringForEnum(PeriodicError error)
{
	switch (error) {
	case PeriodicError::Success:
		return "Success";
	case PeriodicError::InvalidState:
		return "Invalid atlas state for periodic parameterization";
	case PeriodicError::InvalidAngle:
		return "Relation or pin references an invalid face corner";
	case PeriodicError::InvalidTransform:
		return "Relation transform is not a signed permutation matrix";
	case PeriodicError::NonFiniteGeometry:
		return "Non-finite geometry or pin value";
	case PeriodicError::DegenerateGeometry:
		return "Degenerate triangle geometry";
	case PeriodicError::InconsistentRelation:
		return "Inconsistent closed loop of relations";
	case PeriodicError::ConflictingPin:
		return "Conflicting pin constraints";
	case PeriodicError::RankDeficient:
		return "Constrained least squares system is rank deficient";
	case PeriodicError::SolveFailed:
		return "Numerical solve failed or residual tolerance was not met";
	default:
		return "Unknown error";
	}
}

namespace internal {
namespace periodic {


} // namespace periodic
} // namespace internal
} // namespace xatlas
