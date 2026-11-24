#include "libhfuzz/covmerge.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define HF_COVMERGE_MAGIC UINT64_C(0x4846434d52473101)
#define HF_COVMERGE_VERSION UINT32_C(1)
#define HF_COVMERGE_MAX_RANGES 256

/* Called from instrument8BitCountersCount(). */
void hfCovMergeInstrumentHarvest(void);

typedef struct {
    uint64_t id_hi;
    uint64_t id_lo;
    uint32_t length;
    uint32_t reserved;
    uint8_t  counts[HF_COVMERGE_MODULE_POSITIONS];
} hf_covmerge_module_t;

typedef struct {
    atomic_uint next_ticket;
    atomic_uint now_serving;
} hf_covmerge_lock_t;

typedef struct {
    uint64_t             magic;
    uint32_t             version;
    uint32_t             layout_size;
    _Atomic uint64_t     epoch;
    hf_covmerge_lock_t   table_lock;
    atomic_uint          ready;
    uint32_t             module_count;
    uint32_t             reserved;
    hf_covmerge_module_t modules[HF_COVMERGE_MAX_MODULES];
} hf_covmerge_shared_t;

typedef struct {
    char    *start;
    char    *end;
    uint64_t id_hi;
    uint64_t id_lo;
    size_t   length;
    uint32_t module_index;
    bool     used;
} hf_covmerge_range_t;

static struct {
    hf_covmerge_shared_t *shared;
    size_t                bytes;
    uint64_t              begin_epoch;
    bool                  armed;
    hf_covmerge_range_t    ranges[HF_COVMERGE_MAX_RANGES];
} hf_covmerge_local;

static void hfCovMergeLockInit(hf_covmerge_lock_t *lock) {
    atomic_init(&lock->next_ticket, 0U);
    atomic_init(&lock->now_serving, 0U);
}

static void hfCovMergeLock(hf_covmerge_lock_t *lock) {
    unsigned ticket =
        atomic_fetch_add_explicit(&lock->next_ticket, 1U, memory_order_relaxed);
    while (atomic_load_explicit(&lock->now_serving, memory_order_acquire) != ticket) {
        /* Contention here is short and confined to benign local experiments. */
    }
}

static void hfCovMergeUnlock(hf_covmerge_lock_t *lock) {
    unsigned current =
        atomic_load_explicit(&lock->now_serving, memory_order_relaxed);
    atomic_store_explicit(&lock->now_serving, current + 1U, memory_order_release);
}

static bool hfCovMergeStorageValid(
    void *memory, size_t bytes, hf_covmerge_shared_t **shared) {
    if (memory == NULL || shared == NULL) {
        return false;
    }
    if (bytes < hfCovMergeSize()) {
        return false;
    }
    if (((uintptr_t)memory & (_Alignof(hf_covmerge_shared_t) - 1U)) != 0U) {
        return false;
    }

    hf_covmerge_shared_t *candidate = (hf_covmerge_shared_t *)memory;
    if (candidate->magic != HF_COVMERGE_MAGIC ||
        candidate->version != HF_COVMERGE_VERSION ||
        candidate->layout_size != (uint32_t)sizeof(hf_covmerge_shared_t)) {
        return false;
    }
    if (atomic_load_explicit(&candidate->ready, memory_order_acquire) != 1U) {
        return false;
    }
    if (atomic_load_explicit(&candidate->epoch, memory_order_acquire) == 0U) {
        return false;
    }

    *shared = candidate;
    return true;
}

static hf_covmerge_module_t *hfCovMergeFindModuleLocked(
    hf_covmerge_shared_t *shared, uint64_t id_hi, uint64_t id_lo,
    uint32_t *index_out) {
    for (uint32_t i = 0; i < shared->module_count; i++) {
        hf_covmerge_module_t *module = &shared->modules[i];
        if (module->id_hi == id_hi && module->id_lo == id_lo) {
            if (index_out != NULL) {
                *index_out = i;
            }
            return module;
        }
    }
    return NULL;
}

size_t hfCovMergeSize(void) {
    /*
     * Round the layout to the mapping granularity so callers can mmap/munmap
     * exactly the advertised number of bytes.  The structure itself needs
     * only its inherent alignment; MAP_SHARED mappings are page aligned.
     */
    long page_size = sysconf(_SC_PAGESIZE);
    size_t alignment = (page_size > 0) ? (size_t)page_size : 4096U;
    return (sizeof(hf_covmerge_shared_t) + alignment - 1U) & ~(alignment - 1U);
}

