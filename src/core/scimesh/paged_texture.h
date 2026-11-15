/// @file paged_texture.h
/// @brief PagedCache — an LRU cache for texture pages delivered piece by piece.
///
/// A paged texture is never held as one complete image by the cache.  The
/// caller publishes an immutable *version* described by a TextureDescriptor
/// (logical texture id, full dimensions and a square page edge length).
/// Texture pages are then requested explicitly and supplied explicitly:
///
/// 1. A consumer (see PagedRenderJob) calls request_page().  A resident page
///    is served immediately and becomes the most-recently-used page.  A
///    missing page creates an in-flight PageRequest carrying a unique ticket.
///    Several consumers waiting on the same (version, page) share one request
///    and one ticket.
/// 2. The caller fetches the page bytes and hands them back with supply(),
///    in any order.  A response whose ticket is unknown, whose dimensions do
///    not match the page rectangle or whose byte count is wrong is rejected
///    without changing any state.  Completing a ticket twice has no second
///    effect.
/// 3. Accepted pages enter the LRU residency set, which never holds more than
///    the configured number of full pages.  Supplying a page also fans the
///    texels out to every waiter immediately, so a later eviction can never
///    take away a tap that a waiting fragment already received.
///
/// The cache is single-threaded: all methods are meant to be called from one
/// thread, in the same thread that runs PagedRenderJob::step().
///
/// @see PagedRenderJob, paged_renderer.h

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <vector>

namespace scimesh {

/// @brief Immutable description of one published version of a paged texture.
///
/// All dimensions are positive integers.  `page_size` is the edge length of a
/// full page in texels; pages on the right and bottom edges may be smaller.
/// Two publishes of the same `logical_id` produce distinct versions, and
/// pages of different versions are never mixed even when their page
/// coordinates are equal.
struct TextureDescriptor {
    /// @brief Full texture width in texels (must be > 0).
    int width = 0;

    /// @brief Full texture height in texels (must be > 0).
    int height = 0;

    /// @brief Edge length of a (full) page in texels (must be > 0).
    int page_size = 0;

    /// @brief Caller-chosen logical texture id; carried for identification.
    uint64_t logical_id = 0;
};

/// @brief Coordinates of a page inside a texture version, in page units.
///
/// (0, 0) is the top-left page; x grows right and y grows down, matching the
/// row-major RGBA8 page layout and the Image coordinate convention.
struct PageCoord {
    int page_x = 0;  ///< Column of pages, 0-based.
    int page_y = 0;  ///< Row of pages, 0-based.

    bool operator==(const PageCoord &other) const {
        return page_x == other.page_x && page_y == other.page_y;
    }
};

/// @brief One outstanding page request.
///
/// A request is uniquely identified by its `ticket`.  The same ticket is
/// handed to every consumer waiting on that (version, page): there is at most
/// one in-flight request per version/page coordinate.
struct PageRequest {
    uint64_t version = 0;  ///< Texture version the page belongs to.
    int page_x = 0;        ///< Page column.
    int page_y = 0;        ///< Page row.
    uint64_t ticket = 0;   ///< Unique, non-zero ticket used to complete it.
};

/// @brief A resident page: its location, its actual dimensions and its bytes.
///
/// `rgba` holds `width * height * 4` row-major RGBA8 bytes.  Edge pages only
/// cover the actual texture extent, so `width`/`height` can be smaller than
/// TextureDescriptor::page_size.  `origin_x`/`origin_y` are the texel
/// coordinates of the page's top-left corner within the full texture.
struct ResidentPage {
    int page_x = 0;
    int page_y = 0;
    int origin_x = 0;
    int origin_y = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

/// @brief Result of a supply() call.
enum class PageSupplyOutcome {
    ACCEPTED,  ///< The page matched an outstanding ticket and is now resident.
    REJECTED   ///< Ticket, dimensions or byte count did not match; nothing changed.
};

/// @brief Consumer of asynchronously supplied pages.
///
/// PagedRenderJob implements this interface.  The cache calls
/// paged_page_ready() on every registered waiter of a request when supply()
/// accepts its page, before supply() returns.
class PagedPageWaiter {
public:
    virtual ~PagedPageWaiter() = default;

    /// @brief Called when a page this waiter asked for has been accepted.
    /// @param ticket  The ticket returned by request_page().
    /// @param version Texture version of the delivered page.
    /// @param page    The resident page, valid for the duration of the call
    ///                only; implementations copy any texels they need.
    virtual void paged_page_ready(uint64_t ticket, uint64_t version,
                                  const ResidentPage &page) = 0;
};

/// @brief LRU page cache shared by any number of PagedRenderJobs.
///
/// Owns the resident page bytes and the in-flight request bookkeeping.  It
/// never stores a complete texture and keeps no copy of an evicted page.
/// Consumers (jobs) own themselves; the cache only holds non-owning waiter
/// pointers, which they withdraw in their destructor.
class PagedCache {
public:
    /// @brief Construct a cache holding at most `capacity` resident pages.
    /// @param capacity Page capacity; must be >= 1.  Capacity counts actual
    ///                 pages, regardless of their (possibly edge) size.
    explicit PagedCache(size_t capacity);

