
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

// integrators/transientvolpath.cpp*
#include "integrators/transientvolpath.h"

#include "camera.h"
#include "film.h"
#include "interaction.h"
#include "parallel.h"
#include "paramset.h"
#include "progressreporter.h"
#include "scene.h"
#include "stats.h"
#include "transient.h"

namespace pbrt {

STAT_COUNTER("Transient integrator/Camera rays traced",
             transientCameraRays);

void TransientVolPathIntegrator::Preprocess(const Scene &scene,
                                            Sampler &sampler) {
    // Backstop for programmatically constructed scenes: the scene creation
    // layer already rejects everything it can see; this additionally checks
    // the light types against the transient restrictions.
    std::string reason;
    if (!ValidateTransientScene(scene, false, &reason))
        constructionValid = false;
}

Spectrum TransientVolPathIntegrator::Li(
    const RayDifferential &ray, const Scene &scene, Sampler &sampler,
    MemoryArena &arena, int /*depth*/) const {
    // Steady-state view of the transient tracer: summing all timed
    // contributions reproduces the volpath radiance estimate.  Render()
    // calls TraceTransient() itself so the per-contribution timing is kept.
    std::vector<TimedContribution> contributions = TraceTransient(
        ray, scene, sampler, arena, maxDepth, rrThreshold, metersPerUnit);
    Spectrum total(0.f);
    for (const TimedContribution &c : contributions) total += c.value;
    return total;
}

void TransientVolPathIntegrator::Render(const Scene &scene) {
    if (!constructionValid) {
        // Construction already reported the reason with Error(); never start
        // rendering or write partial output.
        Error("transientvolpath: rendering aborted because the scene failed "
              "transient validation.");
        return;
    }
    Preprocess(scene, GetSampler());
    if (!constructionValid) return;

    Film *film = camera->film;
    if (!film->IsTransient()) {
        Error("transientvolpath requires Film \"transient\"; a non-transient "
              "film cannot store timed contributions.");
        return;
    }

    // Render image tiles in parallel, mirroring SamplerIntegrator::Render().
    Bounds2i sampleBounds = film->GetSampleBounds();
    Vector2i sampleExtent = sampleBounds.Diagonal();
    const int tileSize = 16;
    Point2i nTiles((sampleExtent.x + tileSize - 1) / tileSize,
                   (sampleExtent.y + tileSize - 1) / tileSize);
    ProgressReporter reporter(nTiles.x * nTiles.y, "Rendering");
    {
        ParallelFor2D(
            [&](Point2i tile) {
                MemoryArena arena;
                int seed = tile.y * nTiles.x + tile.x;
                std::unique_ptr<Sampler> tileSampler =
                    GetSampler().Clone(seed);

                int x0 = sampleBounds.pMin.x + tile.x * tileSize;
                int x1 = std::min(x0 + tileSize, sampleBounds.pMax.x);
                int y0 = sampleBounds.pMin.y + tile.y * tileSize;
                int y1 = std::min(y0 + tileSize, sampleBounds.pMax.y);
                Bounds2i tileBounds(Point2i(x0, y0), Point2i(x1, y1));

                std::unique_ptr<FilmTile> filmTile =
                    film->GetFilmTile(tileBounds);

                for (Point2i pixel : tileBounds) {
                    tileSampler->StartPixel(pixel);
                    // Matches SamplerIntegrator's pixel-bounds culling; the
                    // StartPixel() call is kept first for RNG consistency.
                    if (!InsideExclusive(pixel, GetPixelBounds())) continue;
                    do {
                        CameraSample cameraSample =
                            tileSampler->GetCameraSample(pixel);
                        RayDifferential ray;
                        Float rayWeight =
                            camera->GenerateRayDifferential(cameraSample,
                                                            &ray);
                        ray.ScaleDifferentials(
                            1 / std::sqrt(
                                    (Float)tileSampler->samplesPerPixel));
                        ++transientCameraRays;

                        std::vector<TimedContribution> contributions;
                        if (rayWeight > 0) {
                            contributions = TraceTransient(
                                ray, scene, *tileSampler, arena, maxDepth,
                                rrThreshold, metersPerUnit);
                            // Guard against non-finite values the same way
                            // the steady integrator does.
                            for (TimedContribution &c : contributions)
                                if (c.value.HasNaNs() ||
                                    std::isinf(c.value.y()) ||
                                    c.value.y() < 0.f)
                                    c.value = Spectrum(0.f);
                        }

                        // The spatial filter denominator is updated exactly
                        // once per camera sample inside the tile, including
                        // zero-weight and fully out-of-window samples.
                        filmTile->AddTransientSample(
                            cameraSample.pFilm, contributions, rayWeight);
                        arena.Reset();
                    } while (tileSampler->StartNextSample());
                }
                film->MergeFilmTile(std::move(filmTile));
                reporter.Update();
            },
            nTiles);
        reporter.Done();
    }
    film->WriteImage();
}

TransientVolPathIntegrator *CreateTransientVolPathIntegrator(
    const ParamSet &params, std::shared_ptr<Sampler> sampler,
    std::shared_ptr<const Camera> camera, bool constructionValid) {
    int maxDepth = params.FindOneInt("maxdepth", 5);
    int np;
    const int *pb = params.FindInt("pixelbounds", &np);
    Bounds2i pixelBounds = camera->film->GetSampleBounds();
    if (pb) {
        if (np != 4) {
            Error("Expected four values for \"pixelbounds\" parameter. Got %d.",
                  np);
            MarkTransientConstructionError();
        } else {
            pixelBounds = Intersect(
                pixelBounds,
                Bounds2i{{pb[0], pb[2]}, {pb[1], pb[3]}});
            if (pixelBounds.Area() == 0) {
                Error("Degenerate \"pixelbounds\" specified.");
                MarkTransientConstructionError();
            }
        }
    }
    Float rrThreshold = params.FindOneFloat("rrthreshold", 1.);
    double metersPerUnit =
        double(params.FindOneFloat("metersperunit", 1.f));
    if (!std::isfinite(metersPerUnit) || metersPerUnit <= 0.0) {
        Error("transientvolpath \"metersperunit\" must be finite and greater "
              "than zero; got %g.", metersPerUnit);
        MarkTransientConstructionError();
        constructionValid = false;
    }
    return new TransientVolPathIntegrator(maxDepth, camera, sampler,
                                          pixelBounds, rrThreshold,
                                          metersPerUnit, constructionValid);
}

}  // namespace pbrt
