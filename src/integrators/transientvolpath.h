
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

#ifndef PBRT_INTEGRATORS_TRANSIENTVOLPATH_H
#define PBRT_INTEGRATORS_TRANSIENTVOLPATH_H

// integrators/transientvolpath.h*
#include "pbrt.h"
#include "integrator.h"

namespace pbrt {

// TransientVolPathIntegrator Declarations
//
// Volumetric path tracer that records per-contribution flight times instead
// of folding everything into a single steady-state Spectrum.  The transport
// rules (max depth, Russian roulette, throughput, medium sampling, uniform
// light selection and MIS) match VolPathIntegrator; all temporal logic lives
// in core/transient.cpp and the transient Film.
class TransientVolPathIntegrator : public SamplerIntegrator {
  public:
    TransientVolPathIntegrator(int maxDepth,
                               std::shared_ptr<const Camera> camera,
                               std::shared_ptr<Sampler> sampler,
                               const Bounds2i &pixelBounds, Float rrThreshold,
                               double metersPerUnit, bool constructionValid)
        : SamplerIntegrator(camera, sampler, pixelBounds),
          maxDepth(maxDepth),
          rrThreshold(rrThreshold),
          metersPerUnit(metersPerUnit),
          constructionValid(constructionValid) {}

    void Preprocess(const Scene &scene, Sampler &sampler);
    // Steady-style interface: sums every timed contribution of one camera
    // sample.  Render() uses TraceTransient() directly to retain timing.
    Spectrum Li(const RayDifferential &ray, const Scene &scene,
                Sampler &sampler, MemoryArena &arena, int depth) const;
    void Render(const Scene &scene);

    double MetersPerUnit() const { return metersPerUnit; }

  private:
    const int maxDepth;
    const Float rrThreshold;
    const double metersPerUnit;
    bool constructionValid;
};

TransientVolPathIntegrator *CreateTransientVolPathIntegrator(
    const ParamSet &params, std::shared_ptr<Sampler> sampler,
    std::shared_ptr<const Camera> camera, bool constructionValid);

}  // namespace pbrt

#endif  // PBRT_INTEGRATORS_TRANSIENTVOLPATH_H
