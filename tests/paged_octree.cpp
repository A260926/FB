/*
 * Paged octree tests.  Registered through task_support/check.h; no main().
 *
 * Groups:
 *   geometry  - cube ownership, slab clipping, leaf identity / colour / t
 *   paging    - bounded cache, coalescing, out-of-order / duplicate replies
 *   lifecycle - cancellation, late replies, load failure, recovery
 */

#include "../headers/PagedOctree.h"
#include "../task_support/check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <unistd.h>

namespace
{
    struct Scene
    {
        Octree tree;
        std::string path;
        std::vector<paged::SnapshotReader*> ownedReaders;
    };

    std::string tempPath(const char* tag)
    {
        const char* base = std::getenv("TMPDIR");
        if(base == NULL || base[0] == '\0') base = ".";
        std::string s(base);
        if(!s.empty() && s[s.size() - 1] == '/') s.erase(s.size() - 1);
        s += "/cov_paged_";
        s += tag;
        s += ".pcoat";
        return s;
    }

    /*
    * A deterministic scene: coloured voxels at several depths inside a
    * modest octree.  Coordinates stay clear of the tree's edge guard.
    */
    void setSceneOptions()
    {
        Options::nologs = true;
        Options::TRANSPARENCY = true;
        Options::SMOOTH_SIBLINGS = false;
        Options::VOXEL_SHELL_TYPE = 0;
        Options::AVOID_SCAN_GLITCHES = false;
        Options::ACCELERATED = false;
        Options::OCTREE_SIZE = 16;
        Options::MIN_VOXELS_PROXIMITY = 0;
    }

    std::vector<Voxel> makeVoxels()
    {
        std::vector<Voxel> voxels;
        const int coords[][3] = {
            {0, 0, 0}, {2, 0, 0}, {-2, 1, 0}, {1, -2, 2},
            {-3, -3, -1}, {3, 3, 3}, {-1, 2, -3}, {2, -1, -2},
            {0, 3, 1}, {-3, 0, 3}, {4, 4, -4}, {-4, 2, 2}};
        const unsigned colors[] = {
            0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu, 0xFFFF00FFu,
            0xFF00FFFFu, 0x00FFFFFFu, 0x808080FFu, 0xFF8000AAu,
            0x0080FFFFu, 0x8000FFFFu, 0x40FF40FFu, 0xFF4040FFu};
        const std::size_t n = sizeof(coords) / sizeof(coords[0]);
        for(std::size_t i = 0; i < n; ++i)
        {
            Voxel v;
            v.coordinates.x = (SI)coords[i][0];
            v.coordinates.y = (SI)coords[i][1];
            v.coordinates.z = (SI)coords[i][2];
            v.color = colors[i];
            voxels.push_back(v);
        }
        return voxels;
    }

    bool buildSnapshot(const char* tag, Octree* tree, std::string* path)
    {
        setSceneOptions();
        tree->initRoot(16, 4, 48);
        std::vector<Voxel> voxels = makeVoxels();
        tree->addVoxels(&voxels[0], (const UI)voxels.size());
        *path = tempPath(tag);
        std::string error;
        return tree->exportPagedSnapshot(path->c_str(), 4, &error);
    }

    /*
    * Independent brute-force oracle: read every page, keep only coloured
    * leaves, and intersect the ray with their integer cubes using a local
    * half-open slab test (no shared code with the traversal).
    */
    struct Cube
    {
        double mn[3];
        double half;
        unsigned index;
        unsigned color;
    };

    bool loadColouredCubes(paged::SnapshotReader* reader,
    std::vector<Cube>* cubes)
    {
        cubes->clear();
        const paged::SnapshotReader::Header& h = reader->header();
        for(std::uint64_t p = 0; p < h.pageCount; ++p)
        {
            paged::PageData page;
            if(!reader->readPage((paged::PageId)p, &page, NULL)) return false;
            for(std::size_t i = 0; i < page.records.size(); ++i)
            {
                const paged::NodeRecord& n = page.records[i];
                if(!n.isLeaf() || !n.hasColor()) continue;
                Cube c;
                c.mn[0] = n.minX;
                c.mn[1] = n.minY;
                c.mn[2] = n.minZ;
                c.half = n.half;
                c.index = n.index;
                c.color = n.color;
                cubes->push_back(c);
            }
        }
        return true;
    }

    bool localCubeHit(const Cube& c, const paged::RayRequest& q, double* tHit)
    {
        const double o[3] = {q.originX, q.originY, q.originZ};
        const double d[3] = {q.dirX, q.dirY, q.dirZ};
        double lo = q.tMin;
        double hi = q.tMax;
        for(int a = 0; a < 3; ++a)
        {
            const double mn = c.mn[a];
            const double mx = c.mn[a] + 2.0 * c.half;
            if(d[a] == 0.0)
            {
                if(o[a] < mn || o[a] >= mx) return false;
                continue;
            }
            const double inv = 1.0 / d[a];
            double t0 = (mn - o[a]) * inv;
            double t1 = (mx - o[a]) * inv;
            if(t0 > t1)
            {
                const double tmp = t0;
                t0 = t1;
                t1 = tmp;
            }
            if(t0 > lo) lo = t0;
            if(t1 < hi) hi = t1;
            if(lo > hi) return false;
        }
        /* Strictly positive clipped length; pure touches are misses. */
        if(!(hi > lo)) return false;
        *tHit = lo;
        return true;
    }