bool hfCovMergeInit(void *memory, size_t bytes, uint64_t epoch) {
    if (memory == NULL || epoch == 0U || bytes < hfCovMergeSize()) {
        return false;
    }
    if (((uintptr_t)memory & (_Alignof(hf_covmerge_shared_t) - 1U)) != 0U) {
        return false;
    }

    hf_covmerge_shared_t *shared = (hf_covmerge_shared_t *)memory;

    /* Initialization is a one-shot operation.  Do not overwrite a live region. */
    if (shared->magic == HF_COVMERGE_MAGIC &&
        shared->version == HF_COVMERGE_VERSION &&
        shared->layout_size == (uint32_t)sizeof(hf_covmerge_shared_t) &&
        atomic_load_explicit(&shared->ready, memory_order_acquire) == 1U) {
        return false;
    }

    memset(shared, 0, sizeof(*shared));
    shared->magic       = HF_COVMERGE_MAGIC;
    shared->version     = HF_COVMERGE_VERSION;
    shared->layout_size = (uint32_t)sizeof(*shared);
    atomic_init(&shared->epoch, 0U);
    atomic_init(&shared->ready, 0U);
    hfCovMergeLockInit(&shared->table_lock);
    shared->module_count = 0;

    /* Publish the initialized header only after all of its fields are ready. */
    atomic_store_explicit(&shared->epoch, epoch, memory_order_release);
    atomic_store_explicit(&shared->ready, 1U, memory_order_release);

    return true;
}

bool hfCovMergeAttach(void *memory, size_t bytes) {
    hf_covmerge_shared_t *shared = NULL;
    if (!hfCovMergeStorageValid(memory, bytes, &shared)) {
        return false;
    }

    /* Attach establishes this process's fresh, thread-local-ish range table. */
    memset(&hf_covmerge_local, 0, sizeof(hf_covmerge_local));
    hf_covmerge_local.shared = shared;
    hf_covmerge_local.bytes  = bytes;
    return true;
}

void hfCovMergeDetach(void) {
    /* The mapping is owned by the caller; only forget the local association. */
    memset(&hf_covmerge_local, 0, sizeof(hf_covmerge_local));
}

bool hfCovMergeBind(
    char *start, char *end, uint64_t id_hi, uint64_t id_lo) {
    hf_covmerge_shared_t *shared = hf_covmerge_local.shared;
    if (shared == NULL || start == NULL || end == NULL) {
        return false;
    }
    if (id_hi == 0U && id_lo == 0U) {
        return false;
    }

    uintptr_t start_addr = (uintptr_t)start;
    uintptr_t end_addr   = (uintptr_t)end;
    if (end_addr <= start_addr) {
        return false;
    }

    size_t length = (size_t)(end_addr - start_addr);
    if (length == 0U || length > HF_COVMERGE_MODULE_POSITIONS) {
        return false;
    }

    int exact_index = -1;
    for (size_t i = 0; i < HF_COVMERGE_MAX_RANGES; i++) {
        hf_covmerge_range_t *range = &hf_covmerge_local.ranges[i];
        if (!range->used) {
            continue;
        }
        if (range->start == start && range->end == end) {
            exact_index = (int)i;
            break;
        }
    }

    if (exact_index >= 0) {
        hf_covmerge_range_t *range = &hf_covmerge_local.ranges[exact_index];
        if (range->id_hi != id_hi || range->id_lo != id_lo) {
            return false;
        }

        hfCovMergeLock(&shared->table_lock);
        hf_covmerge_module_t *module = hfCovMergeFindModuleLocked(
            shared, id_hi, id_lo, NULL);
        bool ok = module != NULL && module->length == (uint32_t)length;
        hfCovMergeUnlock(&shared->table_lock);
        return ok;
    }

    /* Reject any partial or complete overlap with another local range. */
    for (size_t i = 0; i < HF_COVMERGE_MAX_RANGES; i++) {
        hf_covmerge_range_t *range = &hf_covmerge_local.ranges[i];
        if (!range->used) {
            continue;
        }
        uintptr_t range_start = (uintptr_t)range->start;
        uintptr_t range_end   = (uintptr_t)range->end;
        if (start_addr < range_end && range_start < end_addr) {
            return false;
        }
    }

    size_t free_index = HF_COVMERGE_MAX_RANGES;
    for (size_t i = 0; i < HF_COVMERGE_MAX_RANGES; i++) {
        if (!hf_covmerge_local.ranges[i].used) {
            free_index = i;
            break;
        }
    }
    if (free_index == HF_COVMERGE_MAX_RANGES) {
        return false;
    }

    hfCovMergeLock(&shared->table_lock);

    uint32_t module_index = UINT32_MAX;
    hf_covmerge_module_t *module =
        hfCovMergeFindModuleLocked(shared, id_hi, id_lo, &module_index);

    if (module != NULL) {
        if (module->length != (uint32_t)length) {
            hfCovMergeUnlock(&shared->table_lock);
            return false;
        }
    } else {
        if (shared->module_count >= HF_COVMERGE_MAX_MODULES) {
            hfCovMergeUnlock(&shared->table_lock);
            return false;
        }

        module_index = shared->module_count++;
        module = &shared->modules[module_index];
        module->id_hi  = id_hi;
        module->id_lo  = id_lo;
        module->length = (uint32_t)length;
        module->reserved = 0;
        memset(module->counts, 0, sizeof(module->counts));
    }

    hf_covmerge_range_t *range = &hf_covmerge_local.ranges[free_index];
    range->start          = start;
    range->end            = end;
    range->id_hi          = id_hi;
    range->id_lo          = id_lo;
    range->length         = length;
    range->module_index   = module_index;
    range->used           = true;

    hfCovMergeUnlock(&shared->table_lock);
    return true;
}

