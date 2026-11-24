#pragma once

#include <algorithm>    // sort, lower_bound, set_difference
#include <array>        // array
#include <cstddef>      // size_t
#include <cstdint>      // uint8_t, uint32_t, uint64_t
#include <cstdio>       // rename, remove
#include <cstring>      // strcmp
#include <dirent.h>     // opendir, readdir
#include <dlfcn.h>      // dladdr, Dl_info
#include <fstream>      // ifstream, ofstream
#include <map>          // map
#include <mutex>        // mutex, lock_guard
#include <optional>     // optional
#include <set>          // set
#include <string>       // string
#include <sys/stat.h>   // lstat
#include <unistd.h>     // getpid, unlink
#include <utility>      // pair, move
#include <vector>       // vector
#include <nlohmann/json.hpp> // nlohmann::json

#if defined(__APPLE__)
#include <mach/mach.h>       // vm_protect, mach_task_self
#include <mach-o/dyld.h>     // _dyld_image_count, _dyld_get_image_header
#include <mach-o/getsect.h>  // getsectiondata
#include <mach-o/loader.h>   // mach_header_64, segment_command_64, dyld_info_command
#endif

// The real sanitizer coverage callbacks. They are defined in src/coverage.cpp
// and forward to the registry below; instrumented modules reach them either
// directly (ELF global scope) or through the rebound slots installed by
// detail::ensure_image_hooks (Mach-O two-level namespace).
extern "C" void __sanitizer_cov_trace_pc_guard_init(std::uint32_t* start, std::uint32_t* stop);
extern "C" void __sanitizer_cov_trace_pc_guard(std::uint32_t* guard);

namespace fuzzcover {

// one covered edge: a guard slot of a bound module, identified by its index
// in the module's guard array ordered by guard address
struct CoverageHit
{
    std::string module;
    std::uint32_t guard = 0;
};

inline bool operator<(const CoverageHit& lhs, const CoverageHit& rhs)
{
    return lhs.module < rhs.module || (lhs.module == rhs.module && lhs.guard < rhs.guard);
}

inline bool operator==(const CoverageHit& lhs, const CoverageHit& rhs)
{
    return lhs.module == rhs.module && lhs.guard == rhs.guard;
}

// identity of a bound module as recorded in snapshots
struct CoverageModuleManifest
{
    std::string role;
    std::string build;
    std::string layout;
    std::uint64_t guards = 0;
};

inline bool operator==(const CoverageModuleManifest& lhs, const CoverageModuleManifest& rhs)
{
    return lhs.role == rhs.role && lhs.build == rhs.build && lhs.layout == rhs.layout && lhs.guards == rhs.guards;
}

class CoverageScope;

namespace detail {

struct GuardRange
{
    const std::uint32_t* begin = nullptr;
    const std::uint32_t* end = nullptr;
};

// coverage-related state of one loaded image (identity is its load base as
// reported by dladdr, never a path, basename, or registration order)
struct ImageCoverage
{
    const void* fbase = nullptr;
    std::vector<GuardRange> ranges;
    bool bound = false;
    std::string role;
    std::string build;
    std::string layout;
    std::vector<const std::uint32_t*> guards_sorted; // by guard address, fixed at bind time
#if defined(__APPLE__)
    bool hooks_installed = false;
#endif
};

struct CoverageRegistry
{
    std::mutex mutex;
    std::map<const void*, ImageCoverage> images;          // load base -> image state
    std::map<std::string, const void*> roles;             // role -> owning load base
    std::map<const std::uint32_t*, ImageCoverage*> owner; // guard slot -> image
    std::size_t active_scopes = 0;
};

inline CoverageRegistry& coverage_registry()
{
    static CoverageRegistry registry;
    return registry;
}

inline CoverageScope*& current_coverage_scope()
{
    static thread_local CoverageScope* scope = nullptr;
    return scope;
}

inline void coverage_guard_hit(const std::uint32_t* guard);
inline void coverage_register_guards(std::uint32_t* start, std::uint32_t* stop);

} // namespace detail

/*!
 * @brief per-thread coverage collection scope
 *
 * While a scope is alive on a thread, guard hits reported by that thread are
 * collected; hits from other threads or outside any scope are ignored. The
 * scope always closes on destruction, including exception unwinding.
 */
class CoverageScope
{
  public:
    CoverageScope()
        : previous_(detail::current_coverage_scope())
    {
        detail::current_coverage_scope() = this;
        auto& registry = detail::coverage_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        ++registry.active_scopes;
    }

