/*
Copyright (c) 2013 Benedikt Bitterli

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#include "VoxelInterfaces.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace {

const int MaxTreeDepth = 53;

inline int popcount8(uint32 value) {
    int count = 0;
    while (value) {
        count += int(value & 1u);
        value >>= 1;
    }
    return count;
}

inline bool isFinite(float value) {
    return std::isfinite(double(value));
}

inline bool isFinite(double value) {
    return std::isfinite(value);
}

inline bool isAirMaterial(const DielectricMaterial &material) {
    return material.ior == 1.0f &&
           material.absorption.x == 0.0f &&
           material.absorption.y == 0.0f &&
           material.absorption.z == 0.0f;
}

void invalidArgument(const char *message) {
    throw std::invalid_argument(std::string("VoxelInterfaces: ") + message);
}

/*
 * A cell of the walk: absent child octant (air), a leaf carrying a raw
 * payload, or the unbounded region outside the root cube.  Bounds are exact
 * dyadic values: level is the octree level (size = 2^-level) and lo[] is the
 * low corner in u space, with u = p - 1.
 */
struct WalkCell {
    enum Kind {
        Outside = 0,
        Air,
        Leaf
    };

    Kind kind;
    int level;
    double lo[3];
    double size;
    uint32 payload;
    uint32 medium;
    double enterT;
    double exitT;
    int exitAxis;
    int exitSign;

    WalkCell()
    : kind(Outside), level(0), size(1.0), payload(0), medium(0),
      enterT(0.0), exitT(std::numeric_limits<double>::infinity()),
      exitAxis(-1), exitSign(0)
    {
        lo[0] = lo[1] = lo[2] = 0.0;
    }
};

} /* namespace */

struct VoxelInterfaceCursorState {
    const VoxelInterfaces *resource;

    double origin[3];
    double dir[3];
    double speed;
    double tMin;
    double tMax;

    WalkCell cell;

    double segStartT;
    uint32 segMedium;
    uint32 segPayload;

    bool hasPending;
    VoxelInterfaces::InterfaceEvent pending;

    VoxelInterfaces::SegmentInfo firstSegment;
    VoxelInterfaces::SegmentInfo lastSegment;
    VoxelInterfaces::InterfaceEvent lastEvent;
    bool hasLastEvent;

    /* Explicit face data of the most recent interface, used to fork. */
    double lastFaceCoord;
    int lastFaceAxis;
    int lastTravelSign;
    uint32 lastMediumBefore;
    uint32 lastMediumAfter;

    /* Ray is outside the root and propagating away: background here. */
    bool outsideHasBackground;

    bool finished;
    bool reachedBackground;
    Vec3 backgroundDirection;

    uint64 reads;

    VoxelInterfaceCursorState()
    : resource(0), speed(1.0), tMin(0.0), tMax(0.0),
      cell(), segStartT(0.0), segMedium(0), segPayload(0),
      hasPending(false), pending(), firstSegment(), lastSegment(), lastEvent(),
      hasLastEvent(false), lastFaceCoord(0.0), lastFaceAxis(-1),
      lastTravelSign(0), lastMediumBefore(0), lastMediumAfter(0),
      outsideHasBackground(false),
      finished(false), reachedBackground(false), backgroundDirection(), reads(0)
    {
        origin[0] = origin[1] = origin[2] = 0.0;
        dir[0] = dir[1] = dir[2] = 0.0;
    }

    inline double point(int axis, double t) const {
        return origin[axis] + dir[axis]*t;
    }

    inline uint32 readWord(uint64 index) const {
        VoxelInterfaceCursorState *self = const_cast<VoxelInterfaceCursorState *>(this);
        ++self->reads;
        resource->noteDescriptorRead();
        return resource->tree().descriptorWord(index);
    }

    WalkCell locateCell(double px, double py, double pz,
                        int overrideAxis, double face, int sideSign) const;
};