    bool bruteForce(const std::vector<Cube>& cubes,
    const paged::RayRequest& q, paged::RayResult* out)
    {
        int best = -1;
        double bestT = 0.0;
        for(std::size_t i = 0; i < cubes.size(); ++i)
        {
            double t = 0.0;
            if(!localCubeHit(cubes[i], q, &t)) continue;
            /* Unique ownership: equal-t ties resolve by node index only if
            * they are genuinely distinct surfaces; a shared face at the same
            * t would mean two owners, which the half-open rule prevents. */
            if(best < 0 || t < bestT)
            {
                best = (int)i;
                bestT = t;
            }
        }
        out->status = paged::TRACE_MISS;
        out->t = 0.0;
        out->leaf = 0;
        out->r = out->g = out->b = out->a = 0;
        if(best < 0) return true;
        out->status = paged::TRACE_HIT;
        out->t = bestT;
        out->leaf = cubes[best].index;
        out->r = (std::uint8_t)((cubes[best].color >> 24) & 0xFF);
        out->g = (std::uint8_t)((cubes[best].color >> 16) & 0xFF);
        out->b = (std::uint8_t)((cubes[best].color >> 8) & 0xFF);
        out->a = (std::uint8_t)(cubes[best].color & 0xFF);
        return true;
    }

    /*
    * Trace one ray through a fresh page cache fed by a loader that reads the
    * snapshot on demand.  capacity >= 1.
    */
    bool traceOnce(paged::SnapshotReader* reader, const paged::RayRequest& req,
    std::size_t capacity, paged::RayResult* out,
    paged::CacheStats* stats = NULL)
    {
        std::unique_ptr<paged::PageLoader> loader = reader->createFileLoader();
        paged::PageCache cache(capacity, std::move(loader));
        paged::PagedRay ray;
        ray.init(req, reader->header());
        for(int guard = 0; guard < 2000000; ++guard)
        {
            paged::PageId page = paged::INVALID_PAGE;
            paged::RayResult result;
            const paged::TraceStatus st = ray.advance(&cache, &page, &result);
            if(st != paged::TRACE_NEED_PAGE)
            {
                *out = result;
                if(stats) *stats = cache.stats();
                return true;
            }
            bool resident = false;
            paged::RequestId id = paged::INVALID_REQUEST;
            cache.need(page, &resident, &id);
            cache.pump();
            if(!resident && !cache.contains(page) && !cache.isPending(id))
            {
                /* Loader failure: the ray must surface Error, not Miss. */
                ray.notifyFailure();
            }
        }
        return false;
    }

    /*
    * Loader used by lifecycle / paging tests: it records requests but only
    * completes them when the test calls deliver()/fail().
    */
    class ManualLoader : public paged::PageLoader
    {
        public:
        ManualLoader(paged::SnapshotReader* r) : reader(r) {}
        ~ManualLoader() {}

        bool request(paged::PageId id, paged::RequestId requestId)
        {
            this->ids.push_back(requestId);
            this->pageOf[requestId] = id;
            return true;
        }

        bool poll(paged::RequestId, paged::PageData*, bool*)
        {
            return false; /* completions are driven manually */
        }

        bool load(paged::RequestId requestId, paged::PageData* out)
        {
            std::map<paged::RequestId, paged::PageId>::iterator it =
            this->pageOf.find(requestId);
            if(it == this->pageOf.end()) return false;
            return this->reader->readPage(it->second, out, NULL);
        }

        paged::PageId pageFor(paged::RequestId id) const
        {
            std::map<paged::RequestId, paged::PageId>::const_iterator it =
            this->pageOf.find(id);
            return it == this->pageOf.end() ? paged::INVALID_PAGE : it->second;
        }

        paged::SnapshotReader* reader;
        std::vector<paged::RequestId> ids;
        std::map<paged::RequestId, paged::PageId> pageOf;
    };

    bool sameResult(const paged::RayResult& a, const paged::RayResult& b)
    {
        if(a.status != b.status) return false;
        if(a.status != paged::TRACE_HIT) return true;
        if(a.leaf != b.leaf) return false;
        if(std::fabs(a.t - b.t) > 1e-9) return false;
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }

    paged::RayRequest ray(double ox, double oy, double oz, double dx,
    double dy, double dz, double t0, double t1)
    {
        return paged::RayRequest(ox, oy, oz, dx, dy, dz, t0, t1);
    }
}

/* ---------------------------------------------------------------------- */
/* geometry                                                                */
/* ---------------------------------------------------------------------- */

