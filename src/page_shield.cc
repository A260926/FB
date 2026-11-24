/*
 * page_shield.cc - optional local empty-page access barrier.
 *
 * All of the design rationale (chain reachability, sleeping/needed
 * thresholds, budget shrinking and cross-page restoration) is documented in
 * docs/page_shield.md.  The short version:
 *
 *  - External metadata only (this registry), never stored on user pages.
 *  - Outstanding counts are maintained by narrow alloc/free hooks.
 *  - Before a page is protected, every free-list node intersecting it is
 *    removed from meta.free_queue and parked here, so no reachable snmalloc
 *    chain can read or write the PROT_NONE page.
 *  - Allocation restores, on demand, exactly the pages touched by the slot
 *    that is being re-threaded; whole-slab clear_slab restores everything.
 */

#include <snmalloc/page_shield.h>

#if defined(SNMALLOC_ENABLE_PAGE_SHIELD) && \
  defined(SNMALLOC_PAGE_SHIELD_HEADER)

#  include <algorithm>
#  include <cstdio>
#  include <cstdlib>
#  include <map>
#  include <memory>
#  include <mutex>
#  include <vector>

#  include <sys/mman.h>
#  include <unistd.h>

#  ifdef __APPLE__
#    include <mach/mach.h>
#    include <mach/mach_vm.h>
#  elif defined(__linux__)
#    include <cstdio>
#  endif

namespace snmalloc
{
  namespace ps_detail
  {
    using A = snmalloc::Alloc;
    using Config = A::Config;
    using Backend = Config::Backend;
    using Meta = Config::Backend::SlabMetadata;

    static size_t os_page_size() noexcept
    {
      static const size_t ps = []() noexcept {
        long v = ::sysconf(_SC_PAGESIZE);
        return v > 0 ? static_cast<size_t>(v) : size_t(4096);
      }();
      return ps;
    }