namespace {

bool rootInterval(const double origin[3], const double dir[3],
                  double &tEnter, double &tExit) {
    tEnter = -std::numeric_limits<double>::infinity();
    tExit  =  std::numeric_limits<double>::infinity();

    for (int axis = 0; axis < 3; ++axis) {
        double o = origin[axis];
        double d = dir[axis];
        if (d == 0.0) {
            if (o < 0.0 || o > 1.0)
                return false;
        } else {
            double t0 = (0.0 - o)/d;
            double t1 = (1.0 - o)/d;
            if (t0 > t1)
                std::swap(t0, t1);
            tEnter = std::max(tEnter, t0);
            tExit  = std::min(tExit,  t1);
        }
    }
    return tExit >= tEnter;
}

} /* namespace */

/*
 * Descend from the root to the cell containing the query point.  If
 * overrideAxis >= 0 that axis coordinate is taken from the exact dyadic face
 * value `face`; when the point lies exactly on a cell midpoint, `sideSign`
 * chooses the octant.  This crosses boundaries using explicit side state
 * rather than epsilon/predicate nudging.
 */
WalkCell VoxelInterfaceCursorState::locateCell(
                    double px, double py, double pz,
                    int overrideAxis, double face, int sideSign) const {
    WalkCell cell;
    uint64 index = 0;
    int level = 0;
    double lo[3] = {0.0, 0.0, 0.0};
    double size = 1.0;
    double p[3] = {px, py, pz};

    const uint64 count = resource->tree().descriptorCount();
    if (count == 0)
        invalidArgument("empty descriptor array");

    for (;;) {
        if (index >= count)
            invalidArgument("descriptor index out of range");

        uint32 word = readWord(index);
        uint32 childMask    = (word >> 8) & 0xFFu;
        uint32 internalMask = word & 0xFFu;

        double half = size*0.5;
        int octant = 0;
        for (int axis = 0; axis < 3; ++axis) {
            double mid = lo[axis] + half;
            bool high;
            if (axis == overrideAxis) {
                if (face > mid)
                    high = true;
                else if (face < mid)
                    high = false;
                else
                    high = sideSign > 0;
            } else {
                high = p[axis] >= mid;
            }
            if (high)
                octant |= 1 << axis;
        }

        int childLevel = level + 1;
        double childLo[3];
        childLo[0] = lo[0] + ((octant & 1) ? half : 0.0);
        childLo[1] = lo[1] + ((octant & 2) ? half : 0.0);
        childLo[2] = lo[2] + ((octant & 4) ? half : 0.0);

        if (!(childMask & (1u << octant))) {
            cell.kind = WalkCell::Air;
            cell.level = childLevel;
            cell.lo[0] = childLo[0];
            cell.lo[1] = childLo[1];
            cell.lo[2] = childLo[2];
            cell.size = half;
            cell.payload = 0;
            cell.medium = VoxelInterfaces::AirMedium;
            return cell;
        }

        uint64 low = uint64(word >> 18);
        if (word & 0x20000u)
            low = (low << 32) | uint64(readWord(index + 1));
        uint64 base = index + low;
        if (base > count)
            invalidArgument("child address out of range");

        /* Every present child owns one slot at base + rank, in octant order.
         * A group with long addresses reserves two words per child. */
        int rank = popcount8(childMask & ((1u << octant) - 1u));
        uint64 slot = base + uint64((word & 0x10000u) ? 2*rank : rank);
        if (slot >= count)
            invalidArgument("child slot index out of range");

        if (!(internalMask & (1u << octant))) {
            uint32 payload = readWord(slot);

            cell.kind = WalkCell::Leaf;
            cell.level = childLevel;
            cell.lo[0] = childLo[0];
            cell.lo[1] = childLo[1];
            cell.lo[2] = childLo[2];
            cell.size = half;
            cell.payload = payload;
            cell.medium = resource->lookupMedium(payload);
            return cell;
        }

        uint64 childIndex = slot;

        index = childIndex;
        level = childLevel;
        if (level > MaxTreeDepth)
            invalidArgument("descriptor hierarchy too deep or cyclic");

        lo[0] = childLo[0];
        lo[1] = childLo[1];
        lo[2] = childLo[2];
        size = half;
    }
}

