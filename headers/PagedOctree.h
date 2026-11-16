/*
 * Cloud of Voxels (CoV) project
 *
 * Paged, immutable octree snapshots, a shared bounded page cache and
 * resumable first-hit ray tracing.  C++17 standard library only; the classic
 * Octree::rayTrace / accelerator path is untouched.
 *
 * Lifecycle
 * ---------
 *  SnapshotReader::open() reads only the fixed header and the page directory.
 *  Node payloads are read lazily by FilePageLoader (or by a user loader).
 *
 *  PageCache owns at most `capacity` resident pages (capacity >= 1).  A ray
 *  asks the cache for a page; duplicate requests for the same page are
 *  coalesced into one loader request.  Completions are identified by a
 *  process-wide unique RequestId, so out-of-order and duplicate deliveries
 *  are harmless.
 *
 *  PagedRay::advance() returns TRACE_NEED_PAGE when the next hierarchy node
 *  it must inspect is not resident.  The ray keeps its own path stack and
 *  parametric interval; after any number of other ray advances, cache
 *  evictions or out-of-order completions it resumes exactly where it stopped
 *  and never re-walks already consumed branches.
 *
 *  PagedFrame drives many rays round-robin through one bounded cache.  It
 *  reports progress while incomplete and only reports completion after every
 *  ray reached a terminal state without a load failure.  cancel() makes all
 *  later completions inert for that frame.
 *
 * Snapshot file format (all integers little-endian, fixed width)
 * --------------------------------------------------------------
 *  Header, 64 bytes:
 *    0  u32 magic "PCVX" (bytes 'P','C','V','X')
 *    4  u16 version (1)
 *    6  u16 headerBytes (64)
 *    8  u32 rootHalf       root cube is [-rootHalf, +rootHalf)
 *   12  u32 maxDepth       root->depth
 *   16  u32 pageRecords    maximum records in one page
 *   20  u32 flags          must be 0
 *   24  u64 nodeCount
 *   32  u64 pageCount
 *   40  u64 directoryBytes (pageCount * 16)
 *   48  u64 reserved       must be 0
 *   56  u32 reserved       must be 0
 *   60  u32 crc32          CRC-32 of bytes [0,60)
 *
 *  Page directory, pageCount entries of 16 bytes:
 *    u64 pageOffset   absolute file offset of the page payload
 *    u32 recordCount  records in this page (<= pageRecords)
 *    u32 reserved     must be 0
 *
 *  Page payload:
 *    u32 payloadBytes (recordCount * 32)
 *    u32 crc32        CRC-32 of the records
 *    recordCount records of 32 bytes:
 *      u32 nodeIndex  absolute node number (root = 0)
 *      u32 childBase  absolute index of child slot 0, or 0xFFFFFFFF
 *      f32 minX       cube minimum corner, x
 *      f32 minY       cube minimum corner, y
 *      f32 minZ       cube minimum corner, z
 *      f32 half       cube half extent (cube is [min, min + 2*half))
 *      u32 color      packed RGBA (0xRRGGBBAA)
 *      u8  flags      bit0 leaf, bit1 has colour
 *      u8  level      remaining depth of this node (0 = leaf level)
 *      u16 reserved   must be 0
 *
 *  Node i lives on page i / pageRecords at position i % pageRecords.
 *  A node is either a leaf, or has exactly eight consecutive children
 *  starting at childBase, in slot order i + 2*j + 4*k.  Each record carries
 *  its own half-open cube geometry, so no geometry has to be reconstructed
 *  from Voxel::coordinates.  Every cube is [min, min + 2*half).
 */