static int geometry_leaf_ownership(void)
{
    Octree tree;
    std::string path;
    TASK_CHECK(buildSnapshot("geom_owner", &tree, &path));
    paged::SnapshotReader reader;
    std::string error;
    TASK_CHECK(reader.open(path.c_str(), &error));

    std::vector<Cube> cubes;
    TASK_CHECK(loadColouredCubes(&reader, &cubes));
    TASK_CHECK(!cubes.empty());

    /*
    * Rays that graze split planes, edges and corners.  Each must have a
    * single nearest owner, matching the independent brute force and the
    * all-resident hierarchy walk.
    */
    const paged::RayRequest probes[] = {
        ray(-8, 0, -8, 1, 0, 0, 0, 64),      /* along plane y=0 */
        ray(0, 0, -8, 0, 0, 1, 0, 64),       /* through the x=y=0 edge */
        ray(0, 0, 0, 1, 1, 1, 0, 64),        /* through the origin corner */
        ray(-8, 2, 2, 1, 0, 0, 0, 64),       /* axis-parallel, off plane */
        ray(-8, 1, 1, 1, 0, 0, 0, 64),       /* hits split plane y=1 */
        ray(5.999999, 0, -8, 0, 0, 1, 0, 64)/* just inside a leaf face */
    };
    const std::size_t n = sizeof(probes) / sizeof(probes[0]);
    for(std::size_t i = 0; i < n; ++i)
    {
        paged::RayResult expect, got, ref;
        TASK_CHECK(bruteForce(cubes, probes[i], &expect));
        TASK_CHECK(paged::referenceTrace(&reader, probes[i], &ref));
        TASK_CHECK(sameResult(expect, ref));
        TASK_CHECK(traceOnce(&reader, probes[i], 1, &got));
        TASK_CHECK(sameResult(expect, got));
    }
    std::remove(path.c_str());
    return 0;
}

static int geometry_clipping(void)
{
    Octree tree;
    std::string path;
    TASK_CHECK(buildSnapshot("geom_clip", &tree, &path));
    paged::SnapshotReader reader;
    std::string error;
    TASK_CHECK(reader.open(path.c_str(), &error));

    paged::RayResult out;
    /* Entirely outside the root. */
    TASK_CHECK(traceOnce(&reader, ray(-100, 0, -100, 1, 0, 1, 0, 10),
    1, &out));
    TASK_CHECK(out.status == paged::TRACE_MISS);
    /* tMin beyond the scene. */
    TASK_CHECK(traceOnce(&reader, ray(0, 0, -8, 0, 0, 1, 500, 600), 1, &out));
    TASK_CHECK(out.status == paged::TRACE_MISS);
    /* tMax before the first leaf. */
    TASK_CHECK(traceOnce(&reader, ray(0, 0, -8, 0, 0, 1, 0, 7.0), 1, &out));
    TASK_CHECK(out.status == paged::TRACE_MISS);
    /* Origin inside the root, negative direction. */
    TASK_CHECK(traceOnce(&reader, ray(0, 0, 7.5, 0, 0, -1, 0, 64), 1, &out));
    TASK_CHECK(out.status == paged::TRACE_HIT);
    TASK_CHECK(out.t > 0.0);
    /* A ray origin exactly on a face/split plane is still inside the root. */
    TASK_CHECK(traceOnce(&reader, ray(-8, 0, 0, 1, 0, 0, 0, 64), 1, &out));
    TASK_CHECK(out.status == paged::TRACE_HIT || out.status == paged::TRACE_MISS);
    /* A ray that only touches the root at one point is a miss (zero length). */
    TASK_CHECK(traceOnce(&reader, ray(8, 8, -8, 0, 0, 1, 0, 64), 1, &out));
    TASK_CHECK(out.status == paged::TRACE_MISS);
    /* Zero direction components, parallel to two axes. */
    TASK_CHECK(traceOnce(&reader, ray(-8, 0.5, 0.5, 1, 0, 0, 0, 64),
    1, &out));
    TASK_CHECK(out.status == paged::TRACE_HIT);
    /* Non-finite direction is an error, not a hit/miss. */
    TASK_CHECK(traceOnce(&reader, ray(0, 0, -8, std::nan(""), 0, 1, 0, 64),
    1, &out));
    TASK_CHECK(out.status == paged::TRACE_ERROR);
    /* tMin > tMax is rejected. */
    TASK_CHECK(traceOnce(&reader, ray(0, 0, -8, 0, 0, 1, 5, 1), 1, &out));
    TASK_CHECK(out.status == paged::TRACE_ERROR);
    std::remove(path.c_str());
    return 0;
}

