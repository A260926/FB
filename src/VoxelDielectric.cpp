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

#include "VoxelDielectric.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace {

/* A large but finite parameter window used for internal branches.  All
 * directions handed to the traversal are unit length, so this comfortably
 * covers any number of traversals of the [1,2]^3 root cube. */
const double TraceWindow = 1.0e6;

inline Vec3 attenuation(const VoxelInterfaces &interfaces, uint32 medium, double length) {
    if (!(length > 0.0))
        return Vec3(1.0f);
    const DielectricMaterial &material = interfaces.materials()[medium];
    return Vec3(
        float(std::exp(-double(material.absorption.x)*length)),
        float(std::exp(-double(material.absorption.y)*length)),
        float(std::exp(-double(material.absorption.z)*length))
    );
}

} /* namespace */

VoxelDielectric::VoxelDielectric(const VoxelInterfaces &interfaces)
: _interfaces(&interfaces)
{
}

const VoxelInterfaces &VoxelDielectric::interfaces() const {
    return *_interfaces;
}

DielectricRefraction VoxelDielectric::refract(const Vec3 &incident, const Vec3 &normal,
                                              double etaIncident, double etaTransmitted) {
    DielectricRefraction result;

    Vec3 d = incident.normalize();
    Vec3 n = normal.normalize();

    double cosI = -double(d.x)*double(n.x) -
                  double(d.y)*double(n.y) -
                  double(d.z)*double(n.z);
    if (cosI < 0.0) {
        /* The normal was supplied pointing the other way; flip it so it faces
         * the incident medium. */
        n = -n;
        cosI = -cosI;
    }
    if (cosI > 1.0)
        cosI = 1.0;
    if (cosI < 0.0)
        cosI = 0.0;

    result.cosIncident = cosI;
    result.etaIncident = etaIncident;
    result.etaTransmitted = etaTransmitted;

    result.reflectDirection = d + n*(2.0*cosI);

    double eta = etaIncident/etaTransmitted;
    double sinT2 = eta*eta*(1.0 - cosI*cosI);

    if (sinT2 > 1.0) {
        result.totalInternalReflection = true;
        result.cosTransmitted = 0.0;
        result.reflectance = 1.0;
        result.transmittance = 0.0;
        result.transmitDirection = Vec3(0.0f, 0.0f, 0.0f);
        return result;
    }

    double cosT = std::sqrt(std::max(0.0, 1.0 - sinT2));
    result.cosTransmitted = cosT;

    /* Unpolarized Fresnel. */
    double rs = (etaIncident*cosI - etaTransmitted*cosT) /
                (etaIncident*cosI + etaTransmitted*cosT);
    double rp = (etaTransmitted*cosI - etaIncident*cosT) /
                (etaTransmitted*cosI + etaIncident*cosT);
    double F = 0.5*(rs*rs + rp*rp);
    if (F < 0.0) F = 0.0;
    if (F > 1.0) F = 1.0;

    result.reflectance = F;
    result.transmittance = (1.0 - F)*eta*eta;

    Vec3 transmit = d*float(eta) + n*float(eta*cosI - cosT);
    result.transmitDirection = transmit.normalize();

    return result;
}

struct VoxelDielectric::TraceStats {
    uint64 interfaces;
    uint64 reads;
    bool truncated;

    TraceStats() : interfaces(0), reads(0), truncated(false) {}
};