    ~CoverageScope()
    {
        detail::current_coverage_scope() = previous_;
        auto& registry = detail::coverage_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        --registry.active_scopes;
    }

    CoverageScope(const CoverageScope&) = delete;
    CoverageScope& operator=(const CoverageScope&) = delete;

    /*! @brief false if a guard of an unbound module was hit inside the scope */
    bool ok() const
    {
        return !unbound_hit_;
    }

    /*! @brief collected hits, sorted by module role and guard index */
    const std::set<CoverageHit>& hits() const
    {
        return hits_;
    }

  private:
    void note_hit(const std::string& module, std::uint32_t guard)
    {
        hits_.insert(CoverageHit{module, guard});
    }

    void note_unbound_hit()
    {
        unbound_hit_ = true;
    }

    CoverageScope* previous_;
    bool unbound_hit_ = false;
    std::set<CoverageHit> hits_;

    friend void detail::coverage_guard_hit(const std::uint32_t*);
};

namespace detail {

inline void coverage_guard_hit(const std::uint32_t* guard)
{
    if (guard == nullptr || *guard == 0)
    {
        return; // disabled edge
    }
    CoverageScope* scope = current_coverage_scope();
    if (scope == nullptr)
    {
        return; // not collecting on this thread
    }
    auto& registry = coverage_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto it = registry.owner.find(guard);
    if (it == registry.owner.end() || !it->second->bound)
    {
        scope->note_unbound_hit(); // unbound module: fail loudly, never silently drop
        return;
    }
    const ImageCoverage* image = it->second;
    const auto pos = std::lower_bound(image->guards_sorted.begin(), image->guards_sorted.end(), guard);
    if (pos == image->guards_sorted.end() || *pos != guard)
    {
        scope->note_unbound_hit();
        return;
    }
    scope->note_hit(image->role, static_cast<std::uint32_t>(pos - image->guards_sorted.begin()));
}

inline void coverage_register_guards(std::uint32_t* start, std::uint32_t* stop)
{
    if (start == nullptr || stop == nullptr || stop <= start)
    {
        return;
    }
    Dl_info info;
    if (dladdr(static_cast<const void*>(start), &info) == 0 || info.dli_fbase == nullptr)
    {
        return; // cannot attribute the range to a loaded image
    }
    auto& registry = coverage_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    ImageCoverage& image = registry.images[info.dli_fbase];
    image.fbase = info.dli_fbase;
    bool known = false;
    for (const auto& range : image.ranges)
    {
        if (range.begin == start && range.end == stop)
        {
            known = true;
            break;
        }
    }
    if (!known)
    {
        image.ranges.push_back(GuardRange{start, stop});
        for (const std::uint32_t* guard = start; guard != stop; ++guard)
        {
            registry.owner[guard] = &image;
        }
    }
    for (std::uint32_t* guard = start; guard != stop; ++guard)
    {
        if (*guard == 0)
        {
            *guard = 1; // enable the edge; identity comes from the slot address
        }
    }
}

} // namespace detail

#if defined(__APPLE__)
namespace detail {

inline const std::uint8_t* read_uleb128(const std::uint8_t* p, std::uint64_t& value)
{
    std::uint64_t result = 0;
    unsigned shift = 0;
    std::uint8_t byte = 0;
    do
    {
        byte = *p++;
        result |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        shift += 7;
    }
    while (byte & 0x80);
    value = result;
    return p;
}

inline const std::uint8_t* read_sleb128(const std::uint8_t* p, std::int64_t& value)
{
    std::int64_t result = 0;
    unsigned shift = 0;
    std::uint8_t byte = 0;
    do
    {
        byte = *p++;
        result |= static_cast<std::int64_t>(byte & 0x7f) << shift;
        shift += 7;
    }
    while (byte & 0x80);
    if (shift < 64 && (byte & 0x40) != 0)
    {
        result |= -(static_cast<std::int64_t>(1) << shift);
    }
    value = result;
    return p;
}

// Rewrite every bind slot named @a symbol of the image at @a image_index to
// @a replacement. Mach-O modules linked against the sanitizer runtime bind
// their coverage calls to that runtime (two-level namespace), so the slots
// are patched to reach this process's real callbacks instead.
inline void rebind_symbol_in_image(std::uint32_t image_index, const char* symbol, void* replacement)
{
    const auto* header = reinterpret_cast<const struct mach_header_64*>(_dyld_get_image_header(image_index));
    if (header == nullptr)
    {
        return;
    }
    const std::intptr_t slide = _dyld_get_image_vmaddr_slide(image_index);
    const struct segment_command_64* linkedit = nullptr;
    const struct dyld_info_command* dyld_info = nullptr;
    std::uint64_t segment_vmaddr[64] = {};
    std::size_t segment_count = 0;
    const auto* command = reinterpret_cast<const struct load_command*>(header + 1);
    for (std::uint32_t i = 0; i < header->ncmds; ++i)
    {
        if (command->cmd == LC_SEGMENT_64)
        {
            const auto* segment = reinterpret_cast<const struct segment_command_64*>(command);
            if (segment_count < 64)
            {
                segment_vmaddr[segment_count++] = segment->vmaddr;
            }
            if (std::strcmp(segment->segname, SEG_LINKEDIT) == 0)
            {
                linkedit = segment;
            }
        }
        else if (command->cmd == LC_DYLD_INFO || command->cmd == LC_DYLD_INFO_ONLY)
        {
            dyld_info = reinterpret_cast<const struct dyld_info_command*>(command);
        }
        command = reinterpret_cast<const struct load_command*>(reinterpret_cast<const std::uint8_t*>(command) + command->cmdsize);
    }
    if (linkedit == nullptr || dyld_info == nullptr || dyld_info->bind_size == 0)
    {
        return;
    }
    const std::uintptr_t linkedit_base = static_cast<std::uintptr_t>(slide) + linkedit->vmaddr - linkedit->fileoff;
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(linkedit_base + dyld_info->bind_off);
    const std::uint8_t* const end = p + dyld_info->bind_size;
    std::uint32_t segment_index = 0;
    std::uint64_t offset = 0;
    const char* bound_symbol = nullptr;
    const long page_size = static_cast<long>(getpagesize());
    while (p < end)
    {
        const std::uint8_t immediate = *p & BIND_IMMEDIATE_MASK;
        const std::uint8_t opcode = *p & BIND_OPCODE_MASK;
        ++p;
        std::uint64_t uvalue = 0;
        std::int64_t svalue = 0;
        switch (opcode)
        {
            case BIND_OPCODE_DONE:
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
            case BIND_OPCODE_SET_TYPE_IMM:
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                p = read_uleb128(p, uvalue);
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
                bound_symbol = reinterpret_cast<const char*>(p);
                while (*p != '\0')
                {
                    ++p;
                }
                ++p;
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                p = read_sleb128(p, svalue);
                break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                segment_index = immediate;
                p = read_uleb128(p, offset);
                break;
            case BIND_OPCODE_ADD_ADDR_ULEB:
                p = read_uleb128(p, uvalue);
                offset += uvalue;
                break;
            case BIND_OPCODE_DO_BIND:
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
            {
                if (segment_index < segment_count && bound_symbol != nullptr && std::strcmp(bound_symbol, symbol) == 0)
                {
                    const std::uintptr_t location = static_cast<std::uintptr_t>(slide) + segment_vmaddr[segment_index] + offset;
                    const std::uintptr_t page = location & ~static_cast<std::uintptr_t>(page_size - 1);
                    if (vm_protect(mach_task_self(), page, static_cast<vm_size_t>(page_size), FALSE, VM_PROT_READ | VM_PROT_WRITE) == KERN_SUCCESS)
                    {
                        *reinterpret_cast<void**>(location) = replacement;
                        vm_protect(mach_task_self(), page, static_cast<vm_size_t>(page_size), FALSE, VM_PROT_READ);
                    }
                }
                if (opcode == BIND_OPCODE_DO_BIND)
                {
                    offset += sizeof(void*);
                }
                else if (opcode == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB)
                {
                    p = read_uleb128(p, uvalue);
                    offset += uvalue + sizeof(void*);
                }
                else
                {
                    offset += immediate * sizeof(void*) + sizeof(void*);
                }
                break;
            }
            default:
                return; // unknown opcode: stop without touching anything else
        }
    }
}

// Make the sanitizer coverage of the image at @a fbase observable: register
// its guard section through the real init callback and rebind its coverage
// slots to this process's real guard callback. Idempotent per image.
inline void ensure_image_hooks(const void* fbase)
{
    if (fbase == nullptr)
    {
        return;
    }
    {
        auto& registry = coverage_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        const auto it = registry.images.find(fbase);
        if (it != registry.images.end() && it->second.hooks_installed)
        {
            return;
        }
    }
    const auto* header = static_cast<const struct mach_header*>(fbase);
    std::uint32_t image_index = 0;
    bool found = false;
    const std::uint32_t count = _dyld_image_count();
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (_dyld_get_image_header(i) == header)
        {
            image_index = i;
            found = true;
            break;
        }
    }
    if (!found)
    {
        return;
    }
    unsigned long section_size = 0;
    std::uint8_t* section = getsectiondata(reinterpret_cast<const struct mach_header_64*>(header), SEG_DATA, "__sancov_guards", &section_size);
    if (section != nullptr && section_size >= sizeof(std::uint32_t))
    {
        auto* first = reinterpret_cast<std::uint32_t*>(section);
        __sanitizer_cov_trace_pc_guard_init(first, first + section_size / sizeof(std::uint32_t));
    }
    rebind_symbol_in_image(image_index, "___sanitizer_cov_trace_pc_guard", reinterpret_cast<void*>(&__sanitizer_cov_trace_pc_guard));
    rebind_symbol_in_image(image_index, "___sanitizer_cov_trace_pc_guard_init", reinterpret_cast<void*>(&__sanitizer_cov_trace_pc_guard_init));
    auto& registry = coverage_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.images[fbase].hooks_installed = true;
}

} // namespace detail
#endif // defined(__APPLE__)

