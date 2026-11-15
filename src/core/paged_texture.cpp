#include <scimesh/paged_texture.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace scimesh {

bool PagedCache::PageKey::operator<(const PageKey &other) const {
    if (version != other.version) return version < other.version;
    if (page_x != other.page_x) return page_x < other.page_x;
    return page_y < other.page_y;
}

PagedCache::PagedCache(size_t capacity) : capacity_(capacity) {
    if (capacity < 1) {
        throw std::invalid_argument("PagedCache capacity must be >= 1");
    }
}

PagedCache::~PagedCache() {
    // All waiter pointers are non-owning; jobs detach in their own
    // destructors.  Nothing else to release beyond the container members.
}

uint64_t PagedCache::publish(const TextureDescriptor &desc) {
    if (desc.width <= 0 || desc.height <= 0 || desc.page_size <= 0) {
        throw std::invalid_argument(
            "TextureDescriptor width, height and page_size must be positive");
    }
    const uint64_t version = next_version_++;
    descriptors_.emplace(version, desc);
    return version;
}

void PagedCache::release(uint64_t version) {
    auto dit = descriptors_.find(version);
    if (dit == descriptors_.end()) return;
    descriptors_.erase(dit);

    // Drop every resident page of this version and its LRU entries.
    for (auto it = resident_.begin(); it != resident_.end();) {
        if (it->first.version == version) {
            auto lit = lru_it_.find(it->first);
            if (lit != lru_it_.end()) {
                lru_order_.erase(lit->second);
                lru_it_.erase(lit);
            }
            it = resident_.erase(it);
        } else {
            ++it;
        }
    }

    // Remove in-flight requests of this version; a later supply() with one of
    // these tickets is rejected because the ticket is gone.
    for (auto it = in_flight_.begin(); it != in_flight_.end();) {
        if (it->second.request.version == version) {
            in_flight_index_.erase(
                PageKey{version, it->second.request.page_x,
                        it->second.request.page_y});
            it = in_flight_.erase(it);
        } else {
            ++it;
        }
    }
}

const TextureDescriptor *PagedCache::descriptor(uint64_t version) const {
    auto it = descriptors_.find(version);
    if (it == descriptors_.end()) return nullptr;
    return &it->second;
}

const TextureDescriptor &PagedCache::require_descriptor(
    uint64_t version) const {
    auto it = descriptors_.find(version);
    if (it == descriptors_.end()) {
        throw std::invalid_argument("unknown or released texture version");
    }
    return it->second;
}

void PagedCache::page_rect(uint64_t version, int page_x, int page_y,
                           int &out_width, int &out_height) const {
    const TextureDescriptor &desc = require_descriptor(version);
    const int cols = (desc.width + desc.page_size - 1) / desc.page_size;
    const int rows = (desc.height + desc.page_size - 1) / desc.page_size;
    if (page_x < 0 || page_y < 0 || page_x >= cols || page_y >= rows) {
        throw std::invalid_argument("page coordinates out of range");
    }
    const int origin_x = page_x * desc.page_size;
    const int origin_y = page_y * desc.page_size;
    out_width = std::min(desc.page_size, desc.width - origin_x);
    out_height = std::min(desc.page_size, desc.height - origin_y);
}

uint64_t PagedCache::request_page(uint64_t version, int page_x, int page_y,
                                  PagedPageWaiter *waiter) {
    const TextureDescriptor &desc = require_descriptor(version);
    const int cols = (desc.width + desc.page_size - 1) / desc.page_size;
    const int rows = (desc.height + desc.page_size - 1) / desc.page_size;
    if (page_x < 0 || page_y < 0 || page_x >= cols || page_y >= rows) {
        throw std::invalid_argument("page coordinates out of range");
    }
    if (waiter == nullptr) {
        throw std::invalid_argument("request_page requires a waiter");
    }

    const PageKey key{version, page_x, page_y};

    // Resident: this is a cache hit; only the LRU order changes.
    if (resident_.find(key) != resident_.end()) {
        touch_lru(key);
        return 0;
    }

    // An identical request is already in flight: share its ticket and add
    // this waiter once.
    auto idx_it = in_flight_index_.find(key);
    if (idx_it != in_flight_index_.end()) {
        InFlightRequest &req = in_flight_.find(idx_it->second)->second;
        if (std::find(req.waiters.begin(), req.waiters.end(), waiter) ==
            req.waiters.end()) {
            req.waiters.push_back(waiter);
        }
        return req.request.ticket;
    }

    const uint64_t ticket = next_ticket_++;
    InFlightRequest entry;
    entry.request = PageRequest{version, page_x, page_y, ticket};
    entry.waiters.push_back(waiter);
    in_flight_.emplace(ticket, std::move(entry));
    in_flight_index_.emplace(key, ticket);
    return ticket;
}

