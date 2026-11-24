/*
 * Cloud of Voxels (CoV) project
 * GL-free, multi-viewport CPU batch frame rendering.
 * Creation Date: 2025
 */

#include "./headers/CPUFrame.h"

#include <atomic>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace cpuframe
{
    namespace
    {
        constexpr float BASIS_LENGTH_EPSILON = 1.0e-3f;
        constexpr float BASIS_ORTHO_EPSILON = 1.0e-4f;

        bool finiteVector(const Vec3<float>& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        float vectorLength(const Vec3<float>& v)
        {
            return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        }

        float dot(const Vec3<float>& a, const Vec3<float>& b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        Vec3<float> scale(const Vec3<float>& v, const float s)
        {
            return {v.x * s, v.y * s, v.z * s};
        }

        Vec3<float> add(const Vec3<float>& a, const Vec3<float>& b)
        {
            return {a.x + b.x, a.y + b.y, a.z + b.z};
        }

        /*
        * Mirrors Octree::setRay normalizer computation for a camera axis.
        */
        Vec3<float> normalizerFor(const Vec3<float>& axis)
        {
            return {
                math::absf(axis.x) > 0.001f ? 1.0f / axis.x : 1000.0f,
                math::absf(axis.y) > 0.001f ? 1.0f / axis.y : 1000.0f,
                math::absf(axis.z) > 0.001f ? 1.0f / axis.z : 1000.0f
            };
        }

        /*
        * Full validation of one viewport. Runs for every viewport before any
        * worker thread is started, so a batch never starts half of its work.
        */
        void validateViewport(const ViewportInput& v)
        {
            if(v.tree == NULL)
            {
                throw std::invalid_argument("viewport has a null tree");
            }
            if(v.width == 0 || v.height == 0 ||
                (v.width % 2) != 0 || (v.height % 2) != 0)
            {
                throw std::invalid_argument("viewport width and height must be positive even numbers");
            }
            if(!finiteVector(v.basis.i) || !finiteVector(v.basis.j) ||
                !finiteVector(v.basis.k) || !finiteVector(v.position))
            {
                throw std::invalid_argument("viewport camera vectors must be finite");
            }
            if(!std::isfinite(v.perspectiveFactor) || !std::isfinite(v.zoom) ||
                !std::isfinite(v.maxRayDistance))
            {
                throw std::invalid_argument("viewport scalar parameters must be finite");
            }
            if(v.perspectiveFactor < 0.0f)
            {
                throw std::invalid_argument("viewport perspective factor must be non-negative");
            }
            if(v.zoom <= 0.0f)
            {
                throw std::invalid_argument("viewport zoom must be positive");
            }
            if(v.maxRayDistance <= 0.0f)
            {
                throw std::invalid_argument("viewport max ray distance must be positive");
            }
            if(v.maxVoxelsByRay == 0)
            {
                throw std::invalid_argument("viewport max voxels by ray must be positive");
            }

            const float li = vectorLength(v.basis.i);
            const float lj = vectorLength(v.basis.j);
            const float lk = vectorLength(v.basis.k);
            if(li == 0.0f || lj == 0.0f || lk == 0.0f)
            {
                throw std::invalid_argument("viewport basis vectors must be non-zero");
            }
            if(fabsf(li - 1.0f) > BASIS_LENGTH_EPSILON ||
                fabsf(lj - 1.0f) > BASIS_LENGTH_EPSILON ||
                fabsf(lk - 1.0f) > BASIS_LENGTH_EPSILON)
            {
                throw std::invalid_argument("viewport basis vectors must be normalized");
            }
            if(fabsf(dot(v.basis.i, v.basis.j)) > BASIS_ORTHO_EPSILON ||
                fabsf(dot(v.basis.j, v.basis.k)) > BASIS_ORTHO_EPSILON ||
                fabsf(dot(v.basis.k, v.basis.i)) > BASIS_ORTHO_EPSILON)
            {
                throw std::invalid_argument("viewport basis vectors must be orthogonal");
            }
        }

        /*
        * Builds the read-only ray for a pixel exactly like Core::process:
        * origin from the i/j pixel axes plus the camera position, direction
        * equal to k for orthographic rays or the bent, normalized k for
        * perspective rays.
        */
        RayQuery buildRay(const ViewportInput& v, const int curpixX,
        const int curpixY)
        {
            Vec3<float> origin = scale(v.basis.i, (float)curpixX);
            origin = add(origin, scale(v.basis.j, (float)curpixY));
            origin = add(origin, v.position);

            Vec3<float> direction = v.basis.k;
            const bool perspective = v.perspectiveFactor > 0.0f;
            if(perspective)
            {
                const Vec3<float> bent = add(
                    scale(v.basis.i, v.zoom * (float)curpixX),
                    scale(v.basis.j, v.zoom * (float)curpixY));
                direction.x += bent.x * v.perspectiveFactor;
                direction.y += bent.y * v.perspectiveFactor;
                direction.z += bent.z * v.perspectiveFactor;
                /*
                * Divide in place, exactly like math::normalize does in the
                * legacy setPerspectiveRay path.
                */
                const float norm = vectorLength(direction);
                if(norm > 0.0f)
                {
                    direction.x /= norm;
                    direction.y /= norm;
                    direction.z /= norm;
                }
            }

            RayQuery query;
            query.origin = origin;
            query.direction = direction;
            query.maxDistance = v.maxRayDistance;
            query.transparency = v.transparency;
            query.minVoxelProximity = (SUI)v.minVoxelProximity;
            query.avoidGlitches = v.avoidGlitches;
            /*
            * The legacy path derives the border-step normalizer from the
            * unperturbed camera k axis in setRay, before perspective bends
            * the direction. Reuse that same normalizer for both modes.
            */
            query.useNormalizer = true;
            query.normalizer = normalizerFor(v.basis.k);
            return query;
        }

        /*
        * Back-to-front blending with depth darkening and the
        * MAX_VOXELS_BY_RAY truncation, copied from Core::process.
        */
        ColorRGBA shadePixel(const ViewportInput& v,
        const std::vector<RayHit>& hits)
        {
            if(hits.empty())
            {
                return v.background;
            }

            unsigned int i = (unsigned int)hits.size();
            unsigned int end = 0;
            if(i > v.maxVoxelsByRay)
            {
                end = i - v.maxVoxelsByRay;
            }

            unsigned int ro = 0, go = 0, bo = 0, alpha = 0;
            while(i-- > end)
            {
                const unsigned int color = hits[i].color;
                const unsigned int r = (color & 0xFF000000) >> 24;
                const unsigned int g = (color & 0x00FF0000) >> 16;
                const unsigned int b = (color & 0x0000FF00) >> 8;
                unsigned int a = (color & 0x000000FF);
                if(a > alpha)
                {
                    alpha = a;
                }

                const float d = v.tree->getDarkness(hits[i].depth);
                const float af = (a / 255.0f) * d;

                ro = (unsigned char)(r * af + (1.0f - af) * ro);
                go = (unsigned char)(g * af + (1.0f - af) * go);
                bo = (unsigned char)(b * af + (1.0f - af) * bo);
            }
            return {(unsigned char)ro, (unsigned char)go, (unsigned char)bo, 0xFF};
        }

        ColorRGBA renderOnePixel(const ViewportInput& v,
        const int curpixX, const int curpixY)
        {
            const RayQuery query = buildRay(v, curpixX, curpixY);
            const std::vector<RayHit> hits = v.tree->traceRay(query);
            return shadePixel(v, hits);
        }

        void renderRows(const ViewportInput& v, unsigned char* const pixels,
        const unsigned int rowStart, const unsigned int rowEnd,
        std::atomic<unsigned long long>* const pixelCounter)
        {
            const int halfW = (int)(v.width / 2);
            const int halfH = (int)(v.height / 2);
            unsigned long long produced = 0;
            for(unsigned int py = rowStart; py < rowEnd; ++py)
            {
                const int curpixY = (int)py - halfH;
                for(unsigned int px = 0; px < v.width; ++px)
                {
                    const int curpixX = (int)px - halfW;
                    const ColorRGBA sample = renderOnePixel(v, curpixX, curpixY);
                    unsigned char* const out = pixels +
                        (4u * ((size_t)py * v.width + px));
                    out[0] = sample.r;
                    out[1] = sample.g;
                    out[2] = sample.b;
                    out[3] = sample.a;
                    ++produced;
                }
            }
            if(pixelCounter != NULL)
            {
                pixelCounter->fetch_add(produced, std::memory_order_relaxed);
            }
        }

        struct RowBlock
        {
            unsigned int viewport;
            unsigned int rowStart;
            unsigned int rowCount;
        };

        std::vector<RowBlock> buildBlocks(const std::vector<ViewportInput>& viewports,
        const unsigned int rowBlockSize, const TaskOrder order)
        {
            std::vector<std::vector<RowBlock>> perViewport(viewports.size());
            size_t total = 0;
            for(size_t vi = 0; vi < viewports.size(); ++vi)
            {
                for(unsigned int row = 0; row < viewports[vi].height;
                    row += rowBlockSize)
                {
                    const unsigned int count = (row + rowBlockSize <=
                        viewports[vi].height) ? rowBlockSize :
                        (viewports[vi].height - row);
                    perViewport[vi].push_back(
                        {(unsigned int)vi, row, count});
                    ++total;
                }
            }

            std::vector<RowBlock> blocks;
            blocks.reserve(total);
            if(order == TaskOrder::Sequential)
            {
                for(std::vector<RowBlock>& list : perViewport)
                {
                    for(RowBlock& block : list)
                    {
                        blocks.push_back(block);
                    }
                }
                return blocks;
            }

            /*
            * Round-robin viewports so consecutive blocks bounce between
            * trees. Pixels are unchanged; only scheduling order differs.
            */
            std::vector<size_t> nextOf(viewports.size(), 0);
            while(blocks.size() < total)
            {
                for(size_t vi = 0; vi < perViewport.size(); ++vi)
                {
                    if(nextOf[vi] < perViewport[vi].size())
                    {
                        blocks.push_back(perViewport[vi][nextOf[vi]]);
                        ++nextOf[vi];
                    }
                }
            }
            return blocks;
        }
    }

    void renderPixelBuffer(const ViewportInput& viewport, unsigned char* pixels)
    {
        if(pixels == NULL)
        {
            throw std::invalid_argument("renderPixelBuffer requires a non-null buffer");
        }
        validateViewport(viewport);
        renderRows(viewport, pixels, 0, viewport.height, NULL);
    }

    BatchResult renderBatch(const std::vector<ViewportInput>& viewports,
        const unsigned int workerCount, const unsigned int rowBlockSize,
        const TaskOrder order)
    {
        if(workerCount < 1 || workerCount > 32)
        {
            throw std::invalid_argument("worker count must be between 1 and 32");
        }
        if(rowBlockSize == 0)
        {
            throw std::invalid_argument("row block size must be positive");
        }
        for(const ViewportInput& v : viewports)
        {
            validateViewport(v);
        }

        BatchResult result;
        result.frames.reserve(viewports.size());
        for(const ViewportInput& v : viewports)
        {
            ViewportFrame frame;
            frame.width = v.width;
            frame.height = v.height;
            frame.tree = v.tree;
            frame.rgba.assign((size_t)4 * v.width * v.height, 0);
            result.frames.push_back(std::move(frame));
        }

        if(viewports.empty())
        {
            result.stats.viewportsCompleted = 0;
            result.stats.pixelQueries = 0;
            result.stats.workerThreads = 0;
            return result;
        }

        const std::vector<RowBlock> blocks = buildBlocks(viewports, rowBlockSize, order);

        std::atomic<size_t> cursor(0);
        std::atomic<unsigned long long> pixelQueries(0);

        auto worker = [&]()
        {
            while(true)
            {
                const size_t index = cursor.fetch_add(1, std::memory_order_relaxed);
                if(index >= blocks.size())
                {
                    return;
                }
                const RowBlock& block = blocks[index];
                const ViewportInput& v = viewports[block.viewport];
                renderRows(v, result.frames[block.viewport].rgba.data(),
                    block.rowStart, block.rowStart + block.rowCount,
                    &pixelQueries);
            }
        };

        std::vector<std::thread> pool;
        pool.reserve(workerCount);
        for(unsigned int w = 0; w < workerCount; ++w)
        {
            pool.emplace_back(worker);
        }
        for(std::thread& thread : pool)
        {
            thread.join();
        }

        result.stats.viewportsCompleted = (unsigned int)viewports.size();
        result.stats.pixelQueries = pixelQueries.load(std::memory_order_relaxed);
        result.stats.workerThreads = workerCount;
        return result;
    }
}
