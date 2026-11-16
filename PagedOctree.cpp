/*
 * Cloud of Voxels (CoV) project
 *
 * Paged octree: immutable snapshot export, shared bounded page cache and
 * resumable first-hit rays.  C++17 standard library only.
 */

#include "./headers/PagedOctree.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{
    /* snapshot constants */
    static const std::uint32_t SNAPSHOT_MAGIC = 0x58564350u; /* PCVX */
    static const std::uint16_t SNAPSHOT_VERSION = 1;
    static const std::uint8_t NODE_LEAF = 1;
    static const std::uint8_t NODE_COLOR = 2;

    /*
    * Process-wide request id source.  A plain counter is fine for the single
    * threaded cache; ids are never reused, so a stale reply can always be
    * told apart from a live request.
    */
    std::atomic<paged::RequestId> g_requestCounter(0);

    paged::RequestId nextRequestId()
    {
        return g_requestCounter.fetch_add(1) + 1;
    }

    std::uint32_t crc32(const void* data, std::size_t bytes)
    {
        static std::uint32_t table[256];
        static bool ready = false;
        if(!ready)
        {
            for(std::uint32_t i = 0; i < 256; ++i)
            {
                std::uint32_t c = i;
                for(int k = 0; k < 8; ++k)
                {
                    c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                }
                table[i] = c;
            }
            ready = true;
        }
        const unsigned char* p = static_cast<const unsigned char*>(data);
        std::uint32_t c = 0xFFFFFFFFu;
        for(std::size_t i = 0; i < bytes; ++i)
        {
            c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
        }
        return c ^ 0xFFFFFFFFu;
    }

    void putU16(std::vector<unsigned char>* buf, std::uint16_t v)
    {
        buf->push_back((unsigned char)(v & 0xFF));
        buf->push_back((unsigned char)((v >> 8) & 0xFF));
    }

    void putU32(std::vector<unsigned char>* buf, std::uint32_t v)
    {
        for(int i = 0; i < 4; ++i)
        {
            buf->push_back((unsigned char)((v >> (8 * i)) & 0xFF));
        }
    }

    void putU64(std::vector<unsigned char>* buf, std::uint64_t v)
    {
        for(int i = 0; i < 8; ++i)
        {
            buf->push_back((unsigned char)((v >> (8 * i)) & 0xFF));
        }
    }

    void putF32(std::vector<unsigned char>* buf, float v)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        putU32(buf, bits);
    }

    std::uint16_t getU16(const unsigned char* p)
    {
        return (std::uint16_t)((std::uint32_t)p[0] |
        ((std::uint32_t)p[1] << 8));
    }

    std::uint32_t getU32(const unsigned char* p)
    {
        return (std::uint32_t)p[0] | ((std::uint32_t)p[1] << 8) |
        ((std::uint32_t)p[2] << 16) | ((std::uint32_t)p[3] << 24);
    }

    float getF32(const unsigned char* p)
    {
        const std::uint32_t bits = getU32(p);
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    std::uint64_t getU64(const unsigned char* p)
    {
        std::uint64_t v = 0;
        for(int i = 7; i >= 0; --i)
        {
            v = (v << 8) | (std::uint64_t)p[i];
        }
        return v;
    }

    bool writeAll(int fd, const void* src, std::size_t bytes)
    {
        const unsigned char* p = static_cast<const unsigned char*>(src);
        std::size_t done = 0;
        while(done < bytes)
        {
            const ssize_t n = ::write(fd, p + done, bytes - done);
            if(n < 0)
            {
                if(errno == EINTR) continue;
                return false;
            }
            if(n == 0) return false;
            done += (std::size_t)n;
        }
        return true;
    }

    /*
    * Min-heap over parallel ray frames ordered by entry parameter.  The
    * traversal only expands nodes the ray actually intersects, and pops
    * them in increasing t so the first coloured leaf reached is nearest.
    */
    template<typename Frame>
    bool frameLess(const Frame& a, const Frame& b)
    {
        if(a.tIn != b.tIn) return a.tIn < b.tIn;
        return a.index < b.index;
    }

    template<typename Frame>
    void heapPush(std::vector<Frame>* heap, const Frame& v)
    {
        heap->push_back(v);
        std::size_t i = heap->size() - 1;
        while(i > 0)
        {
            const std::size_t parent = (i - 1) / 2;
            if(!frameLess((*heap)[i], (*heap)[parent])) break;
            std::swap((*heap)[i], (*heap)[parent]);
            i = parent;
        }
    }

    template<typename Frame>
    Frame heapPop(std::vector<Frame>* heap)
    {
        const Frame top = (*heap)[0];
        const Frame last = heap->back();
        heap->pop_back();
        if(heap->empty()) return top;
        (*heap)[0] = last;
        std::size_t i = 0;
        for(;;)
        {
            const std::size_t l = i * 2 + 1;
            const std::size_t r = l + 1;
            std::size_t best = i;
            if(l < heap->size() && frameLess((*heap)[l], (*heap)[best]))
            {
                best = l;
            }
            if(r < heap->size() && frameLess((*heap)[r], (*heap)[best]))
            {
                best = r;
            }
            if(best == i) break;
            std::swap((*heap)[i], (*heap)[best]);
            i = best;
        }
        return top;
    }

    /*
    * Flattened export node.
    */
    struct ExportNode
    {
        std::uint32_t index;
        std::uint32_t childBase;
        float min[3];
        float half;
        std::uint32_t color;
        std::uint8_t flags;
        std::uint8_t level;
    };

    /*
    * Write the record for `node` at a pre-allocated index, then - for an
    * internal node - reserve eight contiguous child records and recurse.
    * Children of a node therefore always occupy childBase .. childBase+7.
    */
    void emitNode(const Octant* node, std::uint32_t index,
    const double center[3], double half, std::vector<ExportNode>* out)
    {
        ExportNode& rec = (*out)[index];
        rec.index = index;
        rec.childBase = paged::NO_CHILD;
        rec.flags = 0;
        rec.color = 0;
        rec.level = node->depth;
        rec.half = (float)half;
        rec.min[0] = (float)(center[0] - half);
        rec.min[1] = (float)(center[1] - half);
        rec.min[2] = (float)(center[2] - half);
        if(node->voxel != NULL)
        {
            rec.color = node->voxel->color;
            rec.flags |= NODE_COLOR;
        }

        if(node->children == NULL || node->depth <= 0)
        {
            rec.flags |= NODE_LEAF;
            return;
        }

        const std::uint32_t childBase = (std::uint32_t)out->size();
        rec.childBase = childBase;
        const double childHalf = half * 0.5;
        out->resize(out->size() + 8);
        for(std::uint32_t c = 0; c < 8; ++c)
        {
            const double cc[3] = {
                center[0] + (((c & 1u) != 0) ? childHalf : -childHalf),
                center[1] + (((c & 2u) != 0) ? childHalf : -childHalf),
                center[2] + (((c & 4u) != 0) ? childHalf : -childHalf)};
            emitNode(&node->children[c], childBase + c, cc, childHalf, out);
        }
    }
}