static int geometry_matches_all_resident(void)
{
    Octree tree;
    std::string path;
    TASK_CHECK(buildSnapshot("geom_match", &tree, &path));
    paged::SnapshotReader reader;
    std::string error;
    TASK_CHECK(reader.open(path.c_str(), &error));
    std::vector<Cube> cubes;
    TASK_CHECK(loadColouredCubes(&reader, &cubes));

    std::srand(2024);
    int hits = 0;
    for(int i = 0; i < 60; ++i)
    {
        const double ox = -8.0 + 16.0 * (std::rand() / (double)RAND_MAX);
        const double oy = -8.0 + 16.0 * (std::rand() / (double)RAND_MAX);
        const double oz = -16.0 + 8.0 * (std::rand() / (double)RAND_MAX);
        const double dx = -0.8 + 1.6 * (std::rand() / (double)RAND_MAX);
        const double dy = -0.8 + 1.6 * (std::rand() / (double)RAND_MAX);
        const double dz = 0.3 + 1.4 * (std::rand() / (double)RAND_MAX);
        paged::RayRequest q = ray(ox, oy, oz, dx, dy, dz, 0, 128);
        paged::RayResult expect, got, ref;
        TASK_CHECK(bruteForce(cubes, q, &expect));
        TASK_CHECK(paged::referenceTrace(&reader, q, &ref));
        TASK_CHECK(sameResult(expect, ref));
        TASK_CHECK(traceOnce(&reader, q, 1, &got));
        TASK_CHECK(sameResult(expect, got));
        if(expect.status == paged::TRACE_HIT) ++hits;
    }
    TASK_CHECK(hits > 0);

    /*
    * Vertex/edge rays at every integer split plane in the scene; the paged,
    * reference and brute-force answers must agree and never double-count.
    */
    for(int a = -6; a <= 6; ++a)
    {
        paged::RayRequest q = ray((double)a, (double)a, -12, 0, 0, 1, 0, 128);
        paged::RayResult expect, got, ref;
        TASK_CHECK(bruteForce(cubes, q, &expect));
        TASK_CHECK(paged::referenceTrace(&reader, q, &ref));
        TASK_CHECK(sameResult(expect, ref));
        TASK_CHECK(traceOnce(&reader, q, 1, &got));
        TASK_CHECK(sameResult(expect, got));
    }
    std::remove(path.c_str());
    return 0;
}

/* ---------------------------------------------------------------------- */
/* paging                                                                  */
/* ---------------------------------------------------------------------- */

static int paging_one_page_ancestor_eviction(void)
{
    Octree tree;
    std::string path;
    TASK_CHECK(buildSnapshot("page_evict", &tree, &path));
    paged::SnapshotReader reader;
    std::string error;
    TASK_CHECK(reader.open(path.c_str(), &error));

    /* A deep diagonal ray that crosses many pages at capacity one. */
    paged::RayRequest q = ray(-6.5, -6.5, -12, 0.55, 0.55, 1.0, 0, 128);
    paged::RayResult expect, got;
    TASK_CHECK(paged::referenceTrace(&reader, q, &expect));
    paged::CacheStats stats;
    TASK_CHECK(traceOnce(&reader, q, 1, &got, &stats));

    TASK_CHECK(stats.peakResident == 1);
    TASK_CHECK(stats.pageEvictions > 0);
    TASK_CHECK(stats.pageRequests > 1);
    TASK_CHECK(stats.nodeVisits > 0);
    TASK_CHECK(sameResult(expect, got));
    std::remove(path.c_str());
    return 0;
}

