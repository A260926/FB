/*
 * Contract checks for SNMALLOC_ENABLE_PAGE_SHIELD.
 *
 * Registers three groups with task_support/check.h:
 *   geometry   - alignment, query ownership/diagnostics, cross-page objects
 *   recycling  - protection, budgets, fast-list/refill reuse, sized/realloc,
 *                full return and backend reuse
 *   handoff    - remote send/receive phases, thread exit and pool hand-off
 *
 * Every check uses only legal allocations/frees/lifetime operations and OS
 * permission queries; nothing dereferences freed memory or feeds corrupt
 * input to the allocator.
 */

#include "task_support/check.h"
#include "task_support/page_observer.h"

#include <snmalloc/page_shield.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using snmalloc::page_shield_collect;
using snmalloc::page_shield_info;
using snmalloc::page_shield_query;
using snmalloc::page_shield_release;
using snmalloc::page_shield_state;

namespace
{
size_t page_size()
{
  return size_t(sysconf(_SC_PAGESIZE));
}

/* Count pages currently reported as shielded for a whole fixture slab. */
int count_shielded_pages(task_input::fixture& f)
{
  int n = 0;
  size_t ps = f.page_bytes;
  for (uintptr_t p = f.slab; p < f.slab + f.slab_bytes; p += ps)
  {
    page_shield_info info{};
    if (page_shield_query(*f.owner.alloc, p, info) &&
        info.state == page_shield_state::shielded)
      n++;
  }
  return n;
}
} // namespace

/* ============================ geometry ============================ */

static int geom_query_fields()
{
  task_input::fixture f;
  TASK_CHECK(f.init());
  TASK_CHECK(f.retire_remote());
  f.owner->flush();

  page_shield_info info{};
  bool q = page_shield_query(*f.owner.alloc, f.page, info);
  TASK_CHECK(q);
  TASK_CHECK(info.page == f.page);
  TASK_CHECK(info.slab == f.slab);
  TASK_CHECK(info.page_bytes == f.page_bytes);
  TASK_CHECK(info.page_bytes == page_size());
  TASK_CHECK((info.page % info.page_bytes) == 0);
  TASK_CHECK((info.slab % f.slab_bytes) == 0);
  TASK_CHECK(info.outstanding == 0);
  TASK_CHECK(info.state != page_shield_state::active);

  /* A live sibling reports active on the page that contains it. */
  page_shield_info live{};
  TASK_CHECK(page_shield_query(*f.owner.alloc, f.survivor, live));
  TASK_CHECK(live.outstanding > 0);
  TASK_CHECK(live.state == page_shield_state::active);

  /* Foreign addresses return false and leave out untouched. */
  int stack_local = 0x41;
  page_shield_info sentinel{};
  sentinel.page = uintptr_t(0xAAAAAAAAAAAAAAAAULL);
  sentinel.slab = uintptr_t(0xBBBBBBBBBBBBBBBBULL);
  sentinel.page_bytes = 0xCCCC;
  sentinel.outstanding = 0xDDDD;
  sentinel.state = page_shield_state::shielded;
  bool fq = page_shield_query(
    *f.owner.alloc, reinterpret_cast<uintptr_t>(&stack_local), sentinel);
  TASK_CHECK(!fq);
  TASK_CHECK(sentinel.page == uintptr_t(0xAAAAAAAAAAAAAAAAULL));
  TASK_CHECK(sentinel.slab == uintptr_t(0xBBBBBBBBBBBBBBBBULL));
  TASK_CHECK(sentinel.page_bytes == 0xCCCC);
  TASK_CHECK(sentinel.outstanding == 0xDDDD);
  TASK_CHECK(sentinel.state == page_shield_state::shielded);

  TASK_CHECK(f.finish());
  return 0;
}

