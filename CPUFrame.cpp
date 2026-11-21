/*
 * Cloud of Voxels (CoV) project
 * Author: m-c/d, mcidclan
 * Creation Date: 2025
 *
 * GL-free, multi viewport batched frame renderer.
 */

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "headers/CPUFrame.h"

CPUFrameOptions cpuFrameDefaultOptions()
{
    CPUFrameOptions options;
    options.workers = 1;
    options.tileRows = 8;
    // Baseline geometry: opaque, first hit only, no scan patch.
    options.transparency = false;
    options.minVoxelsProximity = 0.0f;
    options.maxVoxelsByRay = 1;
    options.avoidScanGlitches = false;
    return options;
}

namespace
{
    struct BatchParams
    {
        bool transparency;
        float minVoxelsProximity;
        bool avoidScanGlitches;
        unsigned int maxVoxelsByRay;
    };

    struct RenderTask
    {
        size_t viewport;
        int rowBegin;
        int rowEnd;
    };

    /*
    * Start gate: every worker waits until all of them exist, so the batch
    * really runs queries on several threads at the same time instead of
    * letting the first one drain the queue.
    */
    class StartGate
    {
        public:
        explicit StartGate(unsigned int expected)
        : expected(expected), arrived(0), open(false) {}

        void arriveAndWait()
        {
            std::unique_lock<std::mutex> lock(this->mutex);
            this->arrived++;
            if(this->arrived >= this->expected)
            {
                this->open = true;
                this->condition.notify_all();
                return;
            }
            while(!this->open)
            {
                this->condition.wait(lock);
            }
        }

        void openGate()
        {
            std::unique_lock<std::mutex> lock(this->mutex);
            this->open = true;
            this->condition.notify_all();
        }

        private:
        unsigned int expected;
        unsigned int arrived;
        bool open;
        std::mutex mutex;
        std::condition_variable condition;
    };

    bool isFinite(const float v)
    {
        return std::isfinite(v);
    }

    bool isFinite(const Vec3<float>& v)
    {
        return isFinite(v.x) && isFinite(v.y) && isFinite(v.z);
    }

    double norm(const Vec3<float>& v)
    {
        return std::sqrt((double)v.x * v.x + (double)v.y * v.y +
        (double)v.z * v.z);
    }

    double dot(const Vec3<float>& a, const Vec3<float>& b)
    {
        return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z;
    }

    bool isOrthonormal(const Mat3f& basis)
    {
        if(!isFinite(basis.i) || !isFinite(basis.j) || !isFinite(basis.k))
        {
            return false;
        }
        const double tolerance = 1e-3;
        if(std::fabs(norm(basis.i) - 1.0) > tolerance ||
            std::fabs(norm(basis.j) - 1.0) > tolerance ||
            std::fabs(norm(basis.k) - 1.0) > tolerance)
        {
            return false;
        }
        if(std::fabs(dot(basis.i, basis.j)) > tolerance ||
            std::fabs(dot(basis.i, basis.k)) > tolerance ||
            std::fabs(dot(basis.j, basis.k)) > tolerance)
        {
            return false;
        }
        return true;
    }

    /*
    * Validates the whole batch before any work is started, so an invalid
    * batch never produces a partial frame.
    */
    void validate(const std::vector<CPUFrameViewport>& viewports,
    const CPUFrameOptions& options)
    {
        if(options.workers < 1 || options.workers > 32)
        {
            throw std::invalid_argument(
            "CPUFrame: workers must be in [1, 32]");
        }
        if(options.tileRows < 1)
        {
            throw std::invalid_argument(
            "CPUFrame: tileRows must be positive");
        }
        if(options.maxVoxelsByRay < 1)
        {
            throw std::invalid_argument(
            "CPUFrame: maxVoxelsByRay must be positive");
        }
        if(!isFinite(options.minVoxelsProximity))
        {
            throw std::invalid_argument(
            "CPUFrame: minVoxelsProximity must be finite");
        }

        for(size_t i = 0; i < viewports.size(); i++)
        {
            const CPUFrameViewport& viewport = viewports[i];
            if(viewport.octree == NULL)
            {
                throw std::invalid_argument(
                "CPUFrame: viewport has no octree");
            }
            if(viewport.width <= 0 || (viewport.width & 1) != 0 ||
                viewport.height <= 0 || (viewport.height & 1) != 0)
            {
                throw std::invalid_argument(
                "CPUFrame: width and height must be positive even integers");
            }
            if(viewport.pixelStep != 1)
            {
                throw std::invalid_argument(
                "CPUFrame: PIXEL_STEP must be 1");
            }
            if(!isFinite(viewport.position) ||
                !isFinite(viewport.perspectiveFactor) ||
                !isFinite(viewport.zoom))
            {
                throw std::invalid_argument(
                "CPUFrame: non finite viewport parameter");
            }
            if(viewport.perspectiveFactor < 0.0f)
            {
                throw std::invalid_argument(
                "CPUFrame: perspectiveFactor must be non negative");
            }
            if(!(viewport.zoom > 0.0f))
            {
                throw std::invalid_argument(
                "CPUFrame: zoom must be positive");
            }
            if(!isOrthonormal(viewport.basis))
            {
                throw std::invalid_argument(
                "CPUFrame: camera basis must be a non zero orthonormal basis");
            }
            if(!isFinite(viewport.transparentR) ||
                !isFinite(viewport.transparentG) ||
                !isFinite(viewport.transparentB) ||
                !isFinite(viewport.transparentA))
            {
                throw std::invalid_argument(
                "CPUFrame: non finite transparency parameter");
            }
        }
    }

