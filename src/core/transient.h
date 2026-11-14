
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

#ifndef PBRT_CORE_TRANSIENT_H
#define PBRT_CORE_TRANSIENT_H

// core/transient.h*
#include "pbrt.h"
#include "spectrum.h"
#include "film.h"
#include <array>
#include <vector>

namespace pbrt {

class Filter;
class FilmTile;

// Exact speed of light in vacuum, in metres per second.
static const double SpeedOfLight = 299792458.0;

class Light;
class Medium;
class HomogeneousMedium;
class RayDifferential;
class Scene;
class Sampler;
class MemoryArena;
class ParamSet;

// TemporalPulse Declarations
//
// Piecewise-linear emission pulse h(t).  Times are stored in seconds; the
// public scene parameters use nanoseconds.  The pulse is normalized so that
// its integral over the whole support is one: scaling the shape of the
// pulse never changes the total emitted energy, and the light's I / L keeps
// its original meaning as the pulse-integrated intensity / radiance.
class TemporalPulse {
  public:
    // TemporalPulse Public Methods
    TemporalPulse(std::vector<double> timesSeconds,
                  std::vector<double> values);

    // Integral of the (normalized) pulse over [beginSeconds, endSeconds).
    // The interval mass is computed exactly with piecewise linear
    // integration; no time sampling or numerical grids are used.
    double Integral(double beginSeconds, double endSeconds) const;

    // Integral of the pulse after a translation of _delaySeconds_: the
    // mass of h(t - delaySeconds) on [beginSeconds, endSeconds), i.e. the
    // mass of h over [beginSeconds - delaySeconds, endSeconds - delaySeconds).
    double ShiftedIntegral(double beginSeconds, double endSeconds,
                           double delaySeconds) const {
        return Integral(beginSeconds - delaySeconds,
                        endSeconds - delaySeconds);
    }
    double SupportBegin() const { return times.front(); }
    double SupportEnd() const { return times.back(); }
    bool IsNormalized() const { return integral == 1.0; }

  private:
    // TemporalPulse Private Data
    std::vector<double> times;
    std::vector<double> vals;
    double integral;
};

// TimedContribution Declarations
//
// One spatial estimator contribution produced by a camera path, tagged with
// the propagation time of the light along the physical path from the camera
// to the emitting endpoint.  _value_ is the full-time-integrated
// contribution of this estimator (the same quantity volpath would add to
// its radiance estimate); the temporal shape is applied later by the film.
struct TimedContribution {
    TimedContribution() : value(0.f), flightTime(0.0), emitter(nullptr) {}
    TimedContribution(const Spectrum &value, double flightTime,
                      const Light *emitter)
        : value(value), flightTime(flightTime), emitter(emitter) {}
    Spectrum value;
    double flightTime;
    const Light *emitter;
};

// Trace a camera path with the volpath transport rules and return every
// direct-lighting and emitter-hit estimator contribution together with the
// physical propagation time of its endpoint.  A single camera sample may
// produce multiple contributions with different flight times and emitters.
// _metersPerUnit_ converts world-space distances to metres; the flight time
// is accumulated in double precision seconds.
std::vector<TimedContribution> TraceTransient(
    const RayDifferential &ray, const Scene &scene, Sampler &sampler,
    MemoryArena &arena, int maxDepth, Float rrThreshold,
    double metersPerUnit);

// Transient medium helpers.  Vacuum (a ray with no medium) has group index
// one.  Only HomogeneousMedium instances carry an explicit group index.
double MediumGroupIndex(const Medium *medium);

// Build the normalized emission pulse described by a light's ParamSet.
// Returns the default triangular pulse when neither "pulsetimes" nor
// "pulsevalues" is present, or null (after an Error) when the arrays are
// incomplete or invalid.  Scene-construction code must stop on null.
std::shared_ptr<TemporalPulse> CreateTemporalPulse(const ParamSet &params);

// Construction-time rejection flag.  Transient-only scene objects (pulses,
// the transient film, extended media) set this when their parameters are
// invalid; the scene creation layer checks it before rendering so that a
// rejected scene never produces partial output.
void MarkTransientConstructionError();
bool ConsumeTransientConstructionError();
// Non-destructive check, used to suppress a redundant generic error after a
// specific construction Error() has already been reported.
bool TransientConstructionErrorOccurred();

// Validate the "transientvolpath" integrator's scene assumptions.  Every
// rejection is reported with Error() and the caller must not render (and
// must not write partial output) when this returns false.
bool ValidateTransientScene(const Scene &scene, bool sampledSpectrum,
                            std::string *failureReason = nullptr);

// TransientFilm Declarations
//
// Time-resolved film.  It keeps the steady Film's spatial filtering, crop
// window, scale, tile merging and thread-safety, but additionally bins
// energy into _bins_ time intervals.  Bin k covers
// [tstart + k*binwidth, tstart + (k+1)*binwidth), with tstart/binwidth in
// nanoseconds.  The energy a TimedContribution puts into a bin is
// value * (pulse mass on the bin interval shifted by the flight time); the
// bin width is not divided out.
class TransientFilm : public Film {
  public:
    TransientFilm(const Point2i &resolution, const Bounds2f &cropWindow,
                  std::unique_ptr<Filter> filter, Float diagonal,
                  const std::string &filename, Float scale, int bins,
                  Float tStartNs, Float binWidthNs);

    std::unique_ptr<FilmTile> GetFilmTile(
        const Bounds2i &sampleBounds) override;
    void MergeFilmTile(std::unique_ptr<FilmTile> tile) override;
    void WriteImage(Float splatScale = 1) override;
    void Clear() override;
    std::vector<std::array<double, 3>> GetTransientRGB(
        const Point2i &p) const override;
    bool IsTransient() const override { return true; }

    int Bins() const { return bins; }
    Float TStartNs() const { return tStartNs; }
    Float BinWidthNs() const { return binWidthNs; }

  private:
    const int bins;
    const Float tStartNs;
    const Float binWidthNs;
    // Flat per-pixel XYZ energy, indexed [pixelOffset * 3 * bins + b*3 + c].
    std::unique_ptr<double[]> binXYZ;
    // Single spatial filter denominator per pixel.
    std::unique_ptr<double[]> filterWeights;

    int PixelOffset(const Point2i &p) const {
        int width = croppedPixelBounds.pMax.x - croppedPixelBounds.pMin.x;
        return (p.x - croppedPixelBounds.pMin.x) +
               (p.y - croppedPixelBounds.pMin.y) * width;
    }

    friend class TransientFilmTile;
};

Film *CreateTransientFilm(const ParamSet &params,
                          std::unique_ptr<Filter> filter);

}  // namespace pbrt

#endif  // PBRT_CORE_TRANSIENT_H