    /* Read-only OS permission probe.  Returns r/w/x bits or -1. */
    static int os_permission(uintptr_t at) noexcept
    {
#  ifdef __APPLE__
      mach_vm_address_t a = static_cast<mach_vm_address_t>(at);
      mach_vm_size_t n = 0;
      vm_region_basic_info_data_64_t info{};
      mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t object = MACH_PORT_NULL;
      kern_return_t kr = mach_vm_region(
        mach_task_self(),
        &a,
        &n,
        VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info),
        &count,
        &object);
      if (object != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), object);
      if (kr != KERN_SUCCESS || a > at || at - a >= n)
        return -1;
      return ((info.protection & VM_PROT_READ) ? 1 : 0) |
        ((info.protection & VM_PROT_WRITE) ? 2 : 0) |
        ((info.protection & VM_PROT_EXECUTE) ? 4 : 0);
#  elif defined(__linux__)
      std::FILE* f = std::fopen("/proc/self/maps", "r");
      if (f == nullptr)
        return -1;
      char line[1024];
      char mode[5]{};
      unsigned long long lo = 0, hi = 0;
      int result = -1;
      while (std::fgets(line, sizeof(line), f) != nullptr)
      {
        if (std::sscanf(line, "%llx-%llx %4s", &lo, &hi, mode) == 3 &&
            at >= lo && at < hi)
        {
          result = (mode[0] == 'r' ? 1 : 0) | (mode[1] == 'w' ? 2 : 0) |
            (mode[2] == 'x' ? 4 : 0);
          break;
        }
      }
      std::fclose(f);
      return result;
#  else
      return -1;
#  endif
    }

    [[noreturn]] static void permission_failure() noexcept
    {
      /* Exact token required by the contract.  Further allocator operation
       * could touch a chain whose permissions are now unknown. */
      static const char msg[] = "PAGE_SHIELD_PERMISSION_FAILURE\n";
      ssize_t r = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
      (void)r;
      std::abort();
    }

    static void protect_or_die(uintptr_t page, size_t bytes, int prot) noexcept
    {
      if (::mprotect(reinterpret_cast<void*>(page), bytes, prot) != 0)
        permission_failure();
    }

    struct SlabRec
    {
      uintptr_t slab = 0;
      void* meta = nullptr;
      void* owner_key = nullptr;
      size_t sc_raw = 0;
      size_t slab_bytes = 0;
      size_t obj_size = 0;
      size_t ps = 0;
      int npages = 0;
      long live = 0;
      std::vector<int> outstanding;
      std::vector<unsigned char> shielded;
      /* Free-slot addresses removed from meta->free_queue. */
      std::vector<uintptr_t> parked;
    };

    static std::mutex& mu() noexcept
    {
      static std::mutex m;
      return m;
    }

    static std::map<uintptr_t, std::unique_ptr<SlabRec>>& slabs() noexcept
    {
      static std::map<uintptr_t, std::unique_ptr<SlabRec>> m;
      return m;
    }

    static std::map<void*, uintptr_t>& meta_index() noexcept
    {
      static std::map<void*, uintptr_t> m;
      return m;
    }

    /* Suppress registry re-entry from allocations performed by the registry
     * itself (only relevant if a build overrides global operator new with
     * snmalloc; in the task build the registry uses the system allocator). */
    static thread_local bool t_busy = false;
    struct Busy
    {
      bool prev;
      Busy() noexcept : prev(t_busy)
      {
        t_busy = true;
      }
      ~Busy()
      {
        t_busy = prev;
      }
    };

    struct Geom
    {
      uintptr_t addr = 0;
      uintptr_t slab = 0;
      size_t sc_raw = 0;
      size_t slab_bytes = 0;
      size_t obj_size = 0;
      size_t ps = 0;
      int first_page = 0;
      int last_page = 0;
      int npages = 0;
      Meta* meta = nullptr;
      void* owner_key = nullptr;
    };

    static bool geom_for(uintptr_t addr, Geom& g) noexcept
    {
      auto& entry = Backend::get_metaentry(static_cast<address_t>(addr));
      if (entry.is_backend_owned())
        return false;
      auto scf = entry.get_sizeclass();
      if (!scf.is_small())
        return false;
      auto sc = scf.as_small();
      size_t slab_bytes = sizeclass_to_slab_size(sc);
      size_t obj_size = sizeclass_to_size(sc);
      size_t ps = os_page_size();
      uintptr_t slab = addr - (addr % slab_bytes);
      int npages = static_cast<int>(slab_bytes / ps);
      if (npages <= 0)
        return false;
      int first = static_cast<int>((addr - slab) / ps);
      int last =
        static_cast<int>((addr + obj_size - 1 - slab) / ps);
      if (first < 0 || first >= npages)
        return false;
      if (last >= npages)
        last = npages - 1; /* clamp: edge geometry still tracks overlap */
      g.addr = addr;
      g.slab = slab;
      g.sc_raw = sc.raw;
      g.slab_bytes = slab_bytes;
      g.obj_size = obj_size;
      g.ps = ps;
      g.first_page = first;
      g.last_page = last < first ? first : last;
      g.npages = npages;
      g.meta = entry.get_slab_metadata();
      g.owner_key = static_cast<void*>(entry.get_remote());
      return g.meta != nullptr;
    }

    /* Caller holds mu() and Busy. */
    static SlabRec* get_or_create(const Geom& g)
    {
      auto& all = slabs();
      auto it = all.find(g.slab);
      SlabRec* rec;
      if (it == all.end())
      {
        auto up = std::make_unique<SlabRec>();
        rec = up.get();
        rec->slab = g.slab;
        rec->meta = static_cast<void*>(g.meta);
        rec->owner_key = g.owner_key;
        rec->sc_raw = g.sc_raw;
        rec->slab_bytes = g.slab_bytes;
        rec->obj_size = g.obj_size;
        rec->ps = g.ps;
        rec->npages = g.npages;
        rec->outstanding.assign(g.npages, 0);
        rec->shielded.assign(g.npages, 0);
        meta_index()[rec->meta] = g.slab;
        all[g.slab] = std::move(up);
      }
      else
      {
        rec = it->second.get();
        /* Backend may have reused the address for a different metadata. */
        if (rec->meta != static_cast<void*>(g.meta))
        {
          meta_index().erase(rec->meta);
          rec->meta = static_cast<void*>(g.meta);
          meta_index()[rec->meta] = g.slab;
        }
        rec->owner_key = g.owner_key;
      }
      return rec;
    }

    static void adjust(const void* p, int delta) noexcept
    {
      if (p == nullptr || t_busy)
        return;
      Busy busy;
      uintptr_t addr = reinterpret_cast<uintptr_t>(p);
      Geom g;
      if (!geom_for(addr, g))
        return;
      std::lock_guard<std::mutex> lk(mu());
      SlabRec* rec = get_or_create(g);
      rec->live += delta;
      if (rec->live < 0)
        rec->live = 0;
      for (int i = g.first_page; i <= g.last_page; i++)
      {
        rec->outstanding[i] += delta;
        if (rec->outstanding[i] < 0)
          rec->outstanding[i] = 0;
      }
    }

    /* Re-thread parked nodes into meta.free_queue.  Caller holds mu(). */
    static void rethread_parked(
      SlabRec& rec, Meta* meta, LocalEntropy* entropy) noexcept
    {
      if (rec.parked.empty())
        return;
      LocalEntropy fallback_entropy; /* all-zero bits -> deterministic list 0 */
      LocalEntropy& ent = entropy != nullptr ? *entropy : fallback_entropy;
      const auto& key = freelist::Object::key_root;
      address_t tweak = meta->as_key_tweak();
      for (uintptr_t a : rec.parked)
      {
        auto hp = freelist::HeadPtr::unsafe_from(
          reinterpret_cast<freelist::Object::T<>*>(a));
        meta->free_queue.add(hp, key, tweak, ent);
      }
      rec.parked.clear();
    }

    /* Make every shielded page of this slab accessible again. */
    static void unshield_all_locked(
      Alloc& alloc, SlabRec& rec, LocalEntropy* entropy) noexcept
    {
      for (int i = 0; i < rec.npages; i++)
      {
        if (rec.shielded[i])
        {
          uintptr_t page = rec.slab + static_cast<uintptr_t>(i) * rec.ps;
          protect_or_die(page, rec.ps, PROT_READ | PROT_WRITE);
          rec.shielded[i] = 0;
        }
      }
      Meta* meta = static_cast<Meta*>(rec.meta);
      rethread_parked(rec, meta, entropy);
      (void)alloc;
    }

    /*
     * Restore exactly the pages spanned by one parked slot and re-thread all
     * parked slots whose whole span is now accessible.  Returns true if at
     * least one slot was re-threaded.
     */
    static bool unpark_minimal_locked(
      SlabRec& rec, Meta* meta, LocalEntropy* entropy) noexcept
    {
      if (rec.parked.empty())
        return false;

      uintptr_t chosen = rec.parked.front();
      int f = static_cast<int>((chosen - rec.slab) / rec.ps);
      int l = static_cast<int>(
        (chosen + rec.obj_size - 1 - rec.slab) / rec.ps);
      if (l >= rec.npages)
        l = rec.npages - 1;

      for (int i = f; i <= l; i++)
      {
        if (i >= 0 && i < rec.npages && rec.shielded[i])
        {
          uintptr_t page = rec.slab + static_cast<uintptr_t>(i) * rec.ps;
          protect_or_die(page, rec.ps, PROT_READ | PROT_WRITE);
          rec.shielded[i] = 0;
        }
      }

      std::vector<uintptr_t> keep_parked;
      bool rethreaded = false;
      for (uintptr_t a : rec.parked)
      {
        int af = static_cast<int>((a - rec.slab) / rec.ps);
        int al = static_cast<int>(
          (a + rec.obj_size - 1 - rec.slab) / rec.ps);
        if (al >= rec.npages)
          al = rec.npages - 1;
        bool accessible = true;
        for (int i = af; i <= al; i++)
        {
          if (i < 0 || i >= rec.npages || rec.shielded[i])
          {
            accessible = false;
            break;
          }
        }
        if (accessible)
        {
          auto hp = freelist::HeadPtr::unsafe_from(
            reinterpret_cast<freelist::Object::T<>*>(a));
          LocalEntropy fallback;
          meta->free_queue.add(
            hp,
            freelist::Object::key_root,
            meta->as_key_tweak(),
            entropy != nullptr ? *entropy : fallback);
          rethreaded = true;
        }
        else
        {
          keep_parked.push_back(a);
        }
      }
      rec.parked.swap(keep_parked);
      return rethreaded;
    }
  } // namespace ps_detail

  /* ---------------- public hooks ---------------- */

  void page_shield_hook_alloc_address(const void* p) noexcept
  {
    ps_detail::adjust(p, +1);
  }

  void page_shield_hook_free_address(const void* p) noexcept
  {
    ps_detail::adjust(p, -1);
  }

  void page_shield_hook_pre_free_list(void* meta_v, void* entropy_v) noexcept
  {
    if (meta_v == nullptr || ps_detail::t_busy)
      return;
    ps_detail::Busy busy;
    std::lock_guard<std::mutex> lk(ps_detail::mu());
    auto& idx = ps_detail::meta_index();
    auto it = idx.find(meta_v);
    if (it == idx.end())
      return;
    auto sit = ps_detail::slabs().find(it->second);
    if (sit == ps_detail::slabs().end())
      return;
    auto& rec = *sit->second;
    auto* meta = static_cast<ps_detail::Meta*>(meta_v);
    if (!meta->free_queue.empty())
      return; /* unprotected queued slots are used first */
    if (rec.parked.empty())
      return;
    auto* entropy = static_cast<LocalEntropy*>(entropy_v);
    ps_detail::unpark_minimal_locked(rec, meta, entropy);
  }

  void* page_shield_hook_parked_meta(void* alloc_v, size_t sc_raw) noexcept
  {
    if (alloc_v == nullptr || ps_detail::t_busy)
      return nullptr;
    ps_detail::Busy busy;
    auto& alloc = *static_cast<Alloc*>(alloc_v);
    void* owner = alloc.page_shield_owner_key();
    std::lock_guard<std::mutex> lk(ps_detail::mu());

    for (auto& kv : ps_detail::slabs())
    {
      auto& rec = *kv.second;
      if (rec.owner_key != owner || rec.sc_raw != sc_raw)
        continue;
      if (rec.parked.empty() || rec.meta == nullptr)
        continue;

      /* Confirm the slab is still front-end owned. */
      auto& entry =
        ps_detail::Backend::get_metaentry(static_cast<address_t>(rec.slab));
      if (entry.is_backend_owned() ||
          entry.get_slab_metadata() != rec.meta)
        continue;

      auto* meta = static_cast<ps_detail::Meta*>(rec.meta);
      if (meta->free_queue.empty())
      {
        if (!ps_detail::unpark_minimal_locked(rec, meta, nullptr))
          continue;
      }

      /* Length-zero cache means this metadata is sleeping in `laden`;
       * detach its intrusive node so small_refill can enqueue it.  If the
       * state unexpectedly says awake, leave the set linkage alone. */
      if (!meta->is_sleeping())
        continue;
      meta->node.remove();
      meta->set_not_sleeping(static_cast<smallsizeclass_t>(sc_raw));
      return rec.meta;
    }
    return nullptr;
  }

  void page_shield_hook_clear_slab(void* meta_v, void* entropy_v) noexcept
  {
    if (meta_v == nullptr || ps_detail::t_busy)
      return;
    ps_detail::Busy busy;
    std::lock_guard<std::mutex> lk(ps_detail::mu());
    auto& idx = ps_detail::meta_index();
    auto it = idx.find(meta_v);
    if (it == idx.end())
      return;
    uintptr_t slab = it->second;
    auto sit = ps_detail::slabs().find(slab);
    if (sit == ps_detail::slabs().end())
      return;
    auto& rec = *sit->second;
    auto* meta = static_cast<ps_detail::Meta*>(meta_v);
    auto* entropy = static_cast<LocalEntropy*>(entropy_v);

    for (int i = 0; i < rec.npages; i++)
    {
      if (rec.shielded[i])
      {
        uintptr_t page = rec.slab + static_cast<uintptr_t>(i) * rec.ps;
        ps_detail::protect_or_die(
          page, rec.ps, PROT_READ | PROT_WRITE);
        rec.shielded[i] = 0;
      }
    }
    ps_detail::rethread_parked(rec, meta, entropy);

    idx.erase(meta_v);
    ps_detail::slabs().erase(sit);
  }

  size_t page_shield_release(Alloc& alloc) noexcept
  {
    alloc.page_shield_safe_flush();

    ps_detail::Busy busy;
    void* owner = alloc.page_shield_owner_key();
    std::lock_guard<std::mutex> lk(ps_detail::mu());
    size_t restored_pages = 0;

    for (auto& kv : ps_detail::slabs())
    {
      auto& rec = *kv.second;
      if (rec.owner_key != owner)
        continue;
      for (int i = 0; i < rec.npages; i++)
      {
        if (rec.shielded[i])
        {
          uintptr_t page = rec.slab + static_cast<uintptr_t>(i) * rec.ps;
          ps_detail::protect_or_die(
            page, rec.ps, PROT_READ | PROT_WRITE);
          rec.shielded[i] = 0;
          restored_pages++;
        }
      }
      if (!rec.parked.empty())
      {
        auto* meta = static_cast<ps_detail::Meta*>(rec.meta);
        ps_detail::rethread_parked(rec, meta, nullptr);
      }
    }
    return restored_pages * ps_detail::os_page_size();
  }

  size_t page_shield_collect(Alloc& alloc, size_t max_bytes) noexcept
  {
    /* Collect published remote frees and consolidate fast lists into
     * meta.free_queue before taking the registry lock. */
    alloc.page_shield_safe_flush();

    ps_detail::Busy busy;
    const size_t ps = ps_detail::os_page_size();
    const size_t budget_pages = max_bytes / ps;
    void* owner = alloc.page_shield_owner_key();

    std::lock_guard<std::mutex> lk(ps_detail::mu());

    struct Cand
    {
      uintptr_t addr;
      ps_detail::SlabRec* rec;
      int idx;
    };
    std::vector<Cand> candidates;

    for (auto& kv : ps_detail::slabs())
    {
      auto& rec = *kv.second;
      if (rec.owner_key != owner)
        continue;

      /* Drop stale records for slabs the backend has since reclaimed. */
      auto& entry = ps_detail::Backend::get_metaentry(
        static_cast<address_t>(rec.slab));
      if (entry.is_backend_owned() ||
          entry.get_slab_metadata() != rec.meta)
      {
        continue;
      }

      /* Already protected pages always participate in budget reconciliation,
       * even if our live count has since dropped to zero. */
      bool any_shielded = false;
      for (int i = 0; i < rec.npages; i++)
        if (rec.shielded[i])
          any_shielded = true;

      /* Only partial slabs that still have live objects are new collection
       * candidates; completely unused slabs return through the backend. */
      if (rec.live <= 0 && !any_shielded)
        continue;

      for (int i = 0; i < rec.npages; i++)
      {
        if (rec.outstanding[i] == 0 || rec.shielded[i])
        {
          candidates.push_back(
            {rec.slab + static_cast<uintptr_t>(i) * ps, &rec, i});
        }
      }
    }

    std::sort(
      candidates.begin(), candidates.end(), [](const Cand& a, const Cand& b) {
        return a.addr < b.addr;
      });

    /* Mark the globally retained pages.  Everything else is restored. */
    std::map<ps_detail::SlabRec*, std::vector<unsigned char>> desired;
    for (size_t rank = 0; rank < candidates.size(); rank++)
    {
      Cand& c = candidates[rank];
      auto& mask = desired[c.rec];
      if (mask.empty())
        mask.assign(c.rec->npages, 0);
      if (rank < budget_pages)
        mask[c.idx] = 1;
    }

    size_t protected_pages = 0;

    for (auto& dkv : desired)
    {
      ps_detail::SlabRec& rec = *dkv.first;
      std::vector<unsigned char>& want = dkv.second;
      auto* meta = static_cast<ps_detail::Meta*>(rec.meta);

      bool same = true;
      for (int i = 0; i < rec.npages; i++)
      {
        if (want[i] != rec.shielded[i])
        {
          same = false;
          break;
        }
      }
      if (same)
      {
        for (int i = 0; i < rec.npages; i++)
          if (rec.shielded[i])
            protected_pages++;
        continue;
      }

      /* Full unshield + re-thread, then re-park for the desired set. */
      for (int i = 0; i < rec.npages; i++)
      {
        if (rec.shielded[i])
        {
          uintptr_t page = rec.slab + static_cast<uintptr_t>(i) * ps;
          ps_detail::protect_or_die(
            page, ps, PROT_READ | PROT_WRITE);
          rec.shielded[i] = 0;
        }
      }
      ps_detail::rethread_parked(rec, meta, nullptr);

      /* Drain the whole queue. */
      auto ls = &alloc.get_backend_local_state();
      auto domesticate = [ls](freelist::QueuePtr q) {
        return capptr_domesticate<Config>(ls, q);
      };
      const auto& key = freelist::Object::key_root;
      address_t tweak = meta->as_key_tweak();

      std::vector<uintptr_t> nodes;
      while (true)
      {
        freelist::Iter<> it;
        uint16_t remaining = meta->free_queue.close(it, key, tweak);
        while (!it.empty())
        {
          auto h = it.take(key, domesticate);
          nodes.push_back(reinterpret_cast<uintptr_t>(h.unsafe_ptr()));
        }
        if (remaining == 0)
          break;
      }
      meta->free_queue.init(
        static_cast<address_t>(rec.slab), key, tweak);

      std::vector<uintptr_t> new_parked;
      for (uintptr_t a : nodes)
      {
        int f = static_cast<int>((a - rec.slab) / ps);
        int l = static_cast<int>(
          (a + rec.obj_size - 1 - rec.slab) / ps);
        if (l >= rec.npages)
          l = rec.npages - 1;
        bool park = false;
        for (int i = f; i <= l; i++)
        {
          if (i >= 0 && i < rec.npages && want[i])
          {
            park = true;
            break;
          }
        }
        if (park)
        {
          new_parked.push_back(a);
        }
        else
        {
          auto hp = freelist::HeadPtr::unsafe_from(
            reinterpret_cast<freelist::Object::T<>*>(a));
          LocalEntropy zero_entropy;
          meta->free_queue.add(hp, key, tweak, zero_entropy);
        }
      }

      /* Protect desired pages only after all links are rebuilt. */
      for (int i = 0; i < rec.npages; i++)
      {
        if (want[i])
        {
          uintptr_t page = rec.slab + static_cast<uintptr_t>(i) * ps;
          ps_detail::protect_or_die(page, ps, PROT_NONE);
          rec.shielded[i] = 1;
          protected_pages++;
        }
      }
      rec.parked.swap(new_parked);
    }

    return protected_pages * ps;
  }

  bool page_shield_query(
    Alloc& alloc, uintptr_t address, page_shield_info& out) noexcept
  {
    ps_detail::Busy busy;
    const size_t ps = ps_detail::os_page_size();

    ps_detail::Geom g;
    if (!ps_detail::geom_for(address, g))
      return false;

    void* owner = alloc.page_shield_owner_key();
    if (g.owner_key != owner)
      return false;

    std::lock_guard<std::mutex> lk(ps_detail::mu());
    auto& all = ps_detail::slabs();
    auto it = all.find(g.slab);

    int idx = static_cast<int>((address - g.slab) / ps);
    size_t outstanding = 0;
    bool shielded_flag = false;
    if (it != all.end())
    {
      auto& rec = *it->second;
      if (idx >= 0 && idx < rec.npages)
      {
        outstanding = static_cast<size_t>(rec.outstanding[idx]);
        shielded_flag = rec.shielded[idx] != 0;
      }
    }

    uintptr_t page = (address / ps) * ps;
    int perm = ps_detail::os_permission(page);

    page_shield_state state;
    if (outstanding > 0)
      state = page_shield_state::active;
    else if (perm >= 0)
      state = ((perm & 1) == 0) ? page_shield_state::shielded
                                : page_shield_state::free;
    else
      state = shielded_flag ? page_shield_state::shielded
                            : page_shield_state::free;

    out.page = page;
    out.slab = g.slab;
    out.page_bytes = ps;
    out.outstanding = outstanding;
    out.state = state;
    return true;
  }
} // namespace snmalloc

#endif /* SNMALLOC_ENABLE_PAGE_SHIELD */