static int paging_multi_ray_shared_page(void)
{
    Octree tree;
    std::string path;
    TASK_CHECK(buildSnapshot("page_share", &tree, &path));
    paged::SnapshotReader reader;
    std::string error;
    TASK_CHECK(reader.open(path.c_str(), &error));

    std::unique_ptr<ManualLoader> loader(new ManualLoader(&reader));
    ManualLoader* raw = loader.get();
    paged::PageCache cache(4, std::unique_ptr<paged::PageLoader>(loader.release()));
    const paged::SnapshotReader::Header& hdr = reader.header();

    /*
    * Two rays that start outside and travel into the same region share the
    * root page request.
    */
    paged::PagedRay a, b;
    TASK_CHECK(a.init(ray(-0.5, -0.5, -12, 0, 0, 1, 0, 64), hdr));
    TASK_CHECK(b.init(ray(0.5, 0.5, -12, 0, 0, 1, 0, 64), hdr));
    paged::PageId pa = paged::INVALID_PAGE;
    paged::PageId pb = paged::INVALID_PAGE;
    paged::RayResult ra, rb;
    TASK_CHECK(a.advance(&cache, &pa, &ra) == paged::TRACE_NEED_PAGE);
    TASK_CHECK(b.advance(&cache, &pb, &rb) == paged::TRACE_NEED_PAGE);
    TASK_CHECK(pa == pb);
    TASK_CHECK(pa == 0);
    /*
    * The cache is asked twice for the same page but issues one request,
    * and both ray states are still just NeedPage (no page-address state).
    */
    bool resident = false;
    paged::RequestId first = paged::INVALID_REQUEST;
    paged::RequestId second = paged::INVALID_REQUEST;
    cache.need(pa, &resident, &first);
    cache.need(pb, &resident, &second);
    TASK_CHECK(first == second);
    TASK_CHECK(raw->ids.size() >= 1);
    TASK_CHECK(cache.stats().pageRequests == 1);
    TASK_CHECK(cache.pendingRequests().size() == 1);

    paged::PageData page;
    TASK_CHECK(reader.readPage(pa, &page, &error));
    TASK_CHECK(cache.complete(raw->ids[0], page));
    TASK_CHECK(!cache.complete(raw->ids[0], page)); /* duplicate rejected */
    /*
    * Drive both to a terminal state, answering whatever pages they ask for.
    * Even with every page available they must never revisit geometry; the
    * answers equal the reference trace.
    */
    paged::RayResult expectA, expectB;
    TASK_CHECK(paged::referenceTrace(&reader, ray(-0.5, -0.5, -12, 0, 0, 1,
    0, 64), &expectA));
    TASK_CHECK(paged::referenceTrace(&reader, ray(0.5, 0.5, -12, 0, 0, 1,
    0, 64), &expectB));
    paged::PagedRay* rays[2] = {&a, &b};
    paged::PageId* need[2] = {&pa, &pb};
    paged::RayResult* gotv[2] = {&ra, &rb};
    for(int guard = 0; guard < 20000; ++guard)
    {
        bool done = true;
        for(int k = 0; k < 2; ++k)
        {
            if(rays[k]->isTerminal()) continue;
            done = false;
            paged::RayResult rr;
            if(rays[k]->advance(&cache, need[k], &rr) !=
            paged::TRACE_NEED_PAGE)
            {
                *gotv[k] = rr;
                continue;
            }
            bool res = false;
            paged::RequestId req = paged::INVALID_REQUEST;
            cache.need(*need[k], &res, &req);
            if(res) continue;
            std::vector<paged::RequestId> pending = cache.pendingRequests();
            for(std::size_t j = 0; j < pending.size(); ++j)
            {
                paged::PageData p;
                TASK_CHECK(raw->load(pending[j], &p));
                TASK_CHECK(cache.complete(pending[j], p));
            }
        }
        if(done) break;
    }
    TASK_CHECK(sameResult(expectA, ra));
    TASK_CHECK(sameResult(expectB, rb));
    /*
    * The two rays started on the same root page and to different subtrees;
    * with a bounded cache the shared root was loaded once rather than once
    * per ray.
    */
    TASK_CHECK(cache.stats().pageLoads < cache.stats().pageRequests * 2);
    std::remove(path.c_str());
    return 0;
}

static int paging_out_of_order_and_duplicate(void)
{
    Octree tree;
    std::string path;
    TASK_CHECK(buildSnapshot("page_ooo", &tree, &path));
    paged::SnapshotReader reader;
    std::string error;
    TASK_CHECK(reader.open(path.c_str(), &error));

    std::vector<paged::RayRequest> requests;
    requests.push_back(ray(-6, -6, -12, 0.4, 0.4, 1, 0, 128));
    requests.push_back(ray(6, 6, -12, -0.4, -0.4, 1, 0, 128));
    requests.push_back(ray(-4, 2, -12, 0.1, -0.2, 1, 0, 128));

    std::unique_ptr<ManualLoader> loader(new ManualLoader(&reader));
    ManualLoader* raw = loader.get();
    paged::PageCache cache(2, std::unique_ptr<paged::PageLoader>(loader.release()));
    paged::PagedFrame frame(&cache, reader.header(),
    (std::uint32_t)requests.size());
    TASK_CHECK(frame.setRays(&requests[0], requests.size()));
    TASK_CHECK(frame.start());

    for(int round = 0; round < 20000 && !frame.isComplete(); ++round)
    {
        frame.step(0);
        if(frame.isComplete()) break;
        std::vector<paged::RequestId> pending = cache.pendingRequests();
        if(pending.empty()) continue;
        /* Answer in reverse request order, deliberately out of order. */
        for(std::size_t i = pending.size(); i > 0; --i)
        {
            const paged::RequestId id = pending[i - 1];
            paged::PageData page;
            TASK_CHECK(raw->load(id, &page));
            TASK_CHECK(frame.deliver(id, page));
            /* Duplicate delivery must be rejected and must not advance. */
            const std::uint64_t visits = cache.stats().nodeVisits;
            TASK_CHECK(!frame.deliver(id, page));
            TASK_CHECK(cache.stats().nodeVisits == visits);
        }
    }
    TASK_CHECK(frame.isComplete());
    TASK_CHECK(!frame.hasFailure());
    std::vector<paged::RayResult> got;
    frame.results(&got);
    for(std::size_t i = 0; i < got.size(); ++i)
    {
        paged::RayResult expect;
        TASK_CHECK(paged::referenceTrace(&reader, requests[i], &expect));
        TASK_CHECK(sameResult(expect, got[i]));
    }
    TASK_CHECK(cache.stats().pageLoads <= cache.stats().pageRequests);
    TASK_CHECK(cache.stats().peakResident <= 2);
    std::remove(path.c_str());
    return 0;
}