namespace {

void computeExit(const double origin[3], const double dir[3], WalkCell &cell) {
    double best = std::numeric_limits<double>::infinity();
    int bestAxis = -1;
    int bestSign = 0;

    for (int axis = 0; axis < 3; ++axis) {
        double d = dir[axis];
        if (d == 0.0)
            continue;
        double boundary = d > 0.0 ? cell.lo[axis] + cell.size : cell.lo[axis];
        double t = (boundary - origin[axis])/d;
        if (t < best) {
            best = t;
            bestAxis = axis;
            bestSign = d > 0.0 ? 1 : -1;
        }
    }

    cell.exitT = best;
    cell.exitAxis = bestAxis;
    cell.exitSign = bestSign;
}

inline void fillPosition(const VoxelInterfaceCursorState &state, double t,
                         VoxelInterfaces::InterfaceEvent &event) {
    double x = state.point(0, t) + 1.0;
    double y = state.point(1, t) + 1.0;
    double z = state.point(2, t) + 1.0;
    event.positionX = x;
    event.positionY = y;
    event.positionZ = z;
    event.position = Vec3(float(x), float(y), float(z));
}

inline VoxelInterfaces::SegmentInfo makeSegment(uint32 medium, uint32 payload,
                                                double startT, double endT,
                                                double speed) {
    VoxelInterfaces::SegmentInfo seg;
    seg.medium = medium;
    seg.payload = payload;
    seg.startT = startT;
    seg.endT = endT;
    seg.length = speed*(endT - startT);
    return seg;
}

void enterRoot(VoxelInterfaceCursorState &state, double t) {
    int axis = state.cell.exitAxis;
    int sign = state.cell.exitSign;
    double face = sign > 0 ? 0.0 : 1.0;

    WalkCell next = state.locateCell(state.point(0, t), state.point(1, t),
                               state.point(2, t), axis, face, sign);
    next.enterT = t;
    computeExit(state.origin, state.dir, next);
    state.cell = next;
}

void finishTerminal(VoxelInterfaceCursorState &state,
                    VoxelInterfaces::SegmentInfo &segment,
                    bool background, double endT) {
    segment = makeSegment(state.segMedium, state.segPayload,
                          state.segStartT, endT, state.speed);
    if (background) {
        segment.reachedBackground = true;
        state.reachedBackground = true;
        state.backgroundDirection = Vec3(float(state.dir[0]/state.speed),
                                         float(state.dir[1]/state.speed),
                                         float(state.dir[2]/state.speed));
    } else {
        segment.endsAtWindow = true;
        state.reachedBackground = false;
        state.backgroundDirection = Vec3(0.0f, 0.0f, 0.0f);
    }
    state.lastSegment = segment;
    state.finished = true;
}

/*
 * Walk until the next medium-number change or until the ray leaves the region
 * that can contribute.  Returns true and fills `event` on a change; returns
 * false on a terminal segment.  `segment` describes the maximal same-medium
 * segment that just ended.
 */
bool advance(VoxelInterfaceCursorState &state,
             VoxelInterfaces::InterfaceEvent &event,
             VoxelInterfaces::SegmentInfo &segment) {
    for (;;) {
        WalkCell &cell = state.cell;

        if (cell.kind == WalkCell::Outside) {
            if (cell.exitT <= state.tMax) {
                double t = cell.exitT;
                int axis = cell.exitAxis;
                int sign = cell.exitSign;

                enterRoot(state, t);

                if (cell.medium == state.segMedium)
                    continue;

                event.t = t;
                event.segmentStartT = state.segStartT;
                event.segmentLength = state.speed*(t - state.segStartT);
                fillPosition(state, t, event);
                event.mediumBefore = state.segMedium;
                event.mediumAfter = cell.medium;
                event.payload = cell.payload;
                event.normal = Vec3(0.0f, 0.0f, 0.0f);
                event.normal.a[axis] = float(-sign);
                event.escapedRoot = false;

                segment = makeSegment(state.segMedium, state.segPayload,
                                      state.segStartT, t, state.speed);
                segment.endsAtInterface = true;

                state.lastFaceAxis = axis;
                state.lastFaceCoord = sign > 0 ? 0.0 : 1.0;
                state.lastTravelSign = sign;
                state.lastMediumBefore = state.segMedium;
                state.lastMediumAfter = cell.medium;

                state.segStartT = t;
                state.segMedium = cell.medium;
                state.segPayload = cell.payload;
                return true;
            }

            if (state.outsideHasBackground) {
                finishTerminal(state, segment, true, state.segStartT);
            } else {
                finishTerminal(state, segment, false, state.tMax);
            }
            return false;
        }

        if (!(cell.exitT > state.tMax)) {
            int axis = cell.exitAxis;
            int sign = cell.exitSign;
            double face = sign > 0 ? cell.lo[axis] + cell.size : cell.lo[axis];
            bool exitsRoot = (sign > 0 && face == 1.0) || (sign < 0 && face == 0.0);

            if (exitsRoot) {
                double t = cell.exitT;

                if (state.segMedium == VoxelInterfaces::AirMedium) {
                    state.cell = WalkCell();
                    state.cell.kind = WalkCell::Outside;
                    state.cell.exitT = std::numeric_limits<double>::infinity();
                    state.outsideHasBackground = true;
                    finishTerminal(state, segment, true, t);
                    return false;
                }

                event.t = t;
                event.segmentStartT = state.segStartT;
                event.segmentLength = state.speed*(t - state.segStartT);
                fillPosition(state, t, event);
                event.mediumBefore = state.segMedium;
                event.mediumAfter = VoxelInterfaces::AirMedium;
                event.payload = 0;
                event.normal = Vec3(0.0f, 0.0f, 0.0f);
                event.normal.a[axis] = float(-sign);
                event.escapedRoot = true;

                segment = makeSegment(state.segMedium, state.segPayload,
                                      state.segStartT, t, state.speed);
                segment.endsAtInterface = true;

                state.lastFaceAxis = axis;
                state.lastFaceCoord = face;
                state.lastTravelSign = sign;
                state.lastMediumBefore = state.segMedium;
                state.lastMediumAfter = VoxelInterfaces::AirMedium;

                state.cell = WalkCell();
                state.cell.kind = WalkCell::Outside;
                state.cell.exitT = std::numeric_limits<double>::infinity();
                state.outsideHasBackground = true;

                state.segStartT = t;
                state.segMedium = VoxelInterfaces::AirMedium;
                state.segPayload = 0;
                return true;
            }

            double t = cell.exitT;
            WalkCell next = state.locateCell(state.point(0, t), state.point(1, t),
                                             state.point(2, t), axis, face, sign);
            next.enterT = t;
            computeExit(state.origin, state.dir, next);

            if (next.medium == state.segMedium) {
                cell = next;
                continue;
            }

            event.t = t;
            event.segmentStartT = state.segStartT;
            event.segmentLength = state.speed*(t - state.segStartT);
            fillPosition(state, t, event);
            event.mediumBefore = state.segMedium;
            event.mediumAfter = next.medium;
            event.payload = next.payload;
            event.normal = Vec3(0.0f, 0.0f, 0.0f);
            event.normal.a[axis] = float(-sign);
            event.escapedRoot = false;

            segment = makeSegment(state.segMedium, state.segPayload,
                                  state.segStartT, t, state.speed);
            segment.endsAtInterface = true;

            state.lastFaceAxis = axis;
            state.lastFaceCoord = face;
            state.lastTravelSign = sign;
            state.lastMediumBefore = state.segMedium;
            state.lastMediumAfter = next.medium;

            cell = next;
            state.segStartT = t;
            state.segMedium = next.medium;
            state.segPayload = next.payload;
            return true;
        }

        finishTerminal(state, segment, false, state.tMax);
        return false;
    }
}

void initializeStart(VoxelInterfaceCursorState &state) {
    double tEnter, tExit;
    bool hit = rootInterval(state.origin, state.dir, tEnter, tExit);
    double ts = state.tMin;

    double px = state.point(0, ts);
    double py = state.point(1, ts);
    double pz = state.point(2, ts);
    bool strictlyInside = hit && ts > tEnter && ts < tExit &&
                          px > 0.0 && px < 1.0 &&
                          py > 0.0 && py < 1.0 &&
                          pz > 0.0 && pz < 1.0;

    if (strictlyInside) {
        state.cell = state.locateCell(px, py, pz, -1, 0.0, 0);
        state.cell.enterT = ts;
        computeExit(state.origin, state.dir, state.cell);
        state.outsideHasBackground = false;
    } else {
        state.cell = WalkCell();
        state.cell.kind = WalkCell::Outside;
        state.cell.enterT = ts;
        state.cell.exitT = std::numeric_limits<double>::infinity();
        state.outsideHasBackground = false;

        if (hit && tEnter > ts && tEnter <= state.tMax) {
            /* The ray will enter the root later within the window. */
            double best = -std::numeric_limits<double>::infinity();
            int axis = -1;
            int sign = 0;
            for (int a = 0; a < 3; ++a) {
                double d = state.dir[a];
                if (d == 0.0)
                    continue;
                double t0 = (0.0 - state.origin[a])/d;
                double t1 = (1.0 - state.origin[a])/d;
                double low = std::min(t0, t1);
                if (low > best) {
                    best = low;
                    axis = a;
                    sign = d > 0.0 ? 1 : -1;
                }
            }
            state.cell.exitAxis = axis;
            state.cell.exitSign = sign;
            state.cell.exitT = tEnter;
        } else {
            /* The start is outside the root and the ray does not enter it
             * again.  That is the exterior background only when the ray is
             * heading away from the root and has already left it (tExit <= ts).
             * A ray which starts "outside" but points back towards the root,
             * or is only numerically outside a face, must still be resolved by
             * ordinary traversal so that a later entry is not missed. */
            bool headingAway = hit && tExit <= ts;
            if (headingAway) {
                state.outsideHasBackground = true;
            } else if (hit) {
                /* Give the walker a chance to enter the root: schedule the
                 * clamped entry so advance() reaches it. */
                double enter = std::max(ts, tEnter);
                if (enter <= state.tMax) {
                    double best = -std::numeric_limits<double>::infinity();
                    int axis = -1;
                    int sign = 0;
                    for (int a = 0; a < 3; ++a) {
                        double d = state.dir[a];
                        if (d == 0.0)
                            continue;
                        double t0 = (0.0 - state.origin[a])/d;
                        double t1 = (1.0 - state.origin[a])/d;
                        double low = std::min(t0, t1);
                        if (low > best) {
                            best = low;
                            axis = a;
                            sign = d > 0.0 ? 1 : -1;
                        }
                    }
                    state.cell.exitAxis = axis;
                    state.cell.exitSign = sign;
                    state.cell.exitT = enter;
                }
            }
        }
    }

    state.segMedium = state.cell.kind == WalkCell::Outside
        ? VoxelInterfaces::AirMedium : state.cell.medium;
    state.segPayload = state.cell.kind == WalkCell::Outside ? 0 : state.cell.payload;
    state.segStartT = ts;
}

void prime(VoxelInterfaceCursorState &state) {
    state.finished = false;
    state.hasPending = false;
    state.reachedBackground = false;

    VoxelInterfaces::InterfaceEvent event;
    VoxelInterfaces::SegmentInfo segment;
    bool isEvent = advance(state, event, segment);

    state.firstSegment = segment;
    state.lastSegment = segment;

    if (isEvent) {
        state.hasPending = true;
        state.pending = event;
    } else {
        state.finished = true;
    }
}

} /* namespace */