Vec3 VoxelDielectric::traceNode(const VoxelInterfaces::Cursor &inputCursor,
                                const Vec3 &direction, unsigned depth,
                                unsigned budget,
                                const DielectricBackground &background,
                                const DielectricEventCallback &onEvent,
                                TraceStats &stats) const {
    (void)depth;

    VoxelInterfaces::Cursor cursor = inputCursor;
    Vec3 throughput(1.0f);
    Vec3 radiance(0.0f);

    for (;;) {
        VoxelInterfaces::InterfaceEvent event;
        if (!cursor.next(event)) {
            const VoxelInterfaces::SegmentInfo &segment = cursor.lastSegment();
            throughput *= attenuation(*_interfaces, segment.medium, segment.length);
            if (cursor.reachedBackground()) {
                Vec3 bg = background(cursor.backgroundDirection());
                radiance += throughput*bg;
            }
            break;
        }

        throughput *= attenuation(*_interfaces, event.mediumBefore, event.segmentLength);

        if (budget == 0) {
            /* An interface remains but the event budget is exhausted: this
             * branch contributes nothing. */
            stats.truncated = true;
            break;
        }

        --budget;
        ++stats.interfaces;

        double etaIncident = double(_interfaces->materials()[event.mediumBefore].ior);
        double etaTransmitted = double(_interfaces->materials()[event.mediumAfter].ior);

        DielectricRefraction optics = refract(direction, event.normal,
                                              etaIncident, etaTransmitted);

        if (onEvent) {
            DielectricEvent detail;
            detail.segmentLength = event.segmentLength;
            detail.mediumBefore = event.mediumBefore;
            detail.mediumAfter = event.mediumAfter;
            detail.normal = event.normal;
            detail.incidentDirection = direction;
            detail.reflectDirection = optics.reflectDirection;
            detail.transmitDirection = optics.transmitDirection;
            detail.reflectance = optics.reflectance;
            detail.transmittance = optics.transmittance;
            detail.totalInternalReflection = optics.totalInternalReflection;
            detail.segmentAttenuation = attenuation(*_interfaces, event.mediumBefore,
                                                    event.segmentLength);
            detail.budgetRemaining = int(budget);
            detail.overBudget = false;
            onEvent(detail);
        }

        /* Reflection stays in the incident medium. */
        {
            VoxelInterfaces::Cursor reflected =
                cursor.branch(VoxelInterfaces::NearSide, optics.reflectDirection, TraceWindow);
            Vec3 child = traceNode(reflected, optics.reflectDirection, depth + 1,
                                   budget, background, onEvent, stats);
            radiance += throughput*float(optics.reflectance)*child;
        }

        if (!optics.totalInternalReflection) {
            VoxelInterfaces::Cursor transmitted =
                cursor.branch(VoxelInterfaces::FarSide, optics.transmitDirection, TraceWindow);
            Vec3 child = traceNode(transmitted, optics.transmitDirection, depth + 1,
                                   budget, background, onEvent, stats);
            radiance += throughput*float(optics.transmittance)*child;
        }

        break;
    }

    stats.reads += cursor.descriptorReads();
    return radiance;
}

DielectricTraceResult VoxelDielectric::trace(const Vec3 &origin, const Vec3 &direction,
                                             const DielectricBackground &background,
                                             unsigned maxInterfaces) const {
    return trace(origin, direction, background, maxInterfaces, DielectricEventCallback());
}

DielectricTraceResult VoxelDielectric::trace(const Vec3 &origin, const Vec3 &direction,
                                             const DielectricBackground &background,
                                             unsigned maxInterfaces,
                                             const DielectricEventCallback &onEvent) const {
    return trace(Vec3d(origin.x, origin.y, origin.z),
                 Vec3d(direction.x, direction.y, direction.z),
                 background, maxInterfaces, onEvent);
}

DielectricTraceResult VoxelDielectric::trace(const Vec3d &origin, const Vec3d &direction,
                                             const DielectricBackground &background,
                                             unsigned maxInterfaces) const {
    return trace(origin, direction, background, maxInterfaces, DielectricEventCallback());
}

DielectricTraceResult VoxelDielectric::trace(const Vec3d &origin, const Vec3d &direction,
                                             const DielectricBackground &background,
                                             unsigned maxInterfaces,
                                             const DielectricEventCallback &onEvent) const {
    double speed = std::sqrt(direction.x*direction.x +
                             direction.y*direction.y +
                             direction.z*direction.z);
    if (!(speed > 0.0) || !std::isfinite(speed))
        throw std::invalid_argument("VoxelDielectric: ray direction is zero or degenerate");

    Vec3d unitDirection(direction.x/speed, direction.y/speed, direction.z/speed);

    VoxelInterfaces::Cursor start =
        _interfaces->begin(origin, unitDirection, TraceWindow);

    TraceStats stats;
    DielectricTraceResult result;
    result.radiance = traceNode(start, Vec3(float(unitDirection.x),
                                            float(unitDirection.y),
                                            float(unitDirection.z)),
                                0, maxInterfaces, background, onEvent, stats);
    result.interfaces = stats.interfaces;
    result.descriptorReads = stats.reads;
    result.truncated = stats.truncated;
    return result;
}