    ~PagedCache();

    PagedCache(const PagedCache &) = delete;
    PagedCache &operator=(const PagedCache &) = delete;

    /// @brief Publish a new, immutable texture version.
    /// @param desc Positive width, height and page size.
    /// @return The new version handle (always non-zero).
    /// @throws std::invalid_argument if any descriptor dimension is <= 0.
    uint64_t publish(const TextureDescriptor &desc);

    /// @brief Release a version previously returned by publish().
    ///
    /// Removes its descriptor, its resident pages and its in-flight requests.
    /// Waiters are detached, not notified: callers must not keep jobs bound to
    /// a version they release.  A late response for one of the removed tickets
    /// is then rejected and can never fill another version.
    void release(uint64_t version);

    /// @brief Descriptor of a published version, or null if unknown/released.
    const TextureDescriptor *descriptor(uint64_t version) const;

    /// @brief Actual pixel dimensions of a page (edge pages may be smaller).
    /// @throws std::invalid_argument if the version is unknown or the page
    ///         coordinates fall outside the version's page grid.
    void page_rect(uint64_t version, int page_x, int page_y,
                   int &out_width, int &out_height) const;

    /// @brief Ask for a page.
    ///
    /// If the page is resident it is touched (moved to the MRU end) and this
    /// returns 0; use lookup_resident() to read it.  Otherwise an in-flight
    /// request is created (or an existing one for the same version/page is
    /// shared) and its unique ticket is returned; `waiter` is registered for
    /// delivery and withdrawn again by detach_waiter() or destruction.
    ///
    /// @return 0 when resident, otherwise the non-zero ticket.
    uint64_t request_page(uint64_t version, int page_x, int page_y,
                          PagedPageWaiter *waiter);

    /// @brief Resident page for a version/coordinate, or null if absent.
    ///
    /// A hit marks the page most-recently-used.  The returned pointer is
    /// valid only until the next call that may evict pages (another
    /// request_page() miss resolved by supply()).
    const ResidentPage *lookup_resident(uint64_t version, int page_x, int page_y);

    /// @brief Complete an in-flight request with row-major RGBA8 bytes.
    ///
    /// @param ticket     Ticket returned by request_page().
    /// @param width      Actual page width (full or edge size).
    /// @param height     Actual page height.
    /// @param bytes      `width * height * 4` RGBA8 bytes; must be non-null
    ///                   whenever the expected byte count is positive.
    /// @param byte_count Number of bytes pointed to by `bytes`.
    ///
    /// A mismatch (unknown/stale ticket, wrong dimensions, wrong byte count,
    /// null data) rejects the response without changing any state, and a
    /// second completion of the same ticket has no effect.  On acceptance the
    /// page is inserted into the LRU set (evicting the least-recently-used
    /// pages if necessary) and every waiter receives its taps before this
    /// returns.
    PageSupplyOutcome supply(uint64_t ticket, int width, int height,
                             const uint8_t *bytes, size_t byte_count);

    /// @brief All currently outstanding requests (no particular order).
    std::vector<PageRequest> pending_requests() const;

    /// @brief Number of pages currently resident (never above capacity()).
    size_t resident_page_count() const;

    /// @brief Configured page capacity.
    size_t capacity() const;

    /// @brief Withdraw a waiter from every in-flight request it joined.
    ///
    /// Requests whose last waiter leaves stay outstanding only while another
    /// waiter exists; requests without any waiter are removed.  Called by
    /// PagedRenderJob on cancellation/destruction.
    void detach_waiter(PagedPageWaiter *waiter);

private:
    struct PageKey {
        uint64_t version;
        int page_x;
        int page_y;

        bool operator<(const PageKey &other) const;
    };

    struct InFlightRequest {
        PageRequest request;
        std::vector<PagedPageWaiter *> waiters;
    };

    const TextureDescriptor &require_descriptor(uint64_t version) const;
    void evict_lru();
    void insert_resident(PageKey key, int width, int height,
                         const uint8_t *bytes);
    void touch_lru(const PageKey &key);

    size_t capacity_;
    uint64_t next_version_ = 1;
    uint64_t next_ticket_ = 1;

    // Published versions and their immutable descriptors.
    std::map<uint64_t, TextureDescriptor> descriptors_;

    // Resident pages and LRU order (front = least recently used).
    std::map<PageKey, ResidentPage> resident_;
    std::list<PageKey> lru_order_;
    std::map<PageKey, std::list<PageKey>::iterator> lru_it_;

    // Outstanding requests keyed by ticket, plus a coordinate index so that
    // concurrent waiters share one request per version/page.
    std::map<uint64_t, InFlightRequest> in_flight_;
    std::map<PageKey, uint64_t> in_flight_index_;
};

} // namespace scimesh