VoxelInterfaces::Cursor::Cursor(std::unique_ptr<VoxelInterfaceCursorState> state)
: _state(std::move(state))
{
}

VoxelInterfaces::Cursor::Cursor(const Cursor &other)
: _state(new VoxelInterfaceCursorState(*other._state))
{
}

VoxelInterfaces::Cursor &VoxelInterfaces::Cursor::operator=(const Cursor &other) {
    if (this != &other)
        _state.reset(new VoxelInterfaceCursorState(*other._state));
    return *this;
}

VoxelInterfaces::Cursor::Cursor(Cursor &&other)
: _state(std::move(other._state))
{
}

VoxelInterfaces::Cursor &VoxelInterfaces::Cursor::operator=(Cursor &&other) {
    if (this != &other)
        _state = std::move(other._state);
    return *this;
}

VoxelInterfaces::Cursor::~Cursor() {
}

const VoxelInterfaces &VoxelInterfaces::Cursor::resource() const {
    return *_state->resource;
}

const VoxelInterfaces::SegmentInfo &VoxelInterfaces::Cursor::firstSegment() const {
    return _state->firstSegment;
}

bool VoxelInterfaces::Cursor::next(InterfaceEvent &event) {
    VoxelInterfaceCursorState &state = *_state;

    if (state.hasPending) {
        state.hasPending = false;
        event = state.pending;
        state.lastEvent = event;
        state.hasLastEvent = true;
        return true;
    }

    if (state.finished)
        return false;

    VoxelInterfaces::SegmentInfo segment;
    bool isEvent = advance(state, event, segment);
    state.lastSegment = segment;

    if (isEvent) {
        state.lastEvent = event;
        state.hasLastEvent = true;
        return true;
    }

    state.finished = true;
    return false;
}

