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

#ifndef VOXELDIELECTRIC_HPP_
#define VOXELDIELECTRIC_HPP_

#include "VoxelInterfaces.hpp"
#include "math/Vec3.hpp"
#include "IntTypes.hpp"

#include <functional>

/*
 * Unpolarized Fresnel / Snell result for one interface crossing.
 * incident and normal are unit vectors; normal points towards the incident
 * medium.  reflectance is the unpolarized Fresnel coefficient F, transmittance
 * is (1-F)*(etaIncident/etaTransmitted)^2.  On total internal reflection
 * reflectance is 1, transmittance 0 and transmitDirection is zero.
 */
struct DielectricRefraction {
    double cosIncident;
    double cosTransmitted;
    double reflectance;
    double transmittance;
    double etaIncident;
    double etaTransmitted;
    Vec3 reflectDirection;
    Vec3 transmitDirection;
    bool totalInternalReflection;

    DielectricRefraction()
    : cosIncident(0.0), cosTransmitted(0.0), reflectance(0.0), transmittance(0.0),
      etaIncident(1.0), etaTransmitted(1.0), reflectDirection(), transmitDirection(),
      totalInternalReflection(false) {}
};

/*
 * One interface event observed during transport.  It exposes the traversal
 * quantities (preceding segment length, both medium numbers, the axial normal)
 * and the optical quantities (Fresnel branch directions and weights) so that
 * traversal and optics can be checked independently.
 */
struct DielectricEvent {
    double segmentLength;      /* world length of the segment before the event */
    uint32 mediumBefore;
    uint32 mediumAfter;
    Vec3 normal;               /* unit axial normal towards the incident medium */
    Vec3 incidentDirection;    /* unit */
    Vec3 reflectDirection;     /* unit */
    Vec3 transmitDirection;    /* unit, zero on total internal reflection */
    double reflectance;        /* F */
    double transmittance;      /* (1-F)*(eta_i/eta_t)^2 */
    bool totalInternalReflection;
    Vec3 segmentAttenuation;   /* exp(-absorption * segmentLength) */
    int budgetRemaining;       /* interface budget left after this event */
    bool overBudget;           /* true when this event exhausted the budget */

    DielectricEvent()
    : segmentLength(0.0), mediumBefore(0), mediumAfter(0), normal(),
      incidentDirection(), reflectDirection(), transmitDirection(),
      reflectance(0.0), transmittance(0.0), totalInternalReflection(false),
      segmentAttenuation(1.0f), budgetRemaining(0), overBudget(false) {}
};

struct DielectricTraceResult {
    Vec3 radiance;
    uint64 interfaces;      /* number of real medium changes traversed */
    uint64 descriptorReads; /* cumulative octree words read by this trace */
    bool truncated;         /* some branch was cut by the interface budget */

    DielectricTraceResult()
    : radiance(0.0f), interfaces(0), descriptorReads(0), truncated(false) {}
};

/* Optional observer called synchronously for every interface event. */
typedef std::function<void(const DielectricEvent &)> DielectricEventCallback;

/* Direction-only background: the argument is a unit propagation direction. */
typedef std::function<Vec3(const Vec3 &)> DielectricBackground;

/*
 * Linear RGB dielectric transport over a VoxelInterfaces resource.
 *
 * Each real medium-number change splits into an exact reflection branch and
 * (unless total internal reflection occurs) an exact transmission branch.
 * Each segment is attenuated per channel by exp(-absorption * worldLength);
 * there is no volume scattering and no stochastic sampling.
 *
 * The background function is evaluated only when a branch truly leaves the
 * root cube while propagating outwards.  Empty cavities inside the root keep
 * searching for the next medium, and rays that start outside are not resolved
 * to the background before they have had a chance to enter the root.
 *
 * maxInterfaces counts only events where the medium number actually changes;
 * same-medium seams never consume budget.  A branch that reaches the budget
 * while more interfaces remain contributes zero; a branch with no further
 * interface still evaluates the background.
 */
class VoxelDielectric {
public:
    explicit VoxelDielectric(const VoxelInterfaces &interfaces);

    const VoxelInterfaces &interfaces() const;

    /* Trace without exposing individual events. */
    DielectricTraceResult trace(const Vec3 &origin, const Vec3 &direction,
                                const DielectricBackground &background,
                                unsigned maxInterfaces) const;

    /* Trace while reporting every interface event to onEvent. */
    DielectricTraceResult trace(const Vec3 &origin, const Vec3 &direction,
                                const DielectricBackground &background,
                                unsigned maxInterfaces,
                                const DielectricEventCallback &onEvent) const;

    /*
     * Double-precision entry points.  The direction background still receives a
     * float Vec3 unit direction; use these when the scene contains layers with
     * thickness around or below 1e-6 in the [1,2]^3 root frame.
     */
    DielectricTraceResult trace(const Vec3d &origin, const Vec3d &direction,
                                const DielectricBackground &background,
                                unsigned maxInterfaces) const;
    DielectricTraceResult trace(const Vec3d &origin, const Vec3d &direction,
                                const DielectricBackground &background,
                                unsigned maxInterfaces,
                                const DielectricEventCallback &onEvent) const;

    /* Exact unpolarized Fresnel / Snell for a unit incident direction and an
     * axial unit normal pointing towards the incident medium. */
    static DielectricRefraction refract(const Vec3 &incident, const Vec3 &normal,
                                        double etaIncident, double etaTransmitted);

private:
    struct TraceStats;

    Vec3 traceNode(const VoxelInterfaces::Cursor &cursor, const Vec3 &direction,
                   unsigned depth, unsigned maxInterfaces,
                   const DielectricBackground &background,
                   const DielectricEventCallback &onEvent, TraceStats &stats) const;

    const VoxelInterfaces *_interfaces;
};

#endif /* VOXELDIELECTRIC_HPP_ */
