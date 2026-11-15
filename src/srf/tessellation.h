//-----------------------------------------------------------------------------
// Result of triangulating a trimmed surface or a whole shell. A result
// describes exactly one attempt: on Ok the target mesh received the newly
// generated triangles, and on any failure the target mesh was left in its
// state from before the attempt.
//
// Copyright 2008-2013 Jonathan Westhues.
//-----------------------------------------------------------------------------

#ifndef SOLVESPACE_TESSELLATION_H
#define SOLVESPACE_TESSELLATION_H

#include <cstdint>
#include <string>
#include <utility>

#include "handle.h"

namespace SolveSpace {

class hSSurface {
public:
    uint32_t v;
};

template<>
struct IsHandleOracle<hSSurface> : std::true_type {};

class hSCurve {
public:
    uint32_t v;
};

template<>
struct IsHandleOracle<hSCurve> : std::true_type {};

class TessellationResult {
public:
    enum class Code : uint32_t {
        // Every surface was triangulated to the requested tolerances.
        Ok              = 0,
        // A trim references a missing curve, a non-finite curve point, an
        // endpoint that cannot be located in the trim's traversal order, or
        // a trim that is empty or never reaches its finish point.
        InvalidTrim     = 1,
        // Trim edges do not assemble into closed loops, a hole could not be
        // bridged into its outer contour, or ear clipping found no ear.
        PolygonFailure  = 2,
        // The geometric chord/normal test still fails when the adaptive
        // grid can no longer subdivide (segment budget exhausted, or the
        // floating-point midpoint stops splitting the interval).
        LimitExceeded   = 3,
        // Surface degrees, weights or control points are invalid, an
        // evaluation produced a non-finite value, or a triangle collapsed
        // to zero area.
        InvalidGeometry = 4
    };

    Code        code;
    // Handle of the surface whose triangulation failed; zero when the
    // failure is not attributable to a surface (currently never, since both
    // reporting entry points operate on surfaces).
    hSSurface   surface;
    // Handle of the trim curve that exposes an invalid trim; zero when the
    // failure has no originating curve (grid limits, polygon assembly past
    // the trim walk, invalid surface data, ...).
    hSCurve     curve;
    // Human-readable detail, kept alongside the structured handles so the
    // display cache can show where a regeneration failed.
    std::string detail;

    static TessellationResult Success() {
        return { Code::Ok, {0}, {0}, {} };
    }
    static TessellationResult Failure(Code code, hSSurface srf, hSCurve crv,
                                      std::string detail) {
        return { code, srf, crv, std::move(detail) };
    }

    bool Ok() const { return code == Code::Ok; }
    explicit operator bool() const { return Ok(); }
};

} // namespace SolveSpace

#endif