bool VoxelInterfaces::Cursor::finished() const {
    return _state->finished && !_state->hasPending;
}

bool VoxelInterfaces::Cursor::reachedBackground() const {
    return _state->reachedBackground;
}

Vec3 VoxelInterfaces::Cursor::backgroundDirection() const {
    return _state->backgroundDirection;
}

const VoxelInterfaces::SegmentInfo &VoxelInterfaces::Cursor::lastSegment() const {
    return _state->lastSegment;
}

const VoxelInterfaces::InterfaceEvent &VoxelInterfaces::Cursor::lastEvent() const {
    return _state->lastEvent;
}

bool VoxelInterfaces::Cursor::hasEvent() const {
    return _state->hasLastEvent;
}

uint64 VoxelInterfaces::Cursor::descriptorReads() const {
    return _state->reads;
}

VoxelInterfaces::Cursor VoxelInterfaces::Cursor::branch(Side side,
        const Vec3 &newDirection, double tMax) const {
    const VoxelInterfaceCursorState &state = *_state;

    if (!state.hasLastEvent)
        throw std::logic_error("VoxelInterfaces::Cursor::branch: no interface has been returned");

    if (!isFinite(newDirection.x) || !isFinite(newDirection.y) || !isFinite(newDirection.z))
        invalidArgument("branch direction is not finite");

    double speed = std::sqrt(double(newDirection.x)*newDirection.x +
                             double(newDirection.y)*newDirection.y +
                             double(newDirection.z)*newDirection.z);
    if (!(speed > 0.0) || !isFinite(speed))
        invalidArgument("branch direction is zero or degenerate");
    if (!isFinite(tMax) || !(tMax > 0.0))
        invalidArgument("branch parameter window is invalid");

    int axis = state.lastFaceAxis;
    if (axis < 0 || axis > 2)
        throw std::logic_error("VoxelInterfaces::Cursor::branch: missing interface face");

    int sideSign = side == NearSide ? -state.lastTravelSign : state.lastTravelSign;
    if (double(newDirection.a[axis])*double(sideSign) <= 0.0)
        invalidArgument("branch direction does not point into the requested side");

    std::unique_ptr<VoxelInterfaceCursorState> branchState(new VoxelInterfaceCursorState());
    branchState->resource = state.resource;
    branchState->origin[0] = state.lastEvent.positionX - 1.0;
    branchState->origin[1] = state.lastEvent.positionY - 1.0;
    branchState->origin[2] = state.lastEvent.positionZ - 1.0;
    branchState->dir[0] = newDirection.x;
    branchState->dir[1] = newDirection.y;
    branchState->dir[2] = newDirection.z;
    branchState->speed = speed;
    branchState->tMin = 0.0;
    branchState->tMax = tMax;

    double face = state.lastFaceCoord;
    bool outside = (face == 1.0 && sideSign > 0) || (face == 0.0 && sideSign < 0);

    if (outside) {
        branchState->cell = WalkCell();
        branchState->cell.kind = WalkCell::Outside;
        branchState->cell.exitT = std::numeric_limits<double>::infinity();
        branchState->outsideHasBackground = true;
        branchState->segStartT = 0.0;
        branchState->segMedium = AirMedium;
        branchState->segPayload = 0;
    } else {
        WalkCell start = branchState->locateCell(branchState->origin[0],
                                                 branchState->origin[1],
                                                 branchState->origin[2],
                                                 axis, face, sideSign);
        start.enterT = 0.0;
        computeExit(branchState->origin, branchState->dir, start);
        branchState->cell = start;
        branchState->segStartT = 0.0;
        branchState->segMedium = start.medium;
        branchState->segPayload = start.payload;
    }

    prime(*branchState);
    return Cursor(std::move(branchState));
}