/*
 * ---------------------------------------------------------------------
 * Octree export
 * ---------------------------------------------------------------------
 */
bool Octree::exportPagedSnapshot(const char* path,
std::uint32_t recordsPerPage, std::string* error) const
{
    if(path == NULL)
    {
        if(error) *error = "null snapshot path";
        return false;
    }
    if(this->root == NULL)
    {
        if(error) *error = "octree has no root";
        return false;
    }
    if(recordsPerPage == 0) recordsPerPage = 1;

    std::vector<ExportNode> nodes;
    nodes.resize(1);
    const double rootCenter[3] = {0.0, 0.0, 0.0};
    emitNode(this->root, 0, rootCenter, (double)this->root->half, &nodes);
    if(nodes.size() >= (std::size_t)paged::NO_CHILD)
    {
        if(error) *error = "snapshot has too many nodes";
        return false;
    }

    const std::uint64_t nodeCount = nodes.size();
    const std::uint64_t pageRecords = recordsPerPage;
    const std::uint64_t pageCount =
    (nodeCount + pageRecords - 1) / pageRecords;
    const std::uint64_t directoryBytes =
    pageCount * paged::SNAPSHOT_DIRECTORY_ENTRY_BYTES;

    std::vector<unsigned char> header;
    putU32(&header, SNAPSHOT_MAGIC);
    putU16(&header, SNAPSHOT_VERSION);
    putU16(&header, (std::uint16_t)paged::SNAPSHOT_HEADER_BYTES);
    putU32(&header, (std::uint32_t)this->root->half);
    putU32(&header, (std::uint32_t)this->root->depth);
    putU32(&header, (std::uint32_t)pageRecords);
    putU32(&header, 0); /* flags */
    putU64(&header, nodeCount);
    putU64(&header, pageCount);
    putU64(&header, directoryBytes);
    putU64(&header, 0); /* reserved */
    putU32(&header, 0); /* reserved */
    putU32(&header, crc32(header.data(), header.size()));

    /*
    * Build page payloads first so directory offsets are known.
    */
    std::vector<std::vector<unsigned char> > payloads(
    (std::size_t)pageCount);
    std::vector<std::uint32_t> counts((std::size_t)pageCount, 0);
    for(std::uint64_t p = 0; p < pageCount; ++p)
    {
        const std::uint64_t first = p * pageRecords;
        const std::uint64_t last =
        std::min<std::uint64_t>(nodeCount, first + pageRecords);
        std::vector<unsigned char>& payload = payloads[(std::size_t)p];
        for(std::uint64_t i = first; i < last; ++i)
        {
            const ExportNode& n = nodes[(std::size_t)i];
            putU32(&payload, n.index);
            putU32(&payload, n.childBase);
            putF32(&payload, n.min[0]);
            putF32(&payload, n.min[1]);
            putF32(&payload, n.min[2]);
            putF32(&payload, n.half);
            putU32(&payload, n.color);
            payload.push_back(n.flags);
            payload.push_back(n.level);
            putU16(&payload, 0);
        }
        counts[(std::size_t)p] = (std::uint32_t)(last - first);
    }

    std::uint64_t offset =
    paged::SNAPSHOT_HEADER_BYTES + directoryBytes;
    std::vector<unsigned char> directory;
    for(std::uint64_t p = 0; p < pageCount; ++p)
    {
        putU64(&directory, offset);
        putU32(&directory, counts[(std::size_t)p]);
        putU32(&directory, 0);
        offset += paged::PAGE_PAYLOAD_HEADER_BYTES +
        payloads[(std::size_t)p].size();
    }

    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0)
    {
        if(error) *error = "cannot create snapshot";
        return false;
    }
    bool ok = writeAll(fd, header.data(), header.size());
    if(ok && !directory.empty())
    {
        ok = writeAll(fd, directory.data(), directory.size());
    }
    for(std::uint64_t p = 0; ok && p < pageCount; ++p)
    {
        std::vector<unsigned char> ph;
        putU32(&ph, (std::uint32_t)payloads[(std::size_t)p].size());
        putU32(&ph, crc32(payloads[(std::size_t)p].data(),
        payloads[(std::size_t)p].size()));
        ok = writeAll(fd, ph.data(), ph.size());
        if(ok && !payloads[(std::size_t)p].empty())
        {
            ok = writeAll(fd, payloads[(std::size_t)p].data(),
            payloads[(std::size_t)p].size());
        }
    }
    if(::close(fd) != 0) ok = false;
    if(!ok && error) *error = "snapshot write failed";
    return ok;
}


namespace paged
{
    /*
     * ---------------------------------------------------------------------
     * FilePageLoader (declared in the header's implementation block)
     * ---------------------------------------------------------------------
     */
    class FilePageLoader : public PageLoader
    {
        public:
        FilePageLoader(SnapshotReader* r) : reader(r) {}
        ~FilePageLoader() {}

        bool request(PageId id, RequestId requestId)
        {
            Entry e;
            e.done = true;
            e.failed = false;
            PageData page;
            std::string err;
            if(!this->reader->readPage(id, &page, &err))
            {
                e.failed = true;
                this->error = err;
            } else
            {
                e.page = page;
            }
            this->entries.push_back(std::make_pair(requestId, e));
            return !e.failed;
        }

        bool poll(RequestId requestId, PageData* out, bool* failed)
        {
            for(std::size_t i = 0; i < this->entries.size(); ++i)
            {
                if(this->entries[i].first != requestId) continue;
                Entry& e = this->entries[i].second;
                if(failed != NULL) *failed = e.failed;
                if(!e.failed && out != NULL) *out = e.page;
                this->entries.erase(this->entries.begin() + (long)i);
                return true;
            }
            return false;
        }

        std::string lastError() const { return this->error; }

        private:
        struct Entry
        {
            bool done;
            bool failed;
            PageData page;
        };
        SnapshotReader* reader;
        std::vector<std::pair<RequestId, Entry> > entries;
        std::string error;
    };

    /*
     * ---------------------------------------------------------------------
     * SnapshotReader
     * ---------------------------------------------------------------------
     */
    SnapshotReader::SnapshotReader() : fd(-1) {}

    SnapshotReader::~SnapshotReader()
    {
        this->close();
    }

    void SnapshotReader::close()
    {
        if(this->fd >= 0)
        {
            ::close(this->fd);
            this->fd = -1;
        }
        this->directory.clear();
    }