    /*
    * Shared per pixel implementation. It is used by the batch renderer and
    * by the legacy Core pixel path. It writes a copy of the resulting RGBA
    * bytes into rgba.
    */
    void shadePixel(const Octree& octree, const Mat3f& basis,
    const Vec3<float>& position, const int pixelX, const int pixelY,
    const CPUFramePixelConfig& config, std::vector<Octree::RayHit>& hits,
    UC* const rgba)
    {
        Vec3<float> ray = math::vecxscl(basis.i, (float)pixelX);
        math::vecadd(math::vecxscl(basis.j, (float)pixelY), &ray);
        math::vecadd(position, &ray);

        Octree::RayQuery query;
        query.origin = ray;
        query.direction = basis.k;
        if(config.perspectiveFactor > 0.0f)
        {
            query.direction.x += (basis.i.x * (config.zoom * (float)pixelX) +
            basis.j.x * (config.zoom * (float)pixelY)) *
            config.perspectiveFactor;
            query.direction.y += (basis.i.y * (config.zoom * (float)pixelX) +
            basis.j.y * (config.zoom * (float)pixelY)) *
            config.perspectiveFactor;
            query.direction.z += (basis.i.z * (config.zoom * (float)pixelX) +
            basis.j.z * (config.zoom * (float)pixelY)) *
            config.perspectiveFactor;
            math::normalize(&query.direction);
        }
        query.maxDistance = config.maxRayDistance > 0.0f
        ? config.maxRayDistance : octree.getRayLength();
        query.transparency = config.transparency;
        query.minVoxelsProximity = config.minVoxelsProximity;
        query.avoidScanGlitches = config.avoidScanGlitches;

        octree.queryRay(query, &hits);

        if(hits.empty())
        {
            rgba[0] = config.background.r;
            rgba[1] = config.background.g;
            rgba[2] = config.background.b;
            rgba[3] = config.background.a;
            return;
        }

        size_t i = hits.size();
        size_t end = 0;
        if(config.maxVoxelsByRay > 0 && i > config.maxVoxelsByRay)
        {
            end = i - config.maxVoxelsByRay;
        }

        // Same back to front schedule and per iteration truncation as the
        // historic pixel loop.
        UC ro = config.background.r;
        UC go = config.background.g;
        UC bo = config.background.b;
        UC alpha = 0;

        while(i-- > end)
        {
            const Octree::RayHit& hit = hits[i];
            const float r = (float)hit.color.r;
            const float g = (float)hit.color.g;
            const float b = (float)hit.color.b;
            float a = (float)hit.color.a;
            if(hit.color.a > alpha)
            {
                alpha = hit.color.a;
            }

            const float darkness = octree.getDarkness(hit.depth);
            a = (a / 255.0f) * darkness * config.transparentA;

            ro = (UC)(r * config.transparentR * a + (1.0f - a) * ro);
            go = (UC)(g * config.transparentG * a + (1.0f - a) * go);
            bo = (UC)(b * config.transparentB * a + (1.0f - a) * bo);
        }

        rgba[0] = ro;
        rgba[1] = go;
        rgba[2] = bo;
        rgba[3] = 0xFF;
    }