namespace detail {

inline bool is_lower_hex(const std::string& value, std::size_t length)
{
    if (value.size() != length)
    {
        return false;
    }
    for (const char c : value)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }
    return true;
}

inline bool is_valid_role(const std::string& role)
{
    if (role.empty())
    {
        return false;
    }
    for (const char c : role)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

} // namespace detail

/*!
 * @brief bind the loaded image containing @a anchor to a module identity
 *
 * The image is identified by its actual load base (the same base the
 * registered guard ranges are attributed to), never by registration order,
 * path, or basename. Guard indices follow the guard addresses inside the
 * image, so relocation or load order does not change the identity.
 *
 * Repeating the identical binding is a no-op. Binding the same image to a
 * different identity, or reusing a role for another image, is rejected and
 * leaves the existing binding untouched. Bindings cannot change while a
 * coverage scope is active.
 *
 * @param anchor address of a symbol of the module's loaded image
 * @param role non-empty ASCII [A-Za-z0-9_] module role
 * @param build 64-digit lowercase hex build identifier from the build manifest
 * @param layout 64-digit lowercase hex layout identifier from the build manifest
 * @return true if the binding is in effect, false if it was rejected
 */
inline bool BindCoverageModule(const void* anchor, const std::string& role, const std::string& build, const std::string& layout)
{
    if (!detail::is_valid_role(role) || !detail::is_lower_hex(build, 64) || !detail::is_lower_hex(layout, 64))
    {
        return false;
    }
    if (anchor == nullptr)
    {
        return false;
    }
    Dl_info info;
    if (dladdr(anchor, &info) == 0 || info.dli_fbase == nullptr)
    {
        return false;
    }
#if defined(__APPLE__)
    detail::ensure_image_hooks(info.dli_fbase);
#endif
    auto& registry = detail::coverage_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto it = registry.images.find(info.dli_fbase);
    if (it == registry.images.end() || it->second.ranges.empty())
    {
        return false; // the image registered no guards: nothing to cover
    }
    detail::ImageCoverage& image = it->second;
    if (image.bound)
    {
        // idempotent for the identical binding; any identity change is rejected
        return image.role == role && image.build == build && image.layout == layout;
    }
    if (registry.active_scopes != 0)
    {
        return false; // bindings must not change during active collection
    }
    if (registry.roles.find(role) != registry.roles.end())
    {
        return false; // the role already belongs to another image
    }
    std::vector<const std::uint32_t*> guards;
    for (const auto& range : image.ranges)
    {
        for (const std::uint32_t* guard = range.begin; guard != range.end; ++guard)
        {
            guards.push_back(guard);
        }
    }
    std::sort(guards.begin(), guards.end());
    guards.erase(std::unique(guards.begin(), guards.end()), guards.end());
    image.guards_sorted = std::move(guards);
    image.bound = true;
    image.role = role;
    image.build = build;
    image.layout = layout;
    registry.roles[role] = info.dli_fbase;
    return true;
}