static int geom_cross_page_accounting()
{
  snmalloc::ScopedAllocator<> holder;
  auto& alloc = *holder.alloc;
  const size_t ps = page_size();
  const size_t sz = 3072; /* does not divide 16384 or 4096: slots drift */
  auto sc = snmalloc::size_to_sizeclass(sz);
  const size_t usable = snmalloc::sizeclass_to_size(sc);

  std::vector<void*> objs;
  void* crossing = nullptr;
  uintptr_t pa = 0, pb = 0;
  for (int i = 0; i < 4096 && crossing == nullptr; i++)
  {
    void* p = alloc.alloc(sz);
    TASK_CHECK(p != nullptr);
    objs.push_back(p);
    uintptr_t a = uintptr_t(p);
    uintptr_t start_page = a & ~uintptr_t(ps - 1);
    if (a + usable > start_page + ps)
    {
      crossing = p;
      pa = start_page;
      pb = start_page + ps;
    }
  }
  TASK_CHECK(crossing != nullptr);

  page_shield_info ba{}, bb{};
  TASK_CHECK(page_shield_query(alloc, pa, ba));
  TASK_CHECK(page_shield_query(alloc, pb, bb));
  size_t before_a = ba.outstanding;
  size_t before_b = bb.outstanding;
  TASK_CHECK(before_a >= 1);
  TASK_CHECK(before_b >= 1);

  alloc.dealloc(crossing);
  page_shield_info aa{}, ab{};
  TASK_CHECK(page_shield_query(alloc, pa, aa));
  TASK_CHECK(page_shield_query(alloc, pb, ab));
  TASK_CHECK(aa.outstanding == before_a - 1);
  TASK_CHECK(ab.outstanding == before_b - 1);

  for (void* p : objs)
  {
    if (p != crossing)
      alloc.dealloc(p);
  }
  alloc.flush();
  return 0;
}

/* =========================== recycling =========================== */

static int recycle_protection_and_live()
{
  task_input::fixture f;
  TASK_CHECK(f.init());
  TASK_CHECK(f.retire_remote());

  size_t held = page_shield_collect(*f.owner.alloc, 128u * 1024 * 1024);
  TASK_CHECK(held >= f.page_bytes);
  TASK_CHECK((held % f.page_bytes) == 0);

  page_shield_info info{};
  TASK_CHECK(page_shield_query(*f.owner.alloc, f.page, info));
  TASK_CHECK(info.state == page_shield_state::shielded);
  TASK_CHECK(info.outstanding == 0);
  TASK_CHECK(task_input::permissions(f.page) == 0);

  /* Live siblings stay readable/writable through a flush. */
  TASK_CHECK(f.live_contents());
  f.owner->flush();
  TASK_CHECK(task_input::permissions(f.page) == 0);
  TASK_CHECK(f.live_contents());

  TASK_CHECK(page_shield_release(*f.owner.alloc) == held);
  int perm = task_input::permissions(f.page);
  TASK_CHECK(perm >= 0 && (perm & 3) == 3);

  TASK_CHECK(f.finish());
  return 0;
}

static int recycle_budgets()
{
  task_input::fixture f;
  TASK_CHECK(f.init());
  TASK_CHECK(f.retire_remote());
  const size_t ps = f.page_bytes;

  /* A budget just below one whole page retains zero pages. */
  TASK_CHECK(page_shield_collect(*f.owner.alloc, ps - 1) == 0);
  TASK_CHECK(task_input::permissions(f.page) != 0);

  /* Unlimited shield, then tighten to a single retained page. */
  TASK_CHECK(page_shield_collect(*f.owner.alloc, 1u << 20) >= ps);
  size_t one = page_shield_collect(*f.owner.alloc, ps);
  TASK_CHECK(one == ps);
  TASK_CHECK(count_shielded_pages(f) == 1);

  /* Zero budget restores everything. */
  TASK_CHECK(page_shield_collect(*f.owner.alloc, 0) == 0);
  TASK_CHECK(count_shielded_pages(f) == 0);

  TASK_CHECK(f.finish());
  return 0;
}