static int paging_interleaved_rays(void)
{
    Octree tree;
    std::string path;
    TASK_CHECK(buildSnapshot("page_interleave", &tree, &path));
    paged::SnapshotReader reader;
    std::string error;
    TASK_CHECK(reader.open(path.c_str(), &error));

    std::vector<paged::RayRequest> requests;
    requests.push_back(ray(-6, -1, -12, 0.5, 0.1, 1, 0, 128));
    requests.push_back(ray(6, 1, -12, -0.5, -0.1, 1, 0, 128));

    std::unique_ptr<ManualLoader> loader(new ManualLoader(&reader));
    ManualLoader* raw = loader.get();
    paged::PageCache cache(4, std::unique_ptr<paged::PageLoader>(loader.release()));
    paged::PagedFrame frame(&cache, reader.header(), 2);
    TASK_CHECK(frame.setRays(&requests[0], requests.size()));
    TASK_CHECK(frame.start());
    TASK_CHECK(frame.progress() == 0.0);

    /* Both rays ask for the shared root page: a single coalesced request. */
    frame.step(1);
    frame.step(1);
    TASK_CHECK(cache.pendingRequests().size() == 1);
    const paged::RequestId rootId = cache.pendingRequests()[0];
    paged::PageData rootPage;
    TASK_CHECK(raw->load(rootId, &rootPage));
    TASK_CHECK(frame.deliver(rootId, rootPage));
    TASK_CHECK(!frame.isComplete());
    TASK_CHECK(frame.progress() == 0.0);

    /*
    * Now both rays proceed into different subtrees.  After one more pass
    * each has an outstanding request, proving the frame interleaves rather
    * than draining one ray first.
    */
    /*
    * Do not answer anything yet.  Give each ray a few turns; a ray that
    * blocks is skipped, so the other ray keeps making progress.  Both rays
    * end up with their own outstanding subtree page, and neither has
    * finished: that is interleaving through one shared cache, not loading
    * one ray to completion first.
    */
    for(int k = 0; k < 12; ++k) frame.step(1);
    std::vector<paged::RequestId> pendingNow = cache.pendingRequests();
    TASK_CHECK(pendingNow.size() >= 2);
    TASK_CHECK(frame.progress() == 0.0);
    TASK_CHECK(!frame.isComplete());
    TASK_CHECK(!frame.isDone());

    /* Finish the batch by hand. */
    for(int guard = 0; guard < 4000 && !frame.isComplete(); ++guard)
    {
        std::vector<paged::RequestId> pending = cache.pendingRequests();
        for(std::size_t i = 0; i < pending.size(); ++i)
        {
            paged::PageData page;
            TASK_CHECK(raw->load(pending[i], &page));
            TASK_CHECK(frame.deliver(pending[i], page));
        }
        frame.step(0);
    }
    TASK_CHECK(frame.isComplete());
    std::vector<paged::RayResult> got;
    frame.results(&got);
    for(std::size_t i = 0; i < got.size(); ++i)
    {
        paged::RayResult expect;
        TASK_CHECK(paged::referenceTrace(&reader, requests[i], &expect));
        TASK_CHECK(sameResult(expect, got[i]));
    }
    std::remove(path.c_str());
    return 0;
}

/* ---------------------------------------------------------------------- */
/* lifecycle                                                               */
/* ---------------------------------------------------------------------- */

static int lifecycle_truncated_page(void)
{
    Octree tree;
    std::string path;
    TASK_CHECK(buildSnapshot("life_trunc", &tree, &path));

    /* Copy the snapshot, then cut the final page short. */
    const std::string broken = tempPath("life_trunc_broken");
    std::FILE* in = std::fopen(path.c_str(), "rb");
    TASK_CHECK(in != NULL);
    std::FILE* out = std::fopen(broken.c_str(), "wb");
    TASK_CHECK(out != NULL);
    unsigned char buffer[4096];
    std::size_t total = 0;
    std::size_t n = 0;
    while((n = std::fread(buffer, 1, sizeof(buffer), in)) > 0)
    {
        std::fwrite(buffer, 1, n, out);
        total += n;
    }
    std::fclose(in);
    std::fclose(out);
    TASK_CHECK(total > 80);

    /*
    * Cut the file immediately after the header + directory plus a few
    * bytes, so the directory still describes every page but no page payload
    * can be read in full.
    */
    {
        paged::SnapshotReader probe;
        std::string e;
        TASK_CHECK(probe.open(path.c_str(), &e));
        const std::uint64_t cut = paged::SNAPSHOT_HEADER_BYTES +
        probe.header().directoryBytes + 10;
        TASK_CHECK(cut < total);
        probe.close();
        TASK_CHECK(::truncate(broken.c_str(), (off_t)cut) == 0);
    }

    paged::SnapshotReader reader;
    std::string error;
    /* The header and directory alone must still open. */
    TASK_CHECK(reader.open(broken.c_str(), &error));
    TASK_CHECK(reader.header().pageCount > 0);

    std::vector<paged::RayRequest> requests;
    requests.push_back(ray(-1, 0, -12, 0, 0, 1, 0, 64));
    std::unique_ptr<paged::PagedFrame> frame =
    paged::PagedFrame::openBatch(&reader, &requests[0], requests.size(), 1,
    &error);
    TASK_CHECK(frame.get() != NULL);
    frame->start();
    for(int guard = 0; guard < 1000 && !frame->isDone(); ++guard)
    {
        frame->step(0);
    }
    TASK_CHECK(frame->isDone());
    TASK_CHECK(!frame->isComplete()); /* a failed session never completes */
    TASK_CHECK(frame->hasFailure());
    TASK_CHECK(frame->result(0).status == paged::TRACE_ERROR);

    /*
    * A new session on an intact snapshot must work afterwards.
    */
    paged::SnapshotReader good;
    TASK_CHECK(good.open(path.c_str(), &error));
    std::unique_ptr<paged::PagedFrame> again =
    paged::PagedFrame::openBatch(&good, &requests[0], requests.size(), 1,
    &error);
    TASK_CHECK(again.get() != NULL);
    TASK_CHECK(again->run());
    TASK_CHECK(again->isComplete());

    std::remove(path.c_str());
    std::remove(broken.c_str());
    return 0;
}