VoxelInterfaces::VoxelInterfaces(const VoxelOctree &tree,
                                 const std::vector<DielectricMaterial> &materials,
                                 const std::unordered_map<uint32, uint32> &payloadMedium)
: _tree(&tree),
  _materials(materials),
  _payloadMedium(payloadMedium),
  _descriptorReads(0)
{
    validateInputs();
    validateTree();
}

void VoxelInterfaces::validateInputs() {
    if (_materials.empty())
        invalidArgument("material table is empty");

    for (size_t i = 0; i < _materials.size(); ++i) {
        const DielectricMaterial &material = _materials[i];
        if (!isFinite(material.ior) || !(material.ior > 0.0f))
            invalidArgument("material index of refraction must be finite and positive");
        if (!isFinite(material.absorption.x) ||
            !isFinite(material.absorption.y) ||
            !isFinite(material.absorption.z) ||
            material.absorption.x < 0.0f ||
            material.absorption.y < 0.0f ||
            material.absorption.z < 0.0f)
            invalidArgument("material absorption must be finite and non-negative");
    }

    if (!isAirMaterial(_materials[AirMedium]))
        invalidArgument("medium 0 must be air (ior 1, absorption 0)");

    for (std::unordered_map<uint32, uint32>::const_iterator it = _payloadMedium.begin();
         it != _payloadMedium.end(); ++it) {
        if (it->second >= _materials.size())
            invalidArgument("payload map refers to an unknown medium");
        if (it->first == 0 && it->second != AirMedium)
            invalidArgument("payload 0 must map to air");
    }
}