#ifndef PAGED_OCTREE_H
#define PAGED_OCTREE_H

    #include "Octree.h"

    #include <cstddef>
    #include <cstdint>
    #include <deque>
    #include <memory>
    #include <string>
    #include <vector>

    namespace paged
    {
        typedef std::uint32_t PageId;
        typedef std::uint64_t RequestId;
        typedef std::uint32_t NodeIndex;
        typedef std::uint32_t LeafId;

        static const PageId INVALID_PAGE = 0xFFFFFFFFu;
        static const RequestId INVALID_REQUEST = 0xFFFFFFFFFFFFFFFFull;
        static const NodeIndex NO_CHILD = 0xFFFFFFFFu;
        static const std::size_t NODE_RECORD_BYTES = 32;
        static const std::size_t PAGE_PAYLOAD_HEADER_BYTES = 8;
        static const std::size_t SNAPSHOT_HEADER_BYTES = 64;
        static const std::size_t SNAPSHOT_DIRECTORY_ENTRY_BYTES = 16;

        /*
        * One node, copied out of a page.  It deliberately contains no
        * pointer or offset into the page it came from.
        */
        struct NodeRecord
        {
            NodeIndex index;
            NodeIndex childBase;
            float minX, minY, minZ;
            float half;
            std::uint32_t color;
            std::uint8_t flags;
            std::uint8_t level;

            bool isLeaf() const { return (flags & 1u) != 0; }
            bool hasColor() const { return (flags & 2u) != 0; }
        };

        /*
        * A materialised page.  The cache and all users own it by value, so no
        * page-internal address can outlive an eviction.
        */
        struct PageData
        {
            PageId id;
            std::vector<NodeRecord> records;
            bool valid;
            PageData() : id(INVALID_PAGE), valid(false) {}
        };

        /*
        * Loader interface.  request() is called once per distinct page; the
        * loader eventually reports by calling back through poll() with the
        * same request id.  It may complete synchronously inside request().
        */
        class PageLoader
        {
            public:
            virtual ~PageLoader() {}
            virtual bool request(PageId id, RequestId requestId) = 0;
            virtual bool poll(RequestId requestId, PageData* out,
            bool* failed) = 0;
            virtual std::string lastError() const
            {
                return std::string();
            }
        };

        /*
        * Read-only snapshot access.  open() reads the header and the page
        * directory only.
        */
        class SnapshotReader
        {
            public:
            struct Header
            {
                std::uint32_t rootHalf;
                std::uint32_t maxDepth;
                std::uint32_t pageRecords;
                std::uint64_t nodeCount;
                std::uint64_t pageCount;
                std::uint64_t directoryBytes;
                Header() : rootHalf(0), maxDepth(0), pageRecords(0),
                nodeCount(0), pageCount(0), directoryBytes(0) {}
            };

            SnapshotReader();
            ~SnapshotReader();

            bool open(const char* path, std::string* error = NULL);
            bool isOpen() const { return this->fd >= 0; }
            void close();

            const Header& header() const { return this->hdr; }
            PageId pageOf(NodeIndex index) const
            {
                return (PageId)(index / this->hdr.pageRecords);
            }
            NodeIndex slotOf(NodeIndex index) const
            {
                return (NodeIndex)(index % this->hdr.pageRecords);
            }

            /*
            * Read one complete page, validating its size, checksum and node
            * numbering.  Used by loaders and by the all-resident reference.
            */
            bool readPage(PageId id, PageData* out, std::string* error = NULL);

            /*
            * A loader that reads this file synchronously.
            */
            std::unique_ptr<PageLoader> createFileLoader(
            std::string* error = NULL);

            private:

            struct DirEntry
            {
                std::uint64_t offset;
                std::uint32_t count;
            };

            int fd;
            Header hdr;
            std::vector<DirEntry> directory;
            std::string path;

            bool readAt(std::uint64_t offset, void* dst, std::size_t bytes,
            std::string* error);
        };

        /*
        * Statistics, cumulative for the lifetime of a cache except
        * peakResident which is the high-water mark.
        */
        struct CacheStats
        {
            std::uint64_t nodeVisits;
            std::uint64_t pageRequests;
            std::uint64_t pageHits;
            std::uint64_t pageLoads;
            std::uint64_t pageEvictions;
            std::uint64_t peakResident;
            CacheStats() : nodeVisits(0), pageRequests(0), pageHits(0),
            pageLoads(0), pageEvictions(0), peakResident(0) {}
        };

        /*
        * Shared bounded cache.  The number of resident pages never exceeds
        * capacity(); capacity is clamped to at least one.
        */
        class PageCache
        {
            public:
            PageCache(std::size_t capacity,
            std::unique_ptr<PageLoader> loader);
            ~PageCache();

            std::size_t capacity() const { return this->cap; }
            std::size_t resident() const { return this->slots.size(); }
            const CacheStats& stats() const { return this->st; }

            /*
            * Returns a transient pointer to a resident page (LRU-touched) or
            * NULL.  The pointer is valid only until the next cache mutation;
            * callers must copy records out before asking for another page.
            */
            const PageData* residentPage(PageId id);
            bool contains(PageId id) const;

            /*
            * Ask for a page.  When it is already resident, *resident is true.
            * Otherwise *requestId identifies the (possibly coalesced) loader
            * request; the caller must eventually complete() or fail() it.
            * A new loader request increments pageRequests exactly once.
            */
            bool need(PageId id, bool* resident, RequestId* requestId);

            /*
            * Deliver a completion.  Returns false for unknown, duplicate or
            * already failed request ids; in that case nothing changes.
            */
            bool complete(RequestId requestId, const PageData& page);

            /*
            * Report a load failure, releasing every waiter for that request.
            */
            bool fail(RequestId requestId);
            bool wasFailed(RequestId requestId) const;
            bool isPending(RequestId requestId) const;

            /*
            * Outstanding loader request ids, in request order.
            */
            std::vector<RequestId> pendingRequests() const;

            /*
            * Poll the loader for outstanding requests (supports asynchronous
            * loaders).  Synchronous completions are absorbed here too.
            */
            void pump();

            /*
            * Wait table.  Waiters are opaque tokens; no page address is kept.
            */
            bool addWaiter(RequestId requestId, long token);
            bool removeWaiter(RequestId requestId, long token);
            std::size_t waiterCount(RequestId requestId) const;

            /*
            * Forget an outstanding request.  Late completions for it are
            * rejected.  Used when a cancelled frame has no more waiters.
            */
            void revoke(RequestId requestId);
            void revokeIfUnwaited(RequestId requestId);

            void noteNodeVisit() { ++this->st.nodeVisits; }
            void notePageHit() { ++this->st.pageHits; }

            private:
            struct Pending
            {
                RequestId requestId;
                PageId pageId;
                std::vector<long> waiters;
            };

            std::size_t cap;
            std::unique_ptr<PageLoader> loader;
            std::vector<PageData> slots; /* front = least recently used */
            std::vector<Pending> pending;
            std::deque<RequestId> failed;
            CacheStats st;

            bool insert(const PageData& page);
            Pending* findPending(RequestId id);
            void erasePending(RequestId id);
            void noteFailed(RequestId requestId);
        };

        enum TraceStatus
        {
            TRACE_NEED_PAGE = 0,
            TRACE_HIT = 1,
            TRACE_MISS = 2,
            TRACE_ERROR = 3,
            /*
            * Internal: advanceUnit() consumed one hierarchy node and can
            * continue without a page load.  Public advance() never returns
            * this value; it loops until one of the four public states.
            */
            TRACE_RUNNING = 4
        };

        struct RayRequest
        {
            double originX, originY, originZ;
            double dirX, dirY, dirZ;
            double tMin, tMax;
            RayRequest() : originX(0), originY(0), originZ(0),
            dirX(0), dirY(0), dirZ(0), tMin(0), tMax(0) {}
            RayRequest(double ox, double oy, double oz, double dx,
            double dy, double dz, double t0, double t1) :
            originX(ox), originY(oy), originZ(oz), dirX(dx), dirY(dy),
            dirZ(dz), tMin(t0), tMax(t1) {}
        };

        struct RayResult
        {
            TraceStatus status;
            double t;
            LeafId leaf;
            std::uint8_t r, g, b, a;
            RayResult() : status(TRACE_NEED_PAGE), t(0.0), leaf(0),
            r(0), g(0), b(0), a(0) {}
        };

        /*
        * Resumable ray.  The state consists only of values: tree node
        * indices, half-open cube geometry, the remaining parametric interval
        * and a stack of pending siblings.  No page address is retained.
        */
        class PagedRay
        {
            public:
            PagedRay();

            /*
            * Prepare the ray.  Returns false (status Error) for a non-finite
            * or zero direction, non-finite bounds, or tMin > tMax.  The
            * optional maxDepth overrides the header depth (useful for
            * diagnostics); pass 0 to use the header value.
            */
            bool init(const RayRequest& req,
            const SnapshotReader::Header& hdr, long token = 0,
            std::uint32_t maxDepth = 0);

            TraceStatus advance(PageCache* cache, PageId* needPage,
            RayResult* result);

            /*
            * Advance at most one hierarchy node.  Used by PagedFrame so that
            * rays interleave even when the loader completes synchronously.
            * May return TRACE_RUNNING between nodes.
            */
            TraceStatus advanceUnit(PageCache* cache, PageId* needPage,
            RayResult* result);

            /*
            * Report that the page returned by the last advance() could not
            * be loaded.  The ray becomes Error and never treats it as empty.
            */
            void notifyFailure();

            bool isTerminal() const
            {
                return this->trace == TRACE_HIT ||
                this->trace == TRACE_MISS ||
                this->trace == TRACE_ERROR;
            }
            bool hasFailed() const { return this->trace == TRACE_ERROR; }
            TraceStatus status() const { return this->trace; }
            const RayResult& result() const { return this->res; }
            PageId pendingPage() const { return this->needPage; }
            long token() const { return this->rayToken; }

            private:
            struct Frame
            {
                NodeIndex index;
                double tIn;
                double tOut;
            };

            bool valid;
            RayRequest req;
            RayResult res;
            long rayToken;
            TraceStatus trace;

            std::uint32_t rootHalf;
            std::uint32_t maxDepth;
            std::uint32_t pageRecords;

            double tMin;
            double tMax;
            double dir[3];
            double org[3];

            std::vector<Frame> stack;
            PageId needPage;
            bool needPageSet;
            bool notifiedFailure;
            bool inProgress;

            void setError();
            bool cubeInterval(const double origin[3], double half,
            double* tIn, double* tOut) const;
        };

        /*
        * A batch of rays driven through one shared cache.  Rays advance
        * round-robin so that no ray is fully loaded before the others start.
        */
        class PagedFrame
        {
            public:
            /*
            * Uses an externally owned cache; header supplies the snapshot
            * geometry.  The caller keeps both alive.
            */
            PagedFrame(PageCache* cache,
            const SnapshotReader::Header& hdr, std::uint32_t rayCount);

            /*
            * Owns a cache of pageBudget pages fed by the given loader.
            */
            PagedFrame(std::size_t pageBudget,
            std::unique_ptr<PageLoader> loader,
            const SnapshotReader::Header& hdr, std::uint32_t rayCount);

            ~PagedFrame();

            /*
            * Batch entry point: builds a frame with an internal bounded cache
            * (pageBudget, minimum one) and a file loader for the snapshot.
            */
            static std::unique_ptr<PagedFrame> openBatch(
            SnapshotReader* reader, const RayRequest* rays,
            std::size_t rayCount, std::size_t pageBudget,
            std::string* error = NULL);

            bool setRay(std::size_t index, const RayRequest& req);
            bool setRays(const RayRequest* rays, std::size_t rayCount);
            bool start();

            /*
            * Advance the batch.  maxAdvances == 0 means "until no further
            * progress is possible in this call".  Returns isComplete().
            */
            bool step(std::size_t maxAdvances = 0);

            /*
            * Drive the frame until it is done (or no further progress is
            * possible).  Returns isComplete().
            */
            bool run(std::size_t maxRounds = 0);

            bool isComplete() const;   /* every ray terminal and no failure */
            bool isDone() const;       /* every ray terminal, even on failure */
            bool hasFailure() const;
            bool isCancelled() const { return this->cancelled; }

            /* Fraction of rays that reached a terminal state, in [0,1]. */
            double progress() const;

            const RayResult& result(std::size_t index) const
            {
                return this->slots[index].ray.result();
            }
            void results(std::vector<RayResult>* out) const;

            const CacheStats& stats() const { return this->cache->stats(); }
            PageCache* pageCache() { return this->cache; }

            /*
            * Advance one ray by itself once (used by tests to inspect the
            * NeedPage state without letting the frame drive it).
            */
            TraceStatus advanceRay(std::size_t index, PageId* needPage);

            /*
            * Deliver an asynchronous page completion or failure.  Returns
            * true only when the completion was accepted; duplicates and
            * stale ids are rejected.  No ray is advanced by these calls.
            */
            bool deliver(RequestId requestId, const PageData& page);
            std::size_t failRequest(RequestId requestId);

            /*
            * End the frame.  All outstanding requests belonging only to this
            * frame are revoked; late completions cannot fill the cache and
            * can never resume a ray of this frame.
            */
            void cancel();
            void close() { this->cancel(); }

            std::uint64_t generation() const { return this->gen; }

            private:
            struct Slot
            {
                PagedRay ray;
                bool configured;
                bool waiting;
                bool terminal;
                RequestId request;
                PageId page;
                long token;
                Slot() : configured(false), waiting(false), terminal(false),
                request(INVALID_REQUEST), page(INVALID_PAGE), token(0) {}
            };

            PageCache* cache;
            std::unique_ptr<PageCache> ownedCache;
            SnapshotReader::Header hdr;
            std::vector<Slot> slots;
            std::vector<RequestId> issued;
            std::size_t cursor;
            bool started;
            bool cancelled;
            bool failed;
            std::uint64_t gen;
            long nextToken;

            bool bind(std::size_t index);
            void finishSlot(Slot* slot, TraceStatus status);
            bool tokenIssued(RequestId id) const;
            void failSlot(Slot* slot);
        };

        /*
        * All-resident reference walk over the snapshot records.  It never
        * consults a cache and is intended as a deterministic oracle.
        */
        bool referenceTrace(SnapshotReader* reader, const RayRequest& req,
        RayResult* out);
    }

#endif