const ResidentPage *PagedCache::lookup_resident(uint64_t version,
                                                int page_x, int page_y) {
    const PageKey key{version, page_x, page_y};
    auto it = resident_.find(key);
    if (it == resident_.end()) return nullptr;
    touch_lru(key);
    return &it->second;
}

PageSupplyOutcome PagedCache::supply(uint64_t ticket, int width, int height,
                                     const uint8_t *bytes, size_t byte_count) {
    auto req_it = in_flight_.find(ticket);
    if (req_it == in_flight_.end()) {
        // Unknown, already completed or cancelled ticket: state is untouched.
        return PageSupplyOutcome::REJECTED;
    }

    // Copy the request by value up front: erasing the map entry below would
    // invalidate any reference into it.
    const PageRequest req = req_it->second.request;
    int expected_w = 0;
    int expected_h = 0;
    page_rect(req.version, req.page_x, req.page_y, expected_w, expected_h);

    const size_t expected_bytes =
        static_cast<size_t>(expected_w) * static_cast<size_t>(expected_h) * 4u;
    if (width != expected_w || height != expected_h ||
        byte_count != expected_bytes ||
        (expected_bytes > 0 && bytes == nullptr)) {
        return PageSupplyOutcome::REJECTED;
    }

    // Take ownership of the request before mutating residency, so a waiter
    // callback that calls back into the cache sees a consistent state: the
    // request is gone and the page is already resident.
    InFlightRequest delivered = std::move(req_it->second);
    in_flight_.erase(req_it);
    in_flight_index_.erase(
        PageKey{req.version, req.page_x, req.page_y});

    const PageKey key{req.version, req.page_x, req.page_y};
    insert_resident(key, width, height, bytes);

    // Fan the texels out immediately.  Waiters copy what they need; if the
    // page is evicted afterwards, the delivered taps survive inside them.
    const ResidentPage &page = resident_.find(key)->second;
    for (PagedPageWaiter *waiter : delivered.waiters) {
        waiter->paged_page_ready(ticket, req.version, page);
    }
    return PageSupplyOutcome::ACCEPTED;
}

std::vector<PageRequest> PagedCache::pending_requests() const {
    std::vector<PageRequest> out;
    out.reserve(in_flight_.size());
    for (const auto &entry : in_flight_) {
        out.push_back(entry.second.request);
    }
    return out;
}

size_t PagedCache::resident_page_count() const {
    return resident_.size();
}

size_t PagedCache::capacity() const {
    return capacity_;
}

void PagedCache::detach_waiter(PagedPageWaiter *waiter) {
    if (waiter == nullptr) return;
    for (auto it = in_flight_.begin(); it != in_flight_.end();) {
        auto &waiters = it->second.waiters;
        waiters.erase(std::remove(waiters.begin(), waiters.end(), waiter),
                      waiters.end());
        if (waiters.empty()) {
            in_flight_index_.erase(
                PageKey{it->second.request.version,
                        it->second.request.page_x,
                        it->second.request.page_y});
            it = in_flight_.erase(it);
        } else {
            ++it;
        }
    }
}

void PagedCache::evict_lru() {
    if (lru_order_.empty()) return;
    const PageKey victim = lru_order_.front();
    lru_order_.pop_front();
    lru_it_.erase(victim);
    // erase() frees the page byte storage; no copy is retained anywhere.
    resident_.erase(victim);
}

void PagedCache::insert_resident(PageKey key, int width, int height,
                                 const uint8_t *bytes) {
    auto existing = resident_.find(key);
    if (existing != resident_.end()) {
        // Re-supply of an already resident page cannot normally happen (the
        // request would have been shared and completed once); keep behaviour
        // idempotent and just refresh LRU order.
        touch_lru(key);
        return;
    }

    while (resident_.size() >= capacity_) {
        evict_lru();
    }

    ResidentPage page;
    const TextureDescriptor &desc = require_descriptor(key.version);
    page.page_x = key.page_x;
    page.page_y = key.page_y;
    page.origin_x = key.page_x * desc.page_size;
    page.origin_y = key.page_y * desc.page_size;
    page.width = width;
    page.height = height;
    page.rgba.assign(bytes, bytes + static_cast<size_t>(width) *
                                      static_cast<size_t>(height) * 4u);
    resident_.emplace(key, std::move(page));
    lru_order_.push_back(key);
    lru_it_.emplace(key, std::prev(lru_order_.end()));
}

void PagedCache::touch_lru(const PageKey &key) {
    auto it = lru_it_.find(key);
    if (it == lru_it_.end()) return;
    lru_order_.splice(lru_order_.end(), lru_order_, it->second);
}

} // namespace scimesh