/*! @brief manifests of all bound modules, sorted by role */
inline std::vector<CoverageModuleManifest> BoundCoverageModules()
{
    auto& registry = detail::coverage_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    std::vector<CoverageModuleManifest> manifests;
    for (const auto& entry : registry.images)
    {
        if (entry.second.bound)
        {
            manifests.push_back(CoverageModuleManifest{entry.second.role, entry.second.build, entry.second.layout, entry.second.guards_sorted.size()});
        }
    }
    std::sort(manifests.begin(), manifests.end(), [](const CoverageModuleManifest& lhs, const CoverageModuleManifest& rhs)
    {
        return lhs.role < rhs.role;
    });
    return manifests;
}

namespace detail {

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4), single-shot over a byte buffer
// ---------------------------------------------------------------------------

inline std::uint32_t sha256_rotr(std::uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

inline void sha256_transform(std::uint32_t* state, const std::uint8_t* block)
{
    static const std::uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    std::uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i)
    {
        w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) | (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
               (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) | static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (unsigned i = 16; i < 64; ++i)
    {
        const std::uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (unsigned i = 0; i < 64; ++i)
    {
        const std::uint32_t s1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + k[i] + w[i];
        const std::uint32_t s0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

inline std::array<std::uint8_t, 32> sha256_digest(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::uint8_t block[64];
    std::size_t buffered = 0;
    std::uint64_t total = 0;
    while (size > 0)
    {
        const std::size_t take = std::min<std::size_t>(64 - buffered, size);
        std::memcpy(block + buffered, data, take);
        buffered += take;
        data += take;
        size -= take;
        total += take;
        if (buffered == 64)
        {
            sha256_transform(state, block);
            buffered = 0;
        }
    }
    const std::uint64_t bit_length = total * 8;
    block[buffered++] = 0x80;
    if (buffered > 56)
    {
        while (buffered < 64)
        {
            block[buffered++] = 0;
        }
        sha256_transform(state, block);
        buffered = 0;
    }
    while (buffered < 56)
    {
        block[buffered++] = 0;
    }
    for (int i = 7; i >= 0; --i)
    {
        block[buffered++] = static_cast<std::uint8_t>(bit_length >> (8 * i));
    }
    sha256_transform(state, block);
    std::array<std::uint8_t, 32> digest;
    for (unsigned i = 0; i < 8; ++i)
    {
        digest[4 * i] = static_cast<std::uint8_t>(state[i] >> 24);
        digest[4 * i + 1] = static_cast<std::uint8_t>(state[i] >> 16);
        digest[4 * i + 2] = static_cast<std::uint8_t>(state[i] >> 8);
        digest[4 * i + 3] = static_cast<std::uint8_t>(state[i]);
    }
    return digest;
}

inline std::string hex_encode(const std::uint8_t* data, std::size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i)
    {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0x0f]);
    }
    return out;
}

inline std::string sha256_hex(const std::uint8_t* data, std::size_t size)
{
    const auto digest = sha256_digest(data, size);
    return hex_encode(digest.data(), digest.size());
}

inline std::string sha256_hex(const std::vector<std::uint8_t>& bytes)
{
    return sha256_hex(bytes.data(), bytes.size());
}

/*! @brief decode lowercase hex; nullopt on odd length or any non-hex digit */
inline std::optional<std::vector<std::uint8_t>> hex_decode(const std::string& text)
{
    if (text.size() % 2 != 0)
    {
        return std::nullopt;
    }
    auto nibble = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2)
    {
        const int high = nibble(text[i]);
        const int low = nibble(text[i + 1]);
        if (high < 0 || low < 0)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

// ---------------------------------------------------------------------------
// file helpers
// ---------------------------------------------------------------------------

/*! @brief read a file completely; nullopt on any I/O error */
inline std::optional<std::vector<std::uint8_t>> read_file_bytes(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return std::nullopt;
    }
    file.seekg(0, std::ios::end);
    const auto length = file.tellg();
    if (length < 0)
    {
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (length > 0)
    {
        file.read(reinterpret_cast<char*>(bytes.data()), length);
        if (!file)
        {
            return std::nullopt;
        }
    }
    return bytes;
}

/*! @brief read a whole file as text; nullopt on any I/O error */
inline std::optional<std::string> read_file_text(const std::string& path)
{
    const auto bytes = read_file_bytes(path);
    if (!bytes)
    {
        return std::nullopt;
    }
    return std::string(bytes->begin(), bytes->end());
}

/*!
 * @brief write @a content to @a path atomically
 *
 * The content is written to a temporary file in the same directory first and
 * then renamed over the target, so a failure keeps the previous file intact.
 */
inline bool write_file_atomic(const std::string& path, const std::string& content)
{
    const std::string temporary = path + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return false;
        }
        file << content;
        file.flush();
        if (!file)
        {
            std::remove(temporary.c_str());
            return false;
        }
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

/*!
 * @brief list corpus files as (path, name) pairs
 *
 * Only regular files are accepted; symbolic links and any other non-regular
 * entry make the listing fail. nullopt is also returned if the directory
 * cannot be opened.
 */
inline std::optional<std::vector<std::pair<std::string, std::string>>> list_corpus_files(const std::string& directory)
{
    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr)
    {
        return std::nullopt;
    }
    std::vector<std::pair<std::string, std::string>> files;
    while (const struct dirent* entry = readdir(dir))
    {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
        {
            continue;
        }
        const std::string path = directory + "/" + name;
        struct stat status;
        if (lstat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
        {
            closedir(dir);
            return std::nullopt;
        }
        files.emplace_back(path, name);
    }
    closedir(dir);
    return files;
}

// ---------------------------------------------------------------------------
// snapshot document validation
// ---------------------------------------------------------------------------

inline nlohmann::json coverage_hits_to_json(const std::vector<CoverageHit>& hits)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& hit : hits)
    {
        out.push_back(nlohmann::json{{"module", hit.module}, {"guard", hit.guard}});
    }
    return out;
}

