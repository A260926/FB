/*
    pbrt source code is Copyright(c) 1998-2016
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

#if defined(_MSC_VER)
#define NOMINMAX
#pragma once
#endif

#ifndef PBRT_MEDIA_INSTANCEGRID_H
#define PBRT_MEDIA_INSTANCEGRID_H

// media/instancegrid.h*
#include "medium.h"
#include "transform.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pbrt {

// InstanceGridDensity is the immutable, shareable description of a
// heterogeneous density field.  It owns the voxel array and the mapping from
// the unit density cube to the *prototype* coordinate frame.  It contains no
// world-space information at all, so one instance can be shared by any number
// of placed instances without copying either the density data or the lookup
// metadata.
class InstanceGridDensity {
  public:
    // InstanceGridDensity Public Methods
    static std::shared_ptr<const InstanceGridDensity> Create(
        const Spectrum &sigma_a, const Spectrum &sigma_s, Float g, int nx,
        int ny, int nz, const Transform &densityToPrototype,
        const Float *density, std::string *error);

    // Density in the normalized [0,1]^3 grid parameterization, with the same
    // trilinear interpolation and out-of-bounds behavior as GridDensityMedium.
    Float Density(const Point3f &p) const;
    Float D(const Point3i &p) const;
    // Raw voxel accessor for verification and debugging (bounds-checked).
    Float At(int x, int y, int z) const { return D(Point3i(x, y, z)); }

    const Spectrum &SigmaA() const { return sigma_a; }
    const Spectrum &SigmaS() const { return sigma_s; }
    Float G() const { return g; }
    // Scalar extinction coefficient per *world* unit length.
    Float SigmaT() const { return sigma_t; }
    Float InvMaxDensity() const { return invMaxDensity; }
    const Transform &DensityToPrototype() const {
        return densityToPrototype;
    }

  private:
    // InstanceGridDensity Private Methods
    InstanceGridDensity(const Spectrum &sigma_a, const Spectrum &sigma_s,
                        Float g, int nx, int ny, int nz,
                        const Transform &densityToPrototype,
                        const Float *density);

    // InstanceGridDensity Private Data
    const Spectrum sigma_a, sigma_s;
    const Float g;
    const int nx, ny, nz;
    const Transform densityToPrototype;
    std::unique_ptr<Float[]> density;
    Float sigma_t;
    Float maxDensity, invMaxDensity;
};

// InstanceGridMedium is a per-placement *view* of a shared InstanceGridDensity.
// A prototype created inside ObjectBegin gets a placeholder view whose
// placement is the identity; every ObjectInstance creates a fresh view with
// that instance's prototype-to-world transform.  The views are small (two
// transforms and a shared_ptr) and never duplicate voxel data.
class InstanceGridMedium : public Medium {
  public:
    // InstanceGridMedium Public Methods
    static std::shared_ptr<InstanceGridMedium> CreatePrototype(
        std::shared_ptr<const InstanceGridDensity> grid,
        const std::string &prototypeName);
    static std::shared_ptr<InstanceGridMedium> CreatePlacement(
        std::shared_ptr<const InstanceGridDensity> grid,
        const Transform &prototypeToWorld, const std::string &prototypeName,
        std::string *error);

    bool IsPrototype() const { return isPrototype; }
    const std::string &PrototypeName() const { return prototypeName; }
    const InstanceGridDensity *Grid() const { return grid.get(); }
    std::shared_ptr<const InstanceGridDensity> GridShared() const {
        return grid;
    }
    const Transform &DensityToWorld() const { return densityToWorld; }

    // Density evaluated at a world-space point (used by tests and debugging).
    Float DensityWorld(const Point3f &pWorld) const {
        return grid->Density(worldToDensity(pWorld));
    }

    Spectrum Tr(const Ray &ray, Sampler &sampler) const;
    Spectrum Sample(const Ray &ray, Sampler &sampler, MemoryArena &arena,
                    MediumInteraction *mi) const;

  private:
    // InstanceGridMedium Private Methods
    InstanceGridMedium(std::shared_ptr<const InstanceGridDensity> grid,
                       const Transform &densityToWorld, bool isPrototype,
                       const std::string &prototypeName);
    bool OverlapWorldRay(const Ray &ray, Ray *rDistance, Ray *rDensity,
                         Float *t0, Float *t1) const;

    // InstanceGridMedium Private Data
    std::shared_ptr<const InstanceGridDensity> grid;
    const Transform densityToWorld, worldToDensity;
    const bool isPrototype;
    const std::string prototypeName;
};

// Builds an instancegrid medium from the MakeNamedMedium parameter set,
// mirroring the heterogeneous medium's parameters (density, nx/ny/nz,
// p0/p1, sigma_a, sigma_s, g, preset and scale).  definitionTransform is the
// transform that was current where MakeNamedMedium appeared; it maps the
// medium's local frame (the density box lives there) to prototype coordinates.
std::shared_ptr<InstanceGridMedium> CreateInstanceGridMedium(
    const ParamSet &params, const Transform &definitionTransform,
    const std::string &prototypeName, std::string *error);

// Validates that a placement transform is finite, affine and invertible.
bool ValidateInstanceTransform(const Transform &prototypeToWorld,
                               std::string *error);

// Creates one medium view per prototype medium for a single instance
// placement.  All views share the prototype's density resources.
std::vector<std::shared_ptr<InstanceGridMedium>> CreateInstancePlacementViews(
    const std::vector<std::shared_ptr<InstanceGridMedium>> &prototypeMedia,
    const Transform &prototypeToWorld, std::string *error);

// Creates the per-instance MediumRemap that rewrites the prototype's medium
// pointers to the given placement's medium views.  Returns null on an
// invalid placement transform (and sets *error).
std::shared_ptr<const MediumRemap> CreateInstanceMediumRemap(
    const std::vector<std::shared_ptr<InstanceGridMedium>> &prototypeMedia,
    const Transform &prototypeToWorld, const std::string &prototypeName,
    std::string *error);

// Same, but adopting placement views that the caller has already created
// (useful when the caller also needs the view identities, as tests do).
std::shared_ptr<const MediumRemap> CreateInstanceMediumRemap(
    const std::vector<std::shared_ptr<InstanceGridMedium>> &prototypeMedia,
    const std::vector<std::shared_ptr<InstanceGridMedium>> &placementViews);

}  // namespace pbrt

#endif  // PBRT_MEDIA_INSTANCEGRID_H