    bool SnapshotReader::readAt(std::uint64_t offset, void* dst,
    std::size_t bytes, std::string* error)
    {
        if(this->fd < 0)
        {
            if(error) *error = "snapshot is not open";
            return false;
        }
        unsigned char* p = static_cast<unsigned char*>(dst);
        std::size_t done = 0;
        while(done < bytes)
        {
            const ssize_t n = ::pread(this->fd, p + done, bytes - done,
            (off_t)(offset + done));
            if(n < 0)
            {
                if(errno == EINTR) continue;
                if(error) *error = "snapshot read failed";
                return false;
            }
            if(n == 0)
            {
                if(error) *error = "unexpected end of snapshot";
                return false;
            }
            done += (std::size_t)n;
        }
        return true;
    }

    bool SnapshotReader::open(const char* path, std::string* error)
    {
        this->close();
        if(path == NULL)
        {
            if(error) *error = "null snapshot path";
            return false;
        }
        this->fd = ::open(path, O_RDONLY);
        if(this->fd < 0)
        {
            if(error) *error = "cannot open snapshot";
            return false;
        }
        this->path = path;

        unsigned char head[paged::SNAPSHOT_HEADER_BYTES];
        if(!this->readAt(0, head, paged::SNAPSHOT_HEADER_BYTES, error))
        {
            this->close();
            return false;
        }
        if(getU32(head + 0) != SNAPSHOT_MAGIC ||
        getU16(head + 4) != SNAPSHOT_VERSION ||
        getU16(head + 6) != paged::SNAPSHOT_HEADER_BYTES)
        {
            if(error) *error = "bad snapshot header";
            this->close();
            return false;
        }
        if(getU32(head + 60) != crc32(head, 60))
        {
            if(error) *error = "snapshot header checksum mismatch";
            this->close();
            return false;
        }
        if(getU32(head + 20) != 0 || getU64(head + 48) != 0 ||
        getU32(head + 56) != 0)
        {
            if(error) *error = "unsupported snapshot flags";
            this->close();
            return false;
        }

        this->hdr.rootHalf = getU32(head + 8);
        this->hdr.maxDepth = getU32(head + 12);
        this->hdr.pageRecords = getU32(head + 16);
        this->hdr.nodeCount = getU64(head + 24);
        this->hdr.pageCount = getU64(head + 32);
        this->hdr.directoryBytes = getU64(head + 40);

        if(this->hdr.rootHalf == 0 || this->hdr.pageRecords == 0 ||
        this->hdr.maxDepth > 32 ||
        this->hdr.pageCount > (std::uint64_t)NO_CHILD ||
        this->hdr.directoryBytes !=
        this->hdr.pageCount * paged::SNAPSHOT_DIRECTORY_ENTRY_BYTES ||
        this->hdr.pageCount !=
        (this->hdr.nodeCount + this->hdr.pageRecords - 1) /
        this->hdr.pageRecords)
        {
            if(error) *error = "invalid snapshot geometry";
            this->close();
            return false;
        }

        /*
        * Only the directory is read here; pages stay on disk.
        */
        std::vector<unsigned char> dir((std::size_t)this->hdr.directoryBytes);
        if(!dir.empty() &&
        !this->readAt(paged::SNAPSHOT_HEADER_BYTES, dir.data(), dir.size(), error))
        {
            if(error) *error = "cannot read page directory";
            this->close();
            return false;
        }
        this->directory.resize((std::size_t)this->hdr.pageCount);
        for(std::size_t i = 0; i < this->directory.size(); ++i)
        {
            const unsigned char* e =
            dir.data() + i * paged::SNAPSHOT_DIRECTORY_ENTRY_BYTES;
            this->directory[i].offset = getU64(e);
            this->directory[i].count = getU32(e + 8);
            if(getU32(e + 12) != 0 ||
            this->directory[i].count > this->hdr.pageRecords)
            {
                if(error) *error = "invalid page directory entry";
                this->close();
                return false;
            }
        }
        return true;
    }

    bool SnapshotReader::readPage(PageId id, PageData* out,
    std::string* error)
    {
        if(!this->isOpen())
        {
            if(error) *error = "snapshot is not open";
            return false;
        }
        if(id >= this->directory.size())
        {
            if(error) *error = "page id out of range";
            return false;
        }
        const DirEntry& de = this->directory[id];
        unsigned char ph[paged::PAGE_PAYLOAD_HEADER_BYTES];
        if(!this->readAt(de.offset, ph, paged::PAGE_PAYLOAD_HEADER_BYTES,
        error))
        {
            return false;
        }
        const std::uint32_t bytes = getU32(ph + 0);
        const std::uint32_t sum = getU32(ph + 4);
        const std::uint64_t expected =
        (std::uint64_t)de.count * paged::NODE_RECORD_BYTES;
        if((std::uint64_t)bytes != expected)
        {
            if(error) *error = "page size does not match directory";
            return false;
        }
        std::vector<unsigned char> raw(bytes);
        if(bytes > 0 && !this->readAt(de.offset +
        paged::PAGE_PAYLOAD_HEADER_BYTES, raw.data(), raw.size(), error))
        {
            return false;
        }
        if(crc32(raw.data(), raw.size()) != sum)
        {
            if(error) *error = "page checksum mismatch";
            return false;
        }

        out->id = id;
        out->valid = true;
        out->records.resize(de.count);
        for(std::size_t i = 0; i < de.count; ++i)
        {
            const unsigned char* r = raw.data() + i * paged::NODE_RECORD_BYTES;
            NodeRecord& n = out->records[i];
            n.index = getU32(r + 0);
            n.childBase = getU32(r + 4);
            n.minX = getF32(r + 8);
            n.minY = getF32(r + 12);
            n.minZ = getF32(r + 16);
            n.half = getF32(r + 20);
            n.color = getU32(r + 24);
            n.flags = r[28];
            n.level = r[29];
            if(getU16(r + 30) != 0)
            {
                if(error) *error = "invalid node record padding";
                out->valid = false;
                return false;
            }
            if(!(n.half > 0.0f))
            {
                if(error) *error = "invalid node extent";
                out->valid = false;
                return false;
            }
            if(n.index != id * this->hdr.pageRecords +
            (std::uint32_t)i)
            {
                if(error) *error = "page node numbering mismatch";
                out->valid = false;
                return false;
            }
        }
        return true;
    }

    std::unique_ptr<PageLoader> SnapshotReader::createFileLoader(
    std::string* error)
    {
        if(!this->isOpen())
        {
            if(error) *error = "snapshot is not open";
            return std::unique_ptr<PageLoader>();
        }
        return std::unique_ptr<PageLoader>(new FilePageLoader(this));
    }

    /*
     * ---------------------------------------------------------------------
     * PageCache
     * ---------------------------------------------------------------------
     */
    PageCache::PageCache(std::size_t capacity,
    std::unique_ptr<PageLoader> l) : cap(capacity), loader(std::move(l))
    {
        if(this->cap == 0) this->cap = 1;
    }

    PageCache::~PageCache() {}