uint64_t hfCovMergeBegin(void) {
    hf_covmerge_shared_t *shared = hf_covmerge_local.shared;
    if (shared == NULL) {
        return 0U;
    }

    /* Begin owns the local ranges exclusively; clear the previous local round. */
    for (size_t i = 0; i < HF_COVMERGE_MAX_RANGES; i++) {
        hf_covmerge_range_t *range = &hf_covmerge_local.ranges[i];
        if (range->used) {
            memset(range->start, 0, range->length);
        }
    }

    hfCovMergeLock(&shared->table_lock);
    uint64_t epoch = atomic_load_explicit(&shared->epoch, memory_order_acquire);
    hf_covmerge_local.begin_epoch = epoch;
    hf_covmerge_local.armed       = true;
    hfCovMergeUnlock(&shared->table_lock);

    return epoch;
}

bool hfCovMergeAdvance(uint64_t epoch) {
    hf_covmerge_shared_t *shared = hf_covmerge_local.shared;
    if (shared == NULL || epoch == 0U) {
        return false;
    }

    hfCovMergeLock(&shared->table_lock);
    uint64_t current = atomic_load_explicit(&shared->epoch, memory_order_acquire);
    if (epoch <= current) {
        hfCovMergeUnlock(&shared->table_lock);
        return false;
    }

    /* Clear values only; id_hi/id_lo/length are the stable module identities. */
    for (uint32_t i = 0; i < HF_COVMERGE_MAX_MODULES; i++) {
        memset(shared->modules[i].counts, 0,
            sizeof(shared->modules[i].counts));
    }
    atomic_store_explicit(&shared->epoch, epoch, memory_order_release);
    hfCovMergeUnlock(&shared->table_lock);
    return true;
}

bool hfCovMergeRead(
    uint64_t id_hi, uint64_t id_lo, size_t index, uint8_t *out) {
    hf_covmerge_shared_t *shared = hf_covmerge_local.shared;
    if (shared == NULL || out == NULL || (id_hi == 0U && id_lo == 0U)) {
        return false;
    }

    hfCovMergeLock(&shared->table_lock);
    hf_covmerge_module_t *module =
        hfCovMergeFindModuleLocked(shared, id_hi, id_lo, NULL);
    if (module == NULL || index >= module->length) {
        hfCovMergeUnlock(&shared->table_lock);
        return false;
    }

    uint8_t value = module->counts[index];
    hfCovMergeUnlock(&shared->table_lock);

    *out = value;
    return true;
}

void hfCovMergeInstrumentHarvest(void) {
    hf_covmerge_shared_t *shared = hf_covmerge_local.shared;
    if (shared == NULL || !hf_covmerge_local.armed) {
        return;
    }

    /* Only the first instrument8BitCountersCount() after Begin contributes. */
    hf_covmerge_local.armed = false;

    hfCovMergeLock(&shared->table_lock);

    /*
     * Epoch validation and every saturation update happen while holding the
     * same table lock.  Advance therefore cannot interleave a stale check with
     * a write or observe a partially merged round.
     */
    uint64_t epoch = atomic_load_explicit(&shared->epoch, memory_order_acquire);
    if (epoch != hf_covmerge_local.begin_epoch) {
        hfCovMergeUnlock(&shared->table_lock);
        return;
    }

    for (size_t i = 0; i < HF_COVMERGE_MAX_RANGES; i++) {
        hf_covmerge_range_t *range = &hf_covmerge_local.ranges[i];
        if (!range->used) {
            continue;
        }

        hf_covmerge_module_t *module = &shared->modules[range->module_index];
        for (size_t j = 0; j < range->length; j++) {
            unsigned int sum =
                (unsigned int)module->counts[j] +
                (unsigned int)(uint8_t)range->start[j];
            module->counts[j] =
                (sum >= UINT8_MAX) ? UINT8_MAX : (uint8_t)sum;
        }
    }

    hfCovMergeUnlock(&shared->table_lock);

    /*
     * instrument8BitCountersCount() performs the existing per-counter clear
     * immediately after this hook, preserving the legacy bucket feedback.
     */
}