inline nlohmann::json manifests_to_json(const std::vector<CoverageModuleManifest>& manifests)
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& manifest : manifests)
    {
        out.push_back(nlohmann::json{{"role", manifest.role}, {"build", manifest.build}, {"layout", manifest.layout}, {"guards", manifest.guards}});
    }
    return out;
}

struct ValidatedEntry
{
    std::string sha256;
    std::vector<std::uint8_t> bytes;
    std::vector<std::string> names;
    nlohmann::json input;
    nlohmann::json output;
    std::vector<CoverageHit> coverage;
};

struct ValidatedSnapshot
{
    std::string source;
    std::vector<CoverageModuleManifest> modules;
    std::vector<ValidatedEntry> entries;
};

inline std::optional<CoverageModuleManifest> validate_module(const nlohmann::json& module)
{
    if (!module.is_object() || module.size() != 4 || !module.contains("role") || !module.contains("build") || !module.contains("layout") || !module.contains("guards"))
    {
        return std::nullopt;
    }
    const auto& role = module["role"];
    const auto& build = module["build"];
    const auto& layout = module["layout"];
    const auto& guards = module["guards"];
    if (!role.is_string() || !is_valid_role(role.get_ref<const std::string&>()))
    {
        return std::nullopt;
    }
    if (!build.is_string() || !is_lower_hex(build.get_ref<const std::string&>(), 64))
    {
        return std::nullopt;
    }
    if (!layout.is_string() || !is_lower_hex(layout.get_ref<const std::string&>(), 64))
    {
        return std::nullopt;
    }
    if (!guards.is_number_unsigned() || guards.get<std::uint64_t>() == 0)
    {
        return std::nullopt;
    }
    return CoverageModuleManifest{role.get<std::string>(), build.get<std::string>(), layout.get<std::string>(), guards.get<std::uint64_t>()};
}