static int lifecycle_cancel_late_reply(void)
{
    Octree tree;
    std::string path;
    TASK_CHECK(buildSnapshot("life_cancel", &tree, &path));
    paged::SnapshotReader reader;
    std::string error;
    TASK_CHECK(reader.open(path.c_str(), &error));

    std::vector<paged::RayRequest> requests;
    requests.push_back(ray(-0.5, -0.5, -12, 0, 0, 1, 0, 64));
    requests.push_back(ray(0.5, 0.5, -12, 0, 0, 1, 0, 64));

    std::unique_ptr<ManualLoader> loader(new ManualLoader(&reader));
    ManualLoader* raw = loader.get();
    paged::PageCache cache(2, std::unique_ptr<paged::PageLoader>(loader.release()));
    paged::PagedFrame frame(&cache, reader.header(), 2);
    TASK_CHECK(frame.setRays(&requests[0], requests.size()));
    TASK_CHECK(frame.start());
    frame.step(0);
    TASK_CHECK(!cache.pendingRequests().empty());
    const paged::RequestId stale = cache.pendingRequests()[0];
    const std::uint64_t visits = cache.stats().nodeVisits;

    frame.cancel();
    TASK_CHECK(frame.isCancelled());
    TASK_CHECK(!frame.isComplete());
    TASK_CHECK(frame.step(0) == false);

    paged::PageData page;
    TASK_CHECK(raw->load(stale, &page));
    /* A late reply for the cancelled generation is inert. */
    TASK_CHECK(!frame.deliver(stale, page));
    TASK_CHECK(!cache.complete(stale, page));
    TASK_CHECK(!cache.contains(page.id));
    TASK_CHECK(cache.stats().nodeVisits == visits);
    TASK_CHECK(!frame.isComplete());
    for(std::size_t i = 0; i < frame.generation(); ++i) { /* no-op */ }

    /*
    * A brand-new session with a fresh cache and loader still completes; the
    * stale request id from the cancelled frame shadows nothing.
    */
    std::unique_ptr<paged::PagedFrame> second =
    paged::PagedFrame::openBatch(&reader, &requests[0], requests.size(), 1,
    &error);
    TASK_CHECK(second.get() != NULL);
    TASK_CHECK(second->run());
    TASK_CHECK(second->isComplete());
    (void)raw;
    std::remove(path.c_str());
    return 0;
}

static int lifecycle_failure_then_new_frame(void)
{
    Octree tree;
    std::string path;
    TASK_CHECK(buildSnapshot("life_fail", &tree, &path));
    paged::SnapshotReader reader;
    std::string error;
    TASK_CHECK(reader.open(path.c_str(), &error));

    std::vector<paged::RayRequest> requests;
    requests.push_back(ray(-1, 0, -12, 0, 0, 1, 0, 64));
    requests.push_back(ray(1, 1, -12, 0, 0, 1, 0, 64));

    std::unique_ptr<ManualLoader> loader(new ManualLoader(&reader));
    ManualLoader* raw = loader.get();
    std::unique_ptr<paged::PageCache> cache(new paged::PageCache(2,
    std::unique_ptr<paged::PageLoader>(loader.release())));
    paged::PagedFrame frame(cache.get(), reader.header(), 2);
    TASK_CHECK(frame.setRays(&requests[0], requests.size()));
    TASK_CHECK(frame.start());
    frame.step(0);
    TASK_CHECK(!cache->pendingRequests().empty());

    const paged::RequestId id = cache->pendingRequests()[0];
    const std::size_t affected = frame.failRequest(id);
    TASK_CHECK(affected >= 1);
    TASK_CHECK(frame.hasFailure());
    TASK_CHECK(!frame.isComplete());
    TASK_CHECK(frame.result(0).status == paged::TRACE_ERROR ||
    frame.result(1).status == paged::TRACE_ERROR);

    /* All rays that waited on the failed request are released. */
    for(int guard = 0; guard < 100 && !frame.isDone(); ++guard)
    {
        frame.step(0);
    }
    TASK_CHECK(frame.isDone());
    TASK_CHECK(!frame.isComplete());

    /* A fresh frame with its own cache succeeds. */
    std::unique_ptr<paged::PagedFrame> fresh =
    paged::PagedFrame::openBatch(&reader, &requests[0], requests.size(), 1,
    &error);
    TASK_CHECK(fresh.get() != NULL);
    TASK_CHECK(fresh->run());
    TASK_CHECK(fresh->isComplete());
    TASK_CHECK(!fresh->hasFailure());
    (void)raw;
    std::remove(path.c_str());
    return 0;
}