    const PageData* PageCache::residentPage(PageId id)
    {
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            if(this->slots[i].id != id || !this->slots[i].valid) continue;
            if(i != 0)
            {
                PageData page = this->slots[i];
                this->slots.erase(this->slots.begin() + (long)i);
                this->slots.insert(this->slots.begin(), page);
            }
            return &this->slots[0];
        }
        return NULL;
    }

    bool PageCache::contains(PageId id) const
    {
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            if(this->slots[i].id == id && this->slots[i].valid) return true;
        }
        return false;
    }

    bool PageCache::insert(const PageData& page)
    {
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            if(this->slots[i].id == page.id)
            {
                this->slots[i] = page;
                return true;
            }
        }
        if(this->slots.size() >= this->cap)
        {
            this->slots.erase(this->slots.begin());
            ++this->st.pageEvictions;
        }
        this->slots.push_back(page);
        if(this->slots.size() > this->st.peakResident)
        {
            this->st.peakResident = this->slots.size();
        }
        ++this->st.pageLoads;
        return true;
    }

    PageCache::Pending* PageCache::findPending(RequestId id)
    {
        for(std::size_t i = 0; i < this->pending.size(); ++i)
        {
            if(this->pending[i].requestId == id) return &this->pending[i];
        }
        return NULL;
    }

    void PageCache::erasePending(RequestId id)
    {
        for(std::size_t i = 0; i < this->pending.size(); ++i)
        {
            if(this->pending[i].requestId == id)
            {
                this->pending.erase(this->pending.begin() + (long)i);
                return;
            }
        }
    }

    bool PageCache::need(PageId id, bool* resident, RequestId* requestId)
    {
        *resident = false;
        *requestId = INVALID_REQUEST;
        if(this->contains(id))
        {
            *resident = true;
            ++this->st.pageHits;
            return true;
        }
        for(std::size_t i = 0; i < this->pending.size(); ++i)
        {
            if(this->pending[i].pageId == id)
            {
                *requestId = this->pending[i].requestId;
                return true;
            }
        }
        Pending pr;
        pr.requestId = nextRequestId();
        pr.pageId = id;
        this->pending.push_back(pr);
        *requestId = pr.requestId;
        ++this->st.pageRequests;
        if(this->loader.get() != NULL)
        {
            this->loader->request(id, pr.requestId);
        }
        return true;
    }

    bool PageCache::complete(RequestId requestId, const PageData& page)
    {
        Pending* pr = this->findPending(requestId);
        if(pr == NULL) return false;
        if(!page.valid || page.id != pr->pageId) return false;
        this->insert(page);
        this->erasePending(requestId);
        return true;
    }

    bool PageCache::fail(RequestId requestId)
    {
        if(this->findPending(requestId) == NULL) return false;
        this->erasePending(requestId);
        this->noteFailed(requestId);
        return true;
    }

    void PageCache::noteFailed(RequestId requestId)
    {
        this->failed.push_back(requestId);
        /*
        * Request ids are process-wide unique, so only recent failures can
        * ever be mistaken for a live request.  Keeping a bounded recent
        * window avoids unbounded growth in long sessions.
        */
        while(this->failed.size() > 4096) this->failed.pop_front();
    }

    bool PageCache::wasFailed(RequestId requestId) const
    {
        for(std::size_t i = 0; i < this->failed.size(); ++i)
        {
            if(this->failed[i] == requestId) return true;
        }
        return false;
    }

    bool PageCache::isPending(RequestId requestId) const
    {
        for(std::size_t i = 0; i < this->pending.size(); ++i)
        {
            if(this->pending[i].requestId == requestId) return true;
        }
        return false;
    }

    std::vector<RequestId> PageCache::pendingRequests() const
    {
        std::vector<RequestId> ids;
        for(std::size_t i = 0; i < this->pending.size(); ++i)
        {
            ids.push_back(this->pending[i].requestId);
        }
        return ids;
    }

    void PageCache::pump()
    {
        if(this->loader.get() == NULL) return;
        /*
        * Absorb synchronous completions.  A bounded number of rounds keeps
        * an unexpectedly chatty loader from spinning forever.
        */
        for(int round = 0; round < 4; ++round)
        {
            bool any = false;
            std::vector<RequestId> ids = this->pendingRequests();
            for(std::size_t i = 0; i < ids.size(); ++i)
            {
                PageData page;
                bool failed = false;
                if(!this->loader->poll(ids[i], &page, &failed)) continue;
                any = true;
                if(failed)
                {
                    this->fail(ids[i]);
                } else
                {
                    this->complete(ids[i], page);
                }
            }
            if(!any) break;
        }
    }

    bool PageCache::addWaiter(RequestId requestId, long token)
    {
        Pending* pr = this->findPending(requestId);
        if(pr == NULL) return false;
        pr->waiters.push_back(token);
        return true;
    }

    std::size_t PageCache::waiterCount(RequestId requestId) const
    {
        for(std::size_t i = 0; i < this->pending.size(); ++i)
        {
            if(this->pending[i].requestId == requestId)
            {
                return this->pending[i].waiters.size();
            }
        }
        return 0;
    }

    bool PageCache::removeWaiter(RequestId requestId, long token)
    {
        Pending* pr = this->findPending(requestId);
        if(pr == NULL) return false;
        for(std::size_t i = 0; i < pr->waiters.size(); ++i)
        {
            if(pr->waiters[i] == token)
            {
                pr->waiters.erase(pr->waiters.begin() + (long)i);
                return true;
            }
        }
        return false;
    }

    void PageCache::revoke(RequestId requestId)
    {
        this->erasePending(requestId);
        this->noteFailed(requestId);
    }

    void PageCache::revokeIfUnwaited(RequestId requestId)
    {
        if(this->waiterCount(requestId) != 0) return;
        this->revoke(requestId);
    }

    /*
     * ---------------------------------------------------------------------
     * PagedRay
     * ---------------------------------------------------------------------
     */
    PagedRay::PagedRay() : valid(false), rayToken(0), trace(TRACE_ERROR),
    rootHalf(0), maxDepth(0), pageRecords(0), tMin(0.0), tMax(0.0),
    needPage(INVALID_PAGE), needPageSet(false), notifiedFailure(false),
    inProgress(false)
    {
        this->dir[0] = this->dir[1] = this->dir[2] = 0.0;
        this->org[0] = this->org[1] = this->org[2] = 0.0;
        this->res.status = TRACE_ERROR;
    }

    void PagedRay::setError()
    {
        this->trace = TRACE_ERROR;
        this->res.status = TRACE_ERROR;
        this->res.t = 0.0;
        this->res.leaf = 0;
        this->stack.clear();
        this->needPage = INVALID_PAGE;
        this->needPageSet = false;
    }

    bool PagedRay::init(const RayRequest& req,
    const SnapshotReader::Header& hdr, long tok, std::uint32_t overrideDepth)
    {
        this->req = req;
        this->rayToken = tok;
        this->valid = true;
        this->rootHalf = hdr.rootHalf;
        this->maxDepth = overrideDepth ? overrideDepth : hdr.maxDepth;
        this->pageRecords = hdr.pageRecords;
        this->tMin = req.tMin;
        this->tMax = req.tMax;
        this->dir[0] = req.dirX;
        this->dir[1] = req.dirY;
        this->dir[2] = req.dirZ;
        this->org[0] = req.originX;
        this->org[1] = req.originY;
        this->org[2] = req.originZ;
        this->stack.clear();
        this->needPage = INVALID_PAGE;
        this->needPageSet = false;
        this->notifiedFailure = false;
        this->inProgress = false;
        this->res = RayResult();

        const double len2 = this->dir[0] * this->dir[0] +
        this->dir[1] * this->dir[1] + this->dir[2] * this->dir[2];
        const bool finite = std::isfinite(this->org[0]) &&
        std::isfinite(this->org[1]) && std::isfinite(this->org[2]) &&
        std::isfinite(this->dir[0]) && std::isfinite(this->dir[1]) &&
        std::isfinite(this->dir[2]) && std::isfinite(this->tMin) &&
        std::isfinite(this->tMax);
        if(!finite || !(len2 > 0.0) || this->tMin > this->tMax)
        {
            this->setError();
            return false;
        }

        /*
        * Clip against the root grid first.  The exact root cube is in node
        * record 0, but the header gives the same half extent, avoiding a page
        * load just to reject an outside ray.
        */
        const double rootMin[3] = {-(double)this->rootHalf,
        -(double)this->rootHalf, -(double)this->rootHalf};
        double tIn = 0.0, tOut = 0.0;
        if(!this->cubeInterval(rootMin, (double)this->rootHalf, &tIn, &tOut))
        {
            this->trace = TRACE_MISS;
            this->res.status = TRACE_MISS;
            this->res.t = 0.0;
            this->res.leaf = 0;
            return true;
        }
        Frame root;
        root.index = 0;
        root.tIn = tIn;
        root.tOut = tOut;
        this->stack.push_back(root);
        this->trace = TRACE_NEED_PAGE;
        this->res.status = TRACE_NEED_PAGE;
        return true;
    }

    /*
    * Half-open slab test: the interval [tIn,tOut] intersected with the cube
    * [origin, origin+2*half).  The exit at the maximum face is exclusive,
    * which gives unique ownership of shared faces/edges.  A pure touch
    * (zero-length interval) is rejected.
    */
    bool PagedRay::cubeInterval(const double origin[3], double half,
    double* tIn, double* tOut) const
    {
        double lo = this->tMin;
        double hi = this->tMax;
        const double maxv[3] = {origin[0] + 2.0 * half,
        origin[1] + 2.0 * half, origin[2] + 2.0 * half};
        for(int a = 0; a < 3; ++a)
        {
            const double o = this->org[a];
            const double d = this->dir[a];
            if(d == 0.0)
            {
                if(o < origin[a] || o >= maxv[a]) return false;
                continue;
            }
            const double inv = 1.0 / d;
            double t0 = (origin[a] - o) * inv;
            double t1 = (maxv[a] - o) * inv;
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
        if(!(hi > lo)) return false; /* pure touch or empty */
        *tIn = lo;
        *tOut = hi;
        return true;
    }

    TraceStatus PagedRay::advanceUnit(PageCache* cache, PageId* needPageOut,
    RayResult* result)
    {
        if(!this->valid || this->notifiedFailure || cache == NULL)
        {
            this->setError();
            if(result) *result = this->res;
            return this->trace;
        }
        if(this->isTerminal())
        {
            if(result) *result = this->res;
            return this->trace;
        }
        if(this->needPageSet)
        {
            /*
            * The page reported by the previous call is still required.  If it
            * has become resident, continue from the retained heap.  If not,
            * report the same page again: a missing page is never an empty
            * voxel and the ray is never restarted from the root.
            */
            if(!cache->contains(this->needPage))
            {
                if(needPageOut) *needPageOut = this->needPage;
                if(result) *result = this->res;
                return TRACE_NEED_PAGE;
            }
            this->needPage = INVALID_PAGE;
            this->needPageSet = false;
            cache->notePageHit();
        }
        if(this->stack.empty())
        {
            this->trace = TRACE_MISS;
            this->res.status = TRACE_MISS;
            this->res.t = 0.0;
            this->res.leaf = 0;
            this->needPage = INVALID_PAGE;
            this->needPageSet = false;
            this->inProgress = false;
            if(result) *result = this->res;
            return this->trace;
        }

        const Frame frame = heapPop(&this->stack);
        const PageId pid = (PageId)(frame.index / this->pageRecords);
        const PageData* page = cache->residentPage(pid);
        if(page == NULL)
        {
            heapPush(&this->stack, frame);
            this->needPage = pid;
            this->needPageSet = true;
            if(needPageOut) *needPageOut = this->needPage;
            if(result) *result = this->res;
            return TRACE_NEED_PAGE;
        }
        const NodeIndex off = frame.index % this->pageRecords;
        if(off >= page->records.size())
        {
            this->setError();
            if(result) *result = this->res;
            return this->trace;
        }
        /*
        * Copy the record out.  From here on the ray holds no pointer into
        * the page, so an eviction cannot invalidate anything.
        */
        const NodeRecord node = page->records[off];
        cache->noteNodeVisit();

        if(node.isLeaf())
        {
            if(node.hasColor())
            {
                this->res.status = TRACE_HIT;
                this->res.t = frame.tIn;
                this->res.leaf = frame.index;
                this->res.r = (std::uint8_t)((node.color >> 24) & 0xFF);
                this->res.g = (std::uint8_t)((node.color >> 16) & 0xFF);
                this->res.b = (std::uint8_t)((node.color >> 8) & 0xFF);
                this->res.a = (std::uint8_t)(node.color & 0xFF);
                this->trace = TRACE_HIT;
                this->needPage = INVALID_PAGE;
                this->needPageSet = false;
                this->inProgress = false;
                if(result) *result = this->res;
                return this->trace;
            }
        } else if(node.childBase != NO_CHILD)
        {
            /*
            * Push intersecting children.  Their cubes are computed from the
            * parent cube plus the slot offset; the child record itself is
            * read when it is popped.
            */
            const double origin[3] = {(double)node.minX, (double)node.minY,
            (double)node.minZ};
            const double h = (double)node.half * 0.5;
            for(int s = 0; s < 8; ++s)
            {
                double childMin[3];
                childMin[0] = origin[0] + (((s & 1) != 0) ? node.half : 0.0);
                childMin[1] = origin[1] + (((s & 2) != 0) ? node.half : 0.0);
                childMin[2] = origin[2] + (((s & 4) != 0) ? node.half : 0.0);
                double tIn = 0.0, tOut = 0.0;
                if(!this->cubeInterval(childMin, h, &tIn, &tOut))
                {
                    continue;
                }
                Frame child;
                child.index = node.childBase + (NodeIndex)s;
                child.tIn = tIn;
                child.tOut = tOut;
                heapPush(&this->stack, child);
            }
        }
        if(this->stack.empty())
        {
            this->trace = TRACE_MISS;
            this->res.status = TRACE_MISS;
            this->res.t = 0.0;
            this->res.leaf = 0;
            this->inProgress = false;
            if(result) *result = this->res;
            return this->trace;
        }
        /*
        * Still in progress: the public result stays "needs more work" so a
        * caller polling result() mid-frame never sees an internal state.
        */
        this->trace = TRACE_RUNNING;
        this->res.status = TRACE_NEED_PAGE;
        this->inProgress = true;
        if(result) *result = this->res;
        return TRACE_RUNNING;
    }

    TraceStatus PagedRay::advance(PageCache* cache, PageId* needPageOut,
    RayResult* result)
    {
        /*
        * Public entry point: loop over units until the ray needs a page or
        * reaches a terminal state.  The unit limit keeps a malformed tree
        * from spinning forever without making progress.
        */
        this->inProgress = false;
        for(int guard = 0; guard < 10000000; ++guard)
        {
            const TraceStatus st = this->advanceUnit(cache, needPageOut,
            result);
            if(st != TRACE_RUNNING)
            {
                if(result) *result = this->res;
                return st;
            }
        }
        this->setError();
        if(result) *result = this->res;
        return this->trace;
    }

    void PagedRay::notifyFailure()
    {
        this->notifiedFailure = true;
        this->inProgress = false;
        this->setError();
    }

    /*
     * ---------------------------------------------------------------------
     * Reference trace
     * ---------------------------------------------------------------------
     */
    bool referenceTrace(SnapshotReader* reader, const RayRequest& req,
    RayResult* out)
    {
        if(reader == NULL || !reader->isOpen()) return false;
        struct RefFrame
        {
            NodeIndex index;
            double tIn;
            double tOut;
        };
        const double dir[3] = {req.dirX, req.dirY, req.dirZ};
        const double org[3] = {req.originX, req.originY, req.originZ};
        const double len2 = dir[0] * dir[0] + dir[1] * dir[1] +
        dir[2] * dir[2];
        const bool finite = std::isfinite(org[0]) && std::isfinite(org[1]) &&
        std::isfinite(org[2]) && std::isfinite(dir[0]) &&
        std::isfinite(dir[1]) && std::isfinite(dir[2]) &&
        std::isfinite(req.tMin) && std::isfinite(req.tMax);
        if(!finite || !(len2 > 0.0) || req.tMin > req.tMax)
        {
            if(out) *out = RayResult();
            return false;
        }

        auto interval = [&](double minX, double minY, double minZ, double half,
        double* tIn, double* tOut) -> bool
        {
            const double mn[3] = {minX, minY, minZ};
            const double mx[3] = {minX + 2.0 * half, minY + 2.0 * half,
            minZ + 2.0 * half};
            double lo = req.tMin;
            double hi = req.tMax;
            for(int a = 0; a < 3; ++a)
            {
                const double d = dir[a];
                if(d == 0.0)
                {
                    if(org[a] < mn[a] || org[a] >= mx[a]) return false;
                    continue;
                }
                const double inv = 1.0 / d;
                double t0 = (mn[a] - org[a]) * inv;
                double t1 = (mx[a] - org[a]) * inv;
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
            if(!(hi > lo)) return false;
            *tIn = lo;
            *tOut = hi;
            return true;
        };

        const SnapshotReader::Header& hdr = reader->header();
        std::vector<RefFrame> stack;
        /*
        * Clip the root grid from the header so an outside ray needs no page.
        */
        double rootTIn = 0.0, rootTOut = 0.0;
        if(!interval(-(double)hdr.rootHalf, -(double)hdr.rootHalf,
        -(double)hdr.rootHalf, (double)hdr.rootHalf, &rootTIn, &rootTOut))
        {
            if(out)
            {
                *out = RayResult();
                out->status = TRACE_MISS;
            }
            return true;
        }
        RefFrame root;
        root.index = 0;
        root.tIn = rootTIn;
        root.tOut = rootTOut;
        stack.push_back(root);

        PageData current;
        PageId currentId = INVALID_PAGE;

        while(!stack.empty())
        {
            const RefFrame frame = heapPop(&stack);
            const PageId pid = (PageId)(frame.index / hdr.pageRecords);
            if(pid != currentId)
            {
                PageData page;
                if(!reader->readPage(pid, &page, NULL)) return false;
                current = page;
                currentId = pid;
            }
            const NodeIndex off = frame.index % hdr.pageRecords;
            if(off >= current.records.size()) return false;
            const NodeRecord node = current.records[off];
            if(node.isLeaf())
            {
                if(node.hasColor() && frame.tIn < frame.tOut)
                {
                    RayResult r;
                    r.status = TRACE_HIT;
                    r.t = frame.tIn;
                    r.leaf = frame.index;
                    r.r = (std::uint8_t)((node.color >> 24) & 0xFF);
                    r.g = (std::uint8_t)((node.color >> 16) & 0xFF);
                    r.b = (std::uint8_t)((node.color >> 8) & 0xFF);
                    r.a = (std::uint8_t)(node.color & 0xFF);
                    if(out) *out = r;
                    return true;
                }
                continue;
            }
            if(node.childBase == NO_CHILD) continue;
            const double h = (double)node.half * 0.5;
            const double ox = (double)node.minX;
            const double oy = (double)node.minY;
            const double oz = (double)node.minZ;
            for(int s = 0; s < 8; ++s)
            {
                double tIn = 0.0, tOut = 0.0;
                if(!interval(ox + ((s & 1) ? node.half : 0.0),
                oy + ((s & 2) ? node.half : 0.0),
                oz + ((s & 4) ? node.half : 0.0), h, &tIn, &tOut))
                {
                    continue;
                }
                RefFrame child;
                child.index = node.childBase + (NodeIndex)s;
                child.tIn = tIn;
                child.tOut = tOut;
                heapPush(&stack, child);
            }
        }
        if(out)
        {
            *out = RayResult();
            out->status = TRACE_MISS;
        }
        return true;
    }

    /*
     * ---------------------------------------------------------------------
     * PagedFrame
     * ---------------------------------------------------------------------
     */
    PagedFrame::PagedFrame(PageCache* c, const SnapshotReader::Header& h,
    std::uint32_t rayCount) : cache(c), hdr(h), cursor(0), started(false),
    cancelled(false), failed(false), gen(0), nextToken(1)
    {
        this->slots.resize(rayCount);
    }

    PagedFrame::PagedFrame(std::size_t pageBudget,
    std::unique_ptr<PageLoader> loader, const SnapshotReader::Header& h,
    std::uint32_t rayCount) : cache(NULL), hdr(h), cursor(0), started(false),
    cancelled(false), failed(false), gen(0), nextToken(1)
    {
        this->ownedCache.reset(new PageCache(pageBudget, std::move(loader)));
        this->cache = this->ownedCache.get();
        this->slots.resize(rayCount);
    }

    PagedFrame::~PagedFrame()
    {
        /*
        * A frame that goes away while requests are outstanding must revoke
        * them so that a late completion cannot touch its rays.
        */
        if(!this->cancelled) this->cancel();
    }

    std::unique_ptr<PagedFrame> PagedFrame::openBatch(SnapshotReader* reader,
    const RayRequest* rays, std::size_t rayCount, std::size_t pageBudget,
    std::string* error)
    {
        if(reader == NULL || !reader->isOpen())
        {
            if(error) *error = "snapshot is not open";
            return std::unique_ptr<PagedFrame>();
        }
        std::unique_ptr<PageLoader> loader = reader->createFileLoader(error);
        if(!loader.get())
        {
            return std::unique_ptr<PagedFrame>();
        }
        std::unique_ptr<PagedFrame> frame(new PagedFrame(pageBudget,
        std::move(loader), reader->header(), (std::uint32_t)rayCount));
        if(rays != NULL && !frame->setRays(rays, rayCount))
        {
            if(error) *error = "invalid ray array";
            return std::unique_ptr<PagedFrame>();
        }
        return frame;
    }

    bool PagedFrame::setRay(std::size_t index, const RayRequest& req)
    {
        if(index >= this->slots.size() || this->started) return false;
        Slot& slot = this->slots[index];
        if(slot.token == 0) slot.token = this->nextToken++;
        slot.ray = PagedRay();
        slot.ray.init(req, this->hdr, slot.token);
        slot.configured = true;
        slot.terminal = false;
        slot.waiting = false;
        slot.request = INVALID_REQUEST;
        slot.page = INVALID_PAGE;
        return true;
    }

    bool PagedFrame::setRays(const RayRequest* rays, std::size_t rayCount)
    {
        if(this->started || rays == NULL) return false;
        if(rayCount != this->slots.size()) return false;
        for(std::size_t i = 0; i < rayCount; ++i)
        {
            if(!this->setRay(i, rays[i])) return false;
        }
        return true;
    }

    bool PagedFrame::bind(std::size_t index)
    {
        Slot& slot = this->slots[index];
        if(slot.token == 0) slot.token = this->nextToken++;
        return true;
    }

    bool PagedFrame::start()
    {
        if(this->started) return !this->cancelled;
        if(this->cancelled) return false;
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            this->bind(i);
        }
        this->started = true;
        ++this->gen;
        return true;
    }

    bool PagedFrame::tokenIssued(RequestId id) const
    {
        for(std::size_t i = 0; i < this->issued.size(); ++i)
        {
            if(this->issued[i] == id) return true;
        }
        return false;
    }

    namespace
    {
        void pruneIssued(std::vector<RequestId>* issued, RequestId id)
        {
            for(std::size_t i = 0; i < issued->size(); ++i)
            {
                if((*issued)[i] != id) continue;
                issued->erase(issued->begin() + (long)i);
                return;
            }
        }
    }

    void PagedFrame::finishSlot(Slot* slot, TraceStatus status)
    {
        slot->terminal = true;
        slot->waiting = false;
        slot->request = INVALID_REQUEST;
        slot->page = INVALID_PAGE;
        (void)status;
    }

    void PagedFrame::failSlot(Slot* slot)
    {
        slot->ray.notifyFailure();
        slot->terminal = true;
        slot->waiting = false;
        slot->request = INVALID_REQUEST;
        slot->page = INVALID_PAGE;
        this->failed = true;
    }

    TraceStatus PagedFrame::advanceRay(std::size_t index, PageId* needPage)
    {
        if(index >= this->slots.size()) return TRACE_ERROR;
        Slot& slot = this->slots[index];
        if(slot.terminal) return slot.ray.status();
        if(slot.waiting)
        {
            /*
            * A waiting ray resumes only once its page is resident.  A failed
            * request becomes a terminal error, never an empty node.
            */
            if(this->cache->contains(slot.page))
            {
                slot.waiting = false;
                slot.request = INVALID_REQUEST;
                slot.page = INVALID_PAGE;
            } else if(slot.request != INVALID_REQUEST &&
            this->cache->wasFailed(slot.request))
            {
                this->failSlot(&slot);
                if(needPage) *needPage = slot.page;
                return TRACE_ERROR;
            } else if(slot.request == INVALID_REQUEST ||
            !this->cache->isPending(slot.request))
            {
                /*
                * The request already settled but its page has since been
                * evicted before this ray could consume it.  Drop the stale
                * wait and re-request from the ray's retained state; it still
                * never restarts from the root.
                */
                slot.waiting = false;
                slot.request = INVALID_REQUEST;
                slot.page = INVALID_PAGE;
            } else
            {
                if(needPage) *needPage = slot.page;
                return TRACE_NEED_PAGE;
            }
        }

        /*
        * Consume exactly one hierarchy node per turn whenever possible.
        * If the next node lives on a page that must be loaded, and the
        * loader completes it synchronously, consume it immediately in this
        * same turn (so another ray cannot evict it in between) and then
        * yield.  This keeps multiple rays interleaving page by page instead
        * of draining one ray to completion before the next starts.
        */
        for(int spin = 0; spin < 8; ++spin)
        {
            PageId page = INVALID_PAGE;
            RayResult result;
            const TraceStatus st = slot.ray.advanceUnit(this->cache, &page,
            &result);
            if(st == TRACE_RUNNING)
            {
                /* One hierarchy node consumed; yield to the other rays. */
                if(needPage) *needPage = page;
                return TRACE_RUNNING;
            }
            if(st != TRACE_NEED_PAGE)
            {
                this->finishSlot(&slot, st);
                if(st == TRACE_ERROR) this->failed = true;
                if(needPage) *needPage = page;
                return st;
            }

            slot.page = page;
            bool resident = false;
            RequestId req = INVALID_REQUEST;
            this->cache->need(page, &resident, &req);
            this->cache->pump();
            if(this->cache->contains(page))
            {
                /*
                * Resident (or synchronously completed): consume it now, then
                * yield after the node is visited.
                */
                slot.waiting = false;
                slot.request = INVALID_REQUEST;
                slot.page = INVALID_PAGE;
                PageId again = page;
                const TraceStatus consumed =
                slot.ray.advanceUnit(this->cache, &again, &result);
                if(consumed == TRACE_ERROR || consumed == TRACE_HIT ||
                consumed == TRACE_MISS)
                {
                    this->finishSlot(&slot, consumed);
                    if(consumed == TRACE_ERROR) this->failed = true;
                    if(needPage) *needPage = again;
                    return consumed;
                }
                if(needPage) *needPage = again;
                if(consumed == TRACE_RUNNING) return TRACE_RUNNING;
                return TRACE_NEED_PAGE;
            }
            if(req == INVALID_REQUEST)
            {
                slot.waiting = false;
                continue;
            }
            if(this->cache->wasFailed(req))
            {
                this->failSlot(&slot);
                if(needPage) *needPage = page;
                return TRACE_ERROR;
            }
            slot.request = req;
            slot.waiting = true;
            if(!this->tokenIssued(req)) this->issued.push_back(req);
            /*
            * Register the wait token once per distinct request.  A waiting
            * slot does not re-enter this branch for the same request.
            */
            this->cache->addWaiter(req, slot.token);
            if(needPage) *needPage = page;
            return TRACE_NEED_PAGE;
        }
        if(needPage) *needPage = slot.page;
        return TRACE_NEED_PAGE;
    }

    bool PagedFrame::step(std::size_t maxAdvances)
    {
        if(this->cancelled) return false;
        if(!this->started) this->start();
        this->cache->pump();

        std::size_t advanced = 0;
        const std::size_t n = this->slots.size();
        if(n == 0) return false;
        /*
        * Round-robin over the rays.  Each pass gives every runnable ray one
        * advance before any ray gets a second one, so no single ray is
        * loaded entirely before the others start.  Ray-level waiting is
        * resolved inside advanceRay; a ray already blocked on an outstanding
        * request is skipped, so a pass with nothing runnable terminates.
        */
        for(;;)
        {
            std::size_t attempted = 0;
            for(std::size_t k = 0; k < n; ++k)
            {
                const std::size_t i = this->cursor % n;
                this->cursor = (this->cursor + 1) % n;
                Slot& slot = this->slots[i];
                if(slot.terminal || !slot.configured || slot.waiting) continue;
                ++attempted;
                ++advanced;
                this->advanceRay(i, NULL);
                if(maxAdvances != 0 && advanced >= maxAdvances)
                {
                    this->cache->pump();
                    break;
                }
            }
            this->cache->pump();
            if(attempted == 0) break;
            if(this->isComplete()) return true;
            if(maxAdvances != 0 && advanced >= maxAdvances) break;
            /*
            * If every non-terminal ray is blocked on an outstanding request,
            * stop; an external driver must deliver completions.
            */
            bool blocked = true;
            for(std::size_t i = 0; i < n; ++i)
            {
                const Slot& slot = this->slots[i];
                if(slot.terminal) continue;
                if(!slot.waiting) blocked = false;
            }
            if(blocked) break;
        }
        return this->isComplete();
    }

    bool PagedFrame::run(std::size_t maxRounds)
    {
        if(this->cancelled) return false;
        if(!this->started) this->start();
        std::size_t round = 0;
        for(;;)
        {
            const bool complete = this->step(0);
            if(complete) return true;
            if(this->cancelled) return false;
            if(this->hasFailure()) return false;
            if(this->isDone()) return false;
            const bool pending = !this->cache->pendingRequests().empty();
            if(!pending) return this->isComplete();
            ++round;
            if(maxRounds != 0 && round >= maxRounds)
            {
                return this->isComplete();
            }
        }
    }

    bool PagedFrame::isComplete() const
    {
        if(this->cancelled || !this->started || this->slots.empty())
        {
            return false;
        }
        if(this->hasFailure()) return false;
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            if(!this->slots[i].terminal) return false;
        }
        return true;
    }

    bool PagedFrame::isDone() const
    {
        if(this->cancelled || this->slots.empty()) return false;
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            if(!this->slots[i].terminal) return false;
        }
        return true;
    }

    bool PagedFrame::hasFailure() const
    {
        if(this->failed) return true;
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            if(this->slots[i].terminal &&
            this->slots[i].ray.hasFailed()) return true;
        }
        return false;
    }

    double PagedFrame::progress() const
    {
        if(this->slots.empty()) return 0.0;
        std::size_t done = 0;
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            if(this->slots[i].terminal) ++done;
        }
        return (double)done / (double)this->slots.size();
    }

    void PagedFrame::results(std::vector<RayResult>* out) const
    {
        if(out == NULL) return;
        out->clear();
        out->reserve(this->slots.size());
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            out->push_back(this->slots[i].ray.result());
        }
    }

    bool PagedFrame::deliver(RequestId requestId, const PageData& page)
    {
        if(this->cancelled) return false;
        if(!this->tokenIssued(requestId)) return false;
        if(!this->cache->complete(requestId, page)) return false;
        pruneIssued(&this->issued, requestId);
        /*
        * Wake waiters without advancing anything: a duplicate delivery finds
        * no pending request and is rejected above, so a response can never
        * advance a ray twice.  A completion is still "accepted" when no ray
        * of this frame happens to wait on it any more (all waiters may have
        * failed); the caller can distinguish that by checking stats.
        */
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            Slot& slot = this->slots[i];
            if(slot.terminal || !slot.waiting) continue;
            if(slot.request != requestId) continue;
            slot.waiting = false;
            slot.request = INVALID_REQUEST;
            slot.page = INVALID_PAGE;
        }
        return true;
    }

    std::size_t PagedFrame::failRequest(RequestId requestId)
    {
        if(this->cancelled) return 0;
        if(!this->tokenIssued(requestId)) return 0;
        if(!this->cache->fail(requestId)) return 0;
        pruneIssued(&this->issued, requestId);
        std::size_t affected = 0;
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            Slot& slot = this->slots[i];
            if(slot.terminal || !slot.waiting) continue;
            if(slot.request != requestId) continue;
            this->failSlot(&slot);
            ++affected;
        }
        return affected;
    }

    void PagedFrame::cancel()
    {
        if(this->cancelled) return;
        this->cancelled = true;
        /*
        * Detach every wait token, then revoke this generation's outstanding
        * requests.  A later completion for one of those ids is rejected by
        * the cache, so it can neither fill the cache nor resume a ray.
        */
        for(std::size_t i = 0; i < this->slots.size(); ++i)
        {
            Slot& slot = this->slots[i];
            if(slot.request != INVALID_REQUEST)
            {
                this->cache->removeWaiter(slot.request, slot.token);
                slot.request = INVALID_REQUEST;
            }
            slot.waiting = false;
            slot.terminal = true;
        }
        /*
        * Revoke this generation's outstanding requests.  A request still
        * shared with another live frame is left alone; its other waiters
        * remain valid and the cancelled frame's tokens are already gone.
        */
        for(std::size_t i = 0; i < this->issued.size(); ++i)
        {
            this->cache->revokeIfUnwaited(this->issued[i]);
        }
        this->issued.clear();
    }

}