    void renderTask(const std::vector<CPUFrameViewport>& viewports,
    const BatchParams& batch, const RenderTask& task,
    std::vector<CPUFrameResult>* const results,
    std::atomic<unsigned long long>* const pixelQueries)
    {
        const CPUFrameViewport& viewport = viewports[task.viewport];
        CPUFrameResult& result = (*results)[task.viewport];

        CPUFramePixelConfig config;
        config.pixelStep = viewport.pixelStep;
        config.perspectiveFactor = viewport.perspectiveFactor;
        config.zoom = viewport.zoom;
        config.background = viewport.background;
        config.transparentR = viewport.transparentR;
        config.transparentG = viewport.transparentG;
        config.transparentB = viewport.transparentB;
        config.transparentA = viewport.transparentA;
        config.maxVoxelsByRay = batch.maxVoxelsByRay;
        config.transparency = batch.transparency;
        config.minVoxelsProximity = batch.minVoxelsProximity;
        config.avoidScanGlitches = batch.avoidScanGlitches;
        config.maxRayDistance = viewport.octree->getRayLength();

        const int halfWidth = viewport.width / 2;
        const int halfHeight = viewport.height / 2;

        std::vector<Octree::RayHit> hits;
        unsigned long long queries = 0;

        for(int y = task.rowBegin; y < task.rowEnd; y++)
        {
            UC* const row = &result.pixels[(size_t)y * viewport.width * 4];
            for(int x = 0; x < viewport.width; x++)
            {
                shadePixel(*viewport.octree, viewport.basis, viewport.position,
                x - halfWidth, y - halfHeight, config, hits,
                &row[(size_t)x * 4]);
                queries++;
            }
        }

        (*pixelQueries) += queries;
    }
}

void cpuFrameRenderPixel(const Octree& octree, const Mat3f& basis,
const Vec3<float>& position, const int pixelX, const int pixelY,
const CPUFramePixelConfig& config, UC* const rgba)
{
    std::vector<Octree::RayHit> hits;
    shadePixel(octree, basis, position, pixelX, pixelY, config, hits, rgba);
}

std::vector<CPUFrameResult> cpuFrameRender(
const std::vector<CPUFrameViewport>& viewports,
const CPUFrameOptions& options, CPUFrameStats* const stats)
{
    validate(viewports, options);

    BatchParams batch;
    batch.transparency = options.transparency;
    batch.minVoxelsProximity = options.minVoxelsProximity;
    batch.avoidScanGlitches = options.avoidScanGlitches;
    batch.maxVoxelsByRay = options.maxVoxelsByRay;

    std::vector<CPUFrameResult> results(viewports.size());
    std::vector<RenderTask> tasks;

    for(size_t i = 0; i < viewports.size(); i++)
    {
        const CPUFrameViewport& viewport = viewports[i];
        results[i].width = viewport.width;
        results[i].height = viewport.height;
        results[i].pixels.assign((size_t)viewport.width *
        (size_t)viewport.height * 4, 0);

        for(int y = 0; y < viewport.height; y += options.tileRows)
        {
            RenderTask task;
            task.viewport = i;
            task.rowBegin = y;
            task.rowEnd = y + options.tileRows < viewport.height
            ? y + options.tileRows : viewport.height;
            tasks.push_back(task);
        }
    }

    const unsigned int workerCount = tasks.empty()
    ? 0u : (unsigned int)options.workers;

    std::atomic<size_t> nextTask(0);
    std::atomic<unsigned long long> pixelQueries(0);
    StartGate gate(workerCount);

    std::vector<std::thread> threads;
    threads.reserve(workerCount);
    try
    {
        for(unsigned int w = 0; w < workerCount; w++)
        {
            threads.push_back(std::thread([&]() {
                gate.arriveAndWait();
                size_t index = 0;
                while((index = nextTask.fetch_add(1)) < tasks.size())
                {
                    renderTask(viewports, batch, tasks[index], &results,
                    &pixelQueries);
                }
            }));
        }
    }
    catch(...)
    {
        // Release the workers that were already created, then let the
        // exception propagate.
        gate.openGate();
        for(size_t t = 0; t < threads.size(); t++)
        {
            threads[t].join();
        }
        throw;
    }
    for(size_t t = 0; t < threads.size(); t++)
    {
        threads[t].join();
    }

    if(stats != NULL)
    {
        stats->viewports = (unsigned int)viewports.size();
        stats->pixelQueries = pixelQueries.load();
        stats->workers = workerCount;
    }

    return results;
}