void VoxelInterfaces::validateTree() {
    const uint64 count = _tree->descriptorCount();
    const uint32 *words = _tree->descriptors();

    if (count == 0)
        invalidArgument("tree has no descriptors");
    if (!words)
        invalidArgument("tree descriptor array is null");

    struct Entry {
        uint64 index;
        int level;
    };

    std::vector<Entry> stack;
    stack.push_back(Entry{0, 0});

    while (!stack.empty()) {
        Entry entry = stack.back();
        stack.pop_back();

        if (entry.index >= count)
            invalidArgument("descriptor index out of range");
        if (entry.level > MaxTreeDepth)
            invalidArgument("descriptor hierarchy too deep or cyclic");

        uint32 word = words[entry.index];
        uint32 childMask    = (word >> 8) & 0xFFu;
        uint32 internalMask = word & 0xFFu;

        if ((internalMask & ~childMask) != 0)
            invalidArgument("descriptor has internal bits for absent children");

        if ((word & 0x10000u) && childMask == 0)
            invalidArgument("descriptor has a group marker but no children");

        uint64 low = uint64(word >> 18);
        if (word & 0x20000u) {
            if (entry.index + 1 >= count)
                invalidArgument("truncated long child address");
            low = (low << 32) | uint64(words[entry.index + 1]);
        }
        if (childMask != 0 && low == 0)
            invalidArgument("descriptor with children has a zero child offset");

        uint64 base = entry.index + low;
        if (base >= count && childMask != 0)
            invalidArgument("child address out of range");

        for (int octant = 0; octant < 8; ++octant) {
            if (!(childMask & (1u << octant)))
                continue;

            int rank = popcount8(childMask & ((1u << octant) - 1u));
            uint64 slot = base + uint64((word & 0x10000u) ? 2*rank : rank);
            if (slot >= count)
                invalidArgument("child slot index out of range");

            if (internalMask & (1u << octant)) {
                uint64 childIndex = slot;
                if (childIndex <= entry.index)
                    invalidArgument("invalid child descriptor address");
                stack.push_back(Entry{childIndex, entry.level + 1});
            } else {
                uint32 payload = words[slot];
                if (payload != 0 && _payloadMedium.find(payload) == _payloadMedium.end())
                    invalidArgument("tree contains an unmapped non-zero payload");
            }
        }
    }
}