static int recycle_fast_chain_refill()
{
  task_input::fixture f;
  TASK_CHECK(f.init());
  TASK_CHECK(f.retire_remote());
  TASK_CHECK(page_shield_collect(*f.owner.alloc, 128u * 1024 * 1024) >=
             f.page_bytes);
  TASK_CHECK(task_input::permissions(f.page) == 0);

  /*
   * Drain enough sizeclass slots to force the allocator through the rebuilt
   * free queue and the parked-slot restore path.  At least one returned
   * object must overlap the formerly inaccessible page.
   */
  bool reused = false;
  std::vector<void*> fresh;
  for (int i = 0; i < 256; i++)
  {
    void* p = f.owner->alloc(3072);
    TASK_CHECK(p != nullptr);
    memset(p, 0x6b, 3072);
    fresh.push_back(p);
    uintptr_t a = uintptr_t(p);
    if (a < f.page + f.page_bytes && f.page < a + f.usable)
      reused = true;
  }
  TASK_CHECK(reused);

  for (void* p : fresh)
  {
    volatile unsigned char* b = static_cast<unsigned char*>(p);
    for (size_t k = 0; k < 3072; k++)
      TASK_CHECK(b[k] == 0x6b);
  }
  for (void* p : fresh)
    f.owner->dealloc(p);
  f.owner->flush();

  /* Reuse may already have restored the needed page(s); release is then a
   * no-op, which is fine. */
  page_shield_release(*f.owner.alloc);
  TASK_CHECK(f.finish());
  return 0;
}

static int recycle_sized_and_realloc()
{
  /* Exercise the global sized-dealloc and realloc entry points while an
   * owner fixture is shielded; nothing may corrupt or abort. */
  task_input::fixture f;
  TASK_CHECK(f.init());
  TASK_CHECK(f.retire_remote());
  TASK_CHECK(page_shield_collect(*f.owner.alloc, 1u << 20) >= f.page_bytes);

  for (int i = 0; i < 200; i++)
  {
    size_t sz = 16 + (i * 131) % 4000;
    void* p = snmalloc::alloc(sz);
    TASK_CHECK(p != nullptr);
    memset(p, 0x33, sz);
    snmalloc::dealloc(p, sz); /* sized dealloc */
  }

  void* rp = snmalloc::libc::malloc(64);
  TASK_CHECK(rp != nullptr);
  memset(rp, 0x55, 64);
  for (int step = 0; step < 12; step++)
  {
    size_t ns = 32 + ((step * 7919u) % 5000);
    rp = snmalloc::libc::realloc(rp, ns);
    TASK_CHECK(rp != nullptr);
    memset(rp, 0x5a, ns < 32 ? ns : 32);
  }
  snmalloc::libc::free(rp);
  snmalloc::ThreadAlloc::get().flush();

  TASK_CHECK(f.live_contents());
  page_shield_release(*f.owner.alloc);
  TASK_CHECK(f.finish());
  return 0;
}

static int recycle_all_return_backend_reuse()
{
  task_input::fixture f;
  TASK_CHECK(f.init());
  TASK_CHECK(f.retire_remote());
  TASK_CHECK(page_shield_collect(*f.owner.alloc, 1u << 20) >= f.page_bytes);

  /* Return every remaining object without an explicit release.  The
   * clear_slab hook must restore our protections so original validation
   * passes and the slab returns to the backend. */
  for (void*& p : f.objects)
  {
    if (p != nullptr)
    {
      f.owner->dealloc(p);
      p = nullptr;
    }
  }
  f.owner->flush();
  bool empty = true;
  f.owner->debug_is_empty(&empty);
  TASK_CHECK(empty);

  /* Backend may protect the idle range itself; either way re-allocation on
   * this Alloc must keep working and the process must not abort. */
  std::vector<void*> more;
  for (int i = 0; i < 4096; i++)
  {
    void* p = f.owner->alloc(3072);
    TASK_CHECK(p != nullptr);
    memset(p, 0x17, 3072);
    more.push_back(p);
  }
  for (void* p : more)
    f.owner->dealloc(p);
  f.owner->flush();
  TASK_CHECK(f.finish());
  return 0;
}

/* ============================ handoff ============================ */