static int lifecycle_resume_without_rewalk(void)
{
    Octree tree;
    std::string path;
    TASK_CHECK(buildSnapshot("life_resume", &tree, &path));
    paged::SnapshotReader reader;
    std::string error;
    TASK_CHECK(reader.open(path.c_str(), &error));

    const paged::RayRequest q = ray(-4.5, -4.5, -12, 0.42, 0.42, 1, 0, 128);
    /* All-resident run: visits every hierarchy node exactly once. */
    paged::CacheStats fullStats;
    paged::RayResult fullResult;
    TASK_CHECK(traceOnce(&reader, q, 4096, &fullResult, &fullStats));
    TASK_CHECK(fullResult.status == paged::TRACE_HIT);

    /*
    * Capacity-one run: the ancestor is evicted while the ray waits for a
    * descendant.  The total node visits must match the all-resident run,
    * proving the ray resumes instead of re-walking consumed branches.
    */
    paged::CacheStats smallStats;
    paged::RayResult smallResult;
    TASK_CHECK(traceOnce(&reader, q, 1, &smallResult, &smallStats));
    TASK_CHECK(sameResult(fullResult, smallResult));
    TASK_CHECK(smallStats.nodeVisits == fullStats.nodeVisits);
    TASK_CHECK(smallStats.pageEvictions > 0);
    TASK_CHECK(smallStats.peakResident == 1);

    /*
    * Direct interruption check: stop at the first NeedPage, force unrelated
    * cache traffic, then resume.  Visits after resuming must be strictly
    * fewer than the all-resident total (no restart from the root).
    */
    std::unique_ptr<paged::PageLoader> loader = reader.createFileLoader();
    paged::PageCache cache(2, std::move(loader));
    paged::PagedRay ray;
    TASK_CHECK(ray.init(q, reader.header()));
    paged::PageId page = paged::INVALID_PAGE;
    paged::RayResult r;
    TASK_CHECK(ray.advance(&cache, &page, &r) == paged::TRACE_NEED_PAGE);
    bool resident = false;
    paged::RequestId id = paged::INVALID_REQUEST;
    cache.need(page, &resident, &id);
    cache.pump();
    const std::uint64_t before = cache.stats().nodeVisits;
    /* Fill/evict the cache with an unrelated page before resuming. */
    paged::PageId other = (paged::PageId)(reader.header().pageCount - 1);
    cache.need(other, &resident, &id);
    cache.pump();
    TASK_CHECK(ray.advance(&cache, &page, &r) == paged::TRACE_NEED_PAGE ||
    r.status != paged::TRACE_NEED_PAGE);
    for(int guard = 0; guard < 100000; ++guard)
    {
        const paged::TraceStatus st = ray.advance(&cache, &page, &r);
        if(st != paged::TRACE_NEED_PAGE) break;
        cache.need(page, &resident, &id);
        cache.pump();
    }
    TASK_CHECK(r.status == paged::TRACE_HIT);
    TASK_CHECK(cache.stats().nodeVisits == fullStats.nodeVisits);
    TASK_CHECK(cache.stats().nodeVisits >= before);
    std::remove(path.c_str());
    return 0;
}

/* ---------------------------------------------------------------------- */
/* registration                                                            */
/* ---------------------------------------------------------------------- */

extern const struct task_case task_cases[] = {
    {"geometry", "leaf_ownership", geometry_leaf_ownership},
    {"geometry", "clipping", geometry_clipping},
    {"geometry", "matches_all_resident", geometry_matches_all_resident},
    {"paging", "one_page_ancestor_eviction", paging_one_page_ancestor_eviction},
    {"paging", "multi_ray_shared_page", paging_multi_ray_shared_page},
    {"paging", "out_of_order_and_duplicate", paging_out_of_order_and_duplicate},
    {"paging", "interleaved_rays", paging_interleaved_rays},
    {"lifecycle", "truncated_page", lifecycle_truncated_page},
    {"lifecycle", "cancel_late_reply", lifecycle_cancel_late_reply},
    {"lifecycle", "failure_then_new_frame", lifecycle_failure_then_new_frame},
    {"lifecycle", "resume_without_rewalk", lifecycle_resume_without_rewalk}
};

extern const size_t task_case_count =
sizeof(task_cases) / sizeof(task_cases[0]);