const VoxelOctree &VoxelInterfaces::tree() const {
    return *_tree;
}

const std::vector<DielectricMaterial> &VoxelInterfaces::materials() const {
    return _materials;
}

size_t VoxelInterfaces::mediumCount() const {
    return _materials.size();
}

bool VoxelInterfaces::hasPayload(uint32 payload) const {
    if (payload == 0)
        return true;
    return _payloadMedium.find(payload) != _payloadMedium.end();
}

uint32 VoxelInterfaces::mediumForPayload(uint32 payload) const {
    if (payload == 0)
        return AirMedium;
    std::unordered_map<uint32, uint32>::const_iterator it = _payloadMedium.find(payload);
    if (it == _payloadMedium.end())
        invalidArgument("unmapped non-zero payload");
    return it->second;
}

uint32 VoxelInterfaces::lookupMedium(uint32 payload) const {
    return mediumForPayload(payload);
}

uint64 VoxelInterfaces::descriptorReads() const {
    return _descriptorReads.load(std::memory_order_relaxed);
}

void VoxelInterfaces::noteDescriptorRead() const {
    _descriptorReads.fetch_add(1, std::memory_order_relaxed);
}

VoxelInterfaces::Cursor VoxelInterfaces::begin(const Vec3 &origin,
                                               const Vec3 &direction,
                                               double tMax) const {
    return begin(origin, direction, 0.0, tMax);
}

VoxelInterfaces::Cursor VoxelInterfaces::begin(const Vec3 &origin,
                                               const Vec3 &direction,
                                               double tMin, double tMax) const {
    return begin(Vec3d(origin.x, origin.y, origin.z),
                 Vec3d(direction.x, direction.y, direction.z), tMin, tMax);
}

VoxelInterfaces::Cursor VoxelInterfaces::begin(const Vec3d &origin,
                                               const Vec3d &direction,
                                               double tMax) const {
    return begin(origin, direction, 0.0, tMax);
}

VoxelInterfaces::Cursor VoxelInterfaces::begin(const Vec3d &origin,
                                               const Vec3d &direction,
                                               double tMin, double tMax) const {
    if (!isFinite(origin.x) || !isFinite(origin.y) || !isFinite(origin.z))
        invalidArgument("ray origin is not finite");
    if (!isFinite(direction.x) || !isFinite(direction.y) || !isFinite(direction.z))
        invalidArgument("ray direction is not finite");
    if (!isFinite(tMin) || !isFinite(tMax))
        invalidArgument("parameter window is not finite");
    if (!(tMax > tMin))
        invalidArgument("parameter window is empty");

    double speed = std::sqrt(direction.x*direction.x +
                             direction.y*direction.y +
                             direction.z*direction.z);
    if (!(speed > 0.0) || !isFinite(speed))
        invalidArgument("ray direction is zero or degenerate");

    std::unique_ptr<VoxelInterfaceCursorState> state(new VoxelInterfaceCursorState());
    state->resource = this;
    state->origin[0] = origin.x - 1.0;
    state->origin[1] = origin.y - 1.0;
    state->origin[2] = origin.z - 1.0;
    state->dir[0] = direction.x;
    state->dir[1] = direction.y;
    state->dir[2] = direction.z;
    state->speed = speed;
    state->tMin = tMin;
    state->tMax = tMax;

    initializeStart(*state);
    prime(*state);

    return Cursor(std::move(state));
}