static int handoff_remote_send_receive()
{
  task_input::fixture f;
  TASK_CHECK(f.init());

  std::mutex m;
  std::condition_variable cv;
  std::atomic<bool> published{false};

  std::thread worker([&] {
    auto peer = snmalloc::get_scoped_allocator<>();
    for (void*& p : f.objects)
    {
      if (p != nullptr && f.touches_page(p))
      {
        peer->dealloc(p);
        p = nullptr;
        f.retired++;
      }
    }
    peer->flush(); /* publish to owner queue */
    {
      std::lock_guard<std::mutex> lk(m);
      published.store(true);
    }
    cv.notify_one();
  });

  {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return published.load(); });
  }
  worker.join();

  /* Sent and published, but not yet received by the owner: still active. */
  page_shield_info info{};
  TASK_CHECK(page_shield_query(*f.owner.alloc, f.page, info));
  TASK_CHECK(info.outstanding > 0);
  TASK_CHECK(info.state == page_shield_state::active);

  f.owner->flush(); /* receive */
  page_shield_info after{};
  TASK_CHECK(page_shield_query(*f.owner.alloc, f.page, after));
  TASK_CHECK(after.outstanding == 0);
  TASK_CHECK(after.state != page_shield_state::active);

  TASK_CHECK(f.finish());
  return 0;
}

static int handoff_thread_exit_and_pool()
{
  /* A short-lived thread exercises ThreadAlloc setup and teardown while a
   * shield exists on a pool Allocator; join orders completion. */
  snmalloc::Alloc* raw = nullptr;
  uintptr_t page = 0;
  uintptr_t slab = 0;
  size_t page_bytes = 0;

  {
    task_input::fixture f;
    TASK_CHECK(f.init());
    TASK_CHECK(f.retire_remote());
    TASK_CHECK(page_shield_collect(*f.owner.alloc, 1u << 20) >=
               f.page_bytes);

    std::atomic<int> thread_ok{0};
    std::thread t([&] {
      auto a = snmalloc::get_scoped_allocator<>();
      std::vector<void*> ps;
      for (int i = 0; i < 500; i++)
      {
        void* p = a->alloc(128 + (i % 512));
        if (p == nullptr)
          return;
        memset(p, 0x44, 128);
        ps.push_back(p);
      }
      for (void* p : ps)
        a->dealloc(p);
      a->flush();
      thread_ok.store(1);
    });
    t.join();
    TASK_CHECK(thread_ok.load() == 1);

    /* Shield on the main owner survived worker thread exit. */
    TASK_CHECK(task_input::permissions(f.page) == 0);
    TASK_CHECK(f.live_contents());

    raw = f.owner.alloc;
    page = f.page;
    slab = f.slab;
    page_bytes = f.page_bytes;

    /* Leak the live objects so the slab stays partial across pool return.
     * At block end ~ScopedAllocator flushes and pools this exact Alloc. */
    for (void*& p : f.objects)
      p = nullptr;
  }

  /* Retrieve that exact Alloc from another thread via the pool LIFO. */
  std::atomic<int> reacquire_ok{0};
  std::thread t2([&] {
    std::vector<snmalloc::Alloc*> ahead;
    snmalloc::Alloc* got = nullptr;
    for (int i = 0; i < 64; i++)
    {
      snmalloc::Alloc* x = snmalloc::AllocPool<snmalloc::Config>::acquire();
      if (x == raw)
      {
        got = x;
        break;
      }
      ahead.push_back(x);
    }
    if (got != nullptr)
    {
      page_shield_info info{};
      bool q = page_shield_query(*got, page, info);
      if (q && info.slab == slab &&
          page_shield_release(*got) >= page_bytes)
        reacquire_ok.store(1);
      snmalloc::AllocPool<snmalloc::Config>::release(got);
    }
    for (snmalloc::Alloc* x : ahead)
      snmalloc::AllocPool<snmalloc::Config>::release(x);
  });
  t2.join();
  TASK_CHECK(reacquire_ok.load() == 1);
  return 0;
}

/* ============================ registry ============================ */

const struct task_case task_cases[] = {
  {"geometry", "query_fields", geom_query_fields},
  {"geometry", "cross_page_accounting", geom_cross_page_accounting},
  {"recycling", "protection_and_live", recycle_protection_and_live},
  {"recycling", "budgets", recycle_budgets},
  {"recycling", "fast_chain_refill", recycle_fast_chain_refill},
  {"recycling", "sized_and_realloc", recycle_sized_and_realloc},
  {"recycling", "all_return_backend_reuse",
   recycle_all_return_backend_reuse},
  {"handoff", "remote_send_receive", handoff_remote_send_receive},
  {"handoff", "thread_exit_and_pool", handoff_thread_exit_and_pool},
};

const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