/*!
 * @brief fully validate a snapshot document against the expected source
 *
 * Checks the format marker, source identity, module manifest, entry
 * structure, array ordering and uniqueness, raw digests, and coverage
 * indices. Nothing is executed and no file is written while validating.
 */
inline std::optional<ValidatedSnapshot> validate_snapshot_text(const std::string& text, const std::string& expected_source)
{
    if (!is_lower_hex(expected_source, 40))
    {
        return std::nullopt;
    }
    const nlohmann::json document = nlohmann::json::parse(text, nullptr, false);
    if (document.is_discarded() || !document.is_object() || document.size() != 4 || !document.contains("format") || !document.contains("source") || !document.contains("modules") || !document.contains("entries"))
    {
        return std::nullopt;
    }
    if (!document["format"].is_string() || document["format"].get_ref<const std::string&>() != "fuzzcover-regression-1")
    {
        return std::nullopt;
    }
    if (!document["source"].is_string() || document["source"].get_ref<const std::string&>() != expected_source)
    {
        return std::nullopt;
    }
    ValidatedSnapshot snapshot;
    snapshot.source = expected_source;
    const auto& modules = document["modules"];
    if (!modules.is_array() || modules.empty())
    {
        return std::nullopt;
    }
    std::map<std::string, std::uint64_t> guard_counts;
    std::string previous_role;
    for (const auto& module_json : modules)
    {
        const auto module = validate_module(module_json);
        if (!module)
        {
            return std::nullopt;
        }
        if (!snapshot.modules.empty() && !(previous_role < module->role))
        {
            return std::nullopt; // modules must be sorted by role without duplicates
        }
        previous_role = module->role;
        guard_counts[module->role] = module->guards;
        snapshot.modules.push_back(*module);
    }
    const auto& entries = document["entries"];
    if (!entries.is_array() || entries.empty())
    {
        return std::nullopt;
    }
    std::string previous_sha256;
    for (const auto& entry_json : entries)
    {
        if (!entry_json.is_object() || entry_json.size() != 6 || !entry_json.contains("sha256") || !entry_json.contains("bytes") || !entry_json.contains("names") || !entry_json.contains("input") || !entry_json.contains("output") || !entry_json.contains("coverage"))
        {
            return std::nullopt;
        }
        ValidatedEntry entry;
        const auto& sha256 = entry_json["sha256"];
        const auto& bytes = entry_json["bytes"];
        const auto& names = entry_json["names"];
        const auto& coverage = entry_json["coverage"];
        if (!sha256.is_string() || !is_lower_hex(sha256.get_ref<const std::string&>(), 64))
        {
            return std::nullopt;
        }
        entry.sha256 = sha256.get<std::string>();
        if (!bytes.is_string())
        {
            return std::nullopt;
        }
        const auto decoded = hex_decode(bytes.get_ref<const std::string&>());
        if (!decoded)
        {
            return std::nullopt;
        }
        if (sha256_hex(*decoded) != entry.sha256)
        {
            return std::nullopt; // raw digest does not match the archived bytes
        }
        entry.bytes = *decoded;
        if (!names.is_array() || names.empty())
        {
            return std::nullopt;
        }
        std::string previous_name;
        for (const auto& name_json : names)
        {
            if (!name_json.is_string())
            {
                return std::nullopt;
            }
            const std::string name = name_json.get<std::string>();
            if (name.empty() || name.find('/') != std::string::npos)
            {
                return std::nullopt;
            }
            if (!entry.names.empty() && !(previous_name < name))
            {
                return std::nullopt; // names must be sorted and unique
            }
            previous_name = name;
            entry.names.push_back(name);
        }
        if (!coverage.is_array())
        {
            return std::nullopt;
        }
        CoverageHit previous_hit{"", 0};
        for (const auto& hit_json : coverage)
        {
            if (!hit_json.is_object() || hit_json.size() != 2 || !hit_json.contains("module") || !hit_json.contains("guard"))
            {
                return std::nullopt;
            }
            const auto& module = hit_json["module"];
            const auto& guard = hit_json["guard"];
            if (!module.is_string() || !guard.is_number_unsigned())
            {
                return std::nullopt;
            }
            const auto known = guard_counts.find(module.get_ref<const std::string&>());
            if (known == guard_counts.end() || guard.get<std::uint64_t>() >= known->second)
            {
                return std::nullopt; // coverage index outside the module manifest
            }
            CoverageHit hit{module.get<std::string>(), guard.get<std::uint32_t>()};
            if (!entry.coverage.empty() && !(previous_hit < hit))
            {
                return std::nullopt; // coverage must be sorted and unique
            }
            previous_hit = hit;
            entry.coverage.push_back(hit);
        }
        entry.input = entry_json["input"];
        entry.output = entry_json["output"];
        if (!snapshot.entries.empty() && !(previous_sha256 < entry.sha256))
        {
            return std::nullopt; // entries must be sorted by digest without duplicates
        }
        previous_sha256 = entry.sha256;
        snapshot.entries.push_back(std::move(entry));
    }
    return snapshot;
}

} // namespace detail
} // namespace fuzzcover
