/*
  Structural fragment reduction for afl-tmin
  ------------------------------------------

  The implementation below is deliberately self-contained: it only uses C and
  POSIX interfaces and does not depend on afl-tmin's global state.  The caller
  supplies the observation callback; afl-tmin wires that to its existing
  run_target().

  Reduction model
  ---------------
  The original bytes are cut into fragments described by a dependency table.
  Every active fragment is considered as a deletion candidate in ascending ID
  order.  Deleting a fragment also deletes every active fragment that directly
  or indirectly depends on it, including every member of a dependency cycle.
  After any acceptance the scan restarts at the smallest active ID.  A full
  pass that accepts nothing finishes the search.

  Candidate bytes are always the concatenation of the still-active fragments in
  original position order; bytes inside a fragment are never rewritten.  The
  result cache is keyed on the complete candidate byte string (never on a short
  hash alone) and stores both accepted and rejected outcomes.  Error outcomes
  are never cached and never advance the scan cursor.
*/

#define _POSIX_C_SOURCE 200809L

#include "afl-struct.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define STRUCT_STATE_MAGIC "AFLSTRUCT 1"

typedef struct {
  uint64_t id;
  size_t start;
  size_t end;
  uint64_t *needs;
  size_t nneeds;
} struct_frag;

typedef struct {
  uint64_t hash;
  unsigned char *bytes;
  size_t len;
  int result;
} struct_cache_entry;

struct afl_struct_session {
  unsigned char *orig;
  size_t orig_len;

  char *table;                 /* raw table copy (diagnostics)          */
  char *canon;                 /* canonical, order-independent table    */
  size_t canon_len;

  struct_frag *frags;
  size_t nfrags;

  size_t *by_id;               /* fragment indices sorted by ID         */
  size_t *by_start;            /* fragment indices sorted by position   */

  unsigned char *active;       /* one flag per fragment                 */
  size_t active_count;

  size_t cursor;               /* index into by_id of next candidate    */

  unsigned char *cur;          /* current accepted bytes                */
  size_t cur_len;

  struct_cache_entry *cache;
  size_t ncache;
  size_t cache_cap;
  size_t *slots;               /* open-addressed indices + 1; 0 = free  */
  size_t nslots;
};

/* ------------------------------------------------------------------ */
/* Small dynamic buffer                                               */
/* ------------------------------------------------------------------ */

typedef struct {
  unsigned char *p;
  size_t len;
  size_t cap;
} struct_dyn;

static int dyn_reserve(struct_dyn *b, size_t extra) {
  size_t need;
  unsigned char *np;

  if (extra > (size_t)-1 - b->len - 1) return -1;
  need = b->len + extra + 1;
  if (need <= b->cap) return 0;

  {
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) {
      if (cap > (size_t)-1 / 2) { cap = need; break; }
      cap *= 2;
    }
    np = (unsigned char *)realloc(b->p, cap);
    if (!np) return -1;
    b->p = np;
    b->cap = cap;
  }
  return 0;
}

static int dyn_append(struct_dyn *b, const void *data, size_t n) {
  if (n == 0) return 0;
  if (dyn_reserve(b, n)) return -1;
  memcpy(b->p + b->len, data, n);
  b->len += n;
  return 0;
}

static int dyn_putc(struct_dyn *b, unsigned char c) {
  return dyn_append(b, &c, 1);
}

static int dyn_printf(struct_dyn *b, const char *fmt, ...) {
  va_list ap;
  int need;
  size_t n;

  va_start(ap, fmt);
  need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) return -1;

  n = (size_t)need;
  if (dyn_reserve(b, n)) return -1;

  va_start(ap, fmt);
  vsnprintf((char *)b->p + b->len, n + 1, fmt, ap);
  va_end(ap);
  b->len += n;
  return 0;
}

static int dyn_hex(struct_dyn *b, const unsigned char *data, size_t len) {
  static const char hexdigits[] = "0123456789abcdef";
  size_t i;

  if (len == 0) return dyn_putc(b, '-');
  if (dyn_reserve(b, len * 2)) return -1;
  for (i = 0; i < len; ++i) {
    b->p[b->len++] = (unsigned char)hexdigits[data[i] >> 4];
    b->p[b->len++] = (unsigned char)hexdigits[data[i] & 15];
  }
  return 0;
}

static void dyn_free(struct_dyn *b) {
  free(b->p);
  b->p = NULL;
  b->len = b->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Hashing                                                            */
/* ------------------------------------------------------------------ */

static uint64_t fnv1a(const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  uint64_t h = 1469598103934665603ULL;
  size_t i;

  for (i = 0; i < len; ++i) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

/* ------------------------------------------------------------------ */
/* Very strict table parsing                                          */
/* ------------------------------------------------------------------ */

static int struct_is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\r';
}

static void struct_skip_ws(const char **p, const char *end) {
  while (*p < end && struct_is_ws(**p)) ++*p;
}

static int struct_parse_dec(const char **p, const char *end, uint64_t *out) {
  const char *q = *p;
  uint64_t v = 0;

  if (q >= end || *q < '0' || *q > '9') return -1;

  while (q < end && *q >= '0' && *q <= '9') {
    unsigned d = (unsigned)(*q - '0');
    if (v > (UINT64_MAX - d) / 10) return -1;
    v = v * 10 + d;
    ++q;
  }

  *p = q;
  *out = v;
  return 0;
}

static int struct_parse_needs(const char *p, const char *end, uint64_t **out,
                              size_t *nout) {
  uint64_t *list = NULL;
  size_t len = 0, cap = 0;
  const char *e = end;

  while (e > p && struct_is_ws(e[-1])) --e;
  struct_skip_ws(&p, e);

  if (p == e) { free(list); return -1; }

  if (e - p == 1 && *p == '-') {
    *out = NULL;
    *nout = 0;
    return 0;
  }

  for (;;) {
    uint64_t id;
    struct_skip_ws(&p, e);
    if (struct_parse_dec(&p, e, &id)) { free(list); return -1; }
    if (len == cap) {
      size_t ncap = cap ? cap * 2 : 4;
      uint64_t *nl = (uint64_t *)realloc(list, ncap * sizeof(*nl));
      if (!nl) { free(list); return -1; }
      list = nl;
      cap = ncap;
    }
    list[len++] = id;
    struct_skip_ws(&p, e);
    if (p == e) break;
    if (*p != ',') { free(list); return -1; }
    ++p;
    {
      const char *t = p;
      while (t < e && struct_is_ws(*t)) ++t;
      if (t == e) { free(list); return -1; } /* trailing comma */
    }
  }

  *out = list;
  *nout = len;
  return 0;
}

/* Returns 0 for a fragment line, 1 for a blank line, -1 for a syntax error. */

static int struct_parse_line(const char *begin, const char *end,
                             struct_frag *out) {
  const char *p = begin;
  uint64_t id, start, stop;

  memset(out, 0, sizeof(*out));
  struct_skip_ws(&p, end);
  if (p == end) return 1;

  if (struct_parse_dec(&p, end, &id)) return -1;
  if (p == end || !struct_is_ws(*p)) return -1;
  struct_skip_ws(&p, end);

  if (struct_parse_dec(&p, end, &start)) return -1;
  if (p == end || !struct_is_ws(*p)) return -1;
  struct_skip_ws(&p, end);

  if (struct_parse_dec(&p, end, &stop)) return -1;
  struct_skip_ws(&p, end);

  if (p == end || *p != ':') return -1;
  ++p;
  struct_skip_ws(&p, end);

  if (struct_parse_needs(p, end, &out->needs, &out->nneeds)) return -1;

  out->id = id;
  out->start = (size_t)start;
  out->end = (size_t)stop;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Sorting helpers                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
  uint64_t id;
  size_t idx;
} struct_id_order;

static int struct_cmp_id_order(const void *va, const void *vb) {
  const struct_id_order *a = (const struct_id_order *)va;
  const struct_id_order *b = (const struct_id_order *)vb;
  if (a->id < b->id) return -1;
  if (a->id > b->id) return 1;
  return 0;
}

static int struct_cmp_u64(const void *va, const void *vb) {
  uint64_t a = *(const uint64_t *)va;
  uint64_t b = *(const uint64_t *)vb;
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}

/* Positional order: start, then end, then ID (insertion sort on indices). */

static void struct_position_sort(const struct_frag *frags, size_t *idx,
                                 size_t n) {
  size_t i;

  for (i = 1; i < n; ++i) {
    size_t cur = idx[i];
    size_t j = i;
    while (j > 0) {
      const struct_frag *a = &frags[idx[j - 1]];
      const struct_frag *b = &frags[cur];
      int after = 0;
      if (a->start != b->start) after = a->start > b->start;
      else if (a->end != b->end) after = a->end > b->end;
      else after = a->id > b->id;
      if (!after) break;
      idx[j] = idx[j - 1];
      --j;
    }
    idx[j] = cur;
  }
}

/* ------------------------------------------------------------------ */
/* Canonical table identity                                           */
/* ------------------------------------------------------------------ */

static int struct_build_canon(struct afl_struct_session *s) {
  struct_dyn b;
  size_t i;
  int rc = -1;

  memset(&b, 0, sizeof(b));

  for (i = 0; i < s->nfrags; ++i) {
    size_t fi = s->by_id[i];
    struct_frag *f = &s->frags[fi];
    uint64_t *sorted = NULL;
    size_t n = 0, j;

    if (f->nneeds) {
      sorted = (uint64_t *)malloc(f->nneeds * sizeof(*sorted));
      if (!sorted) goto done;
      memcpy(sorted, f->needs, f->nneeds * sizeof(*sorted));
      qsort(sorted, f->nneeds, sizeof(*sorted), struct_cmp_u64);
      for (j = 0; j < f->nneeds; ++j)
        if (!n || sorted[j] != sorted[n - 1]) sorted[n++] = sorted[j];
    }

    if (dyn_printf(&b, "%llu %zu %zu : ",
                   (unsigned long long)f->id, f->start, f->end)) {
      free(sorted);
      goto done;
    }

    if (!n) {
      if (dyn_putc(&b, '-')) { free(sorted); goto done; }
    } else {
      for (j = 0; j < n; ++j) {
        if (j && dyn_putc(&b, ',')) { free(sorted); goto done; }
        if (dyn_printf(&b, "%llu", (unsigned long long)sorted[j])) {
          free(sorted);
          goto done;
        }
      }
    }

    free(sorted);
    if (dyn_putc(&b, '\n')) goto done;
  }

  if (dyn_reserve(&b, 0)) goto done;
  s->canon = (char *)b.p;
  s->canon_len = b.len;
  b.p = NULL;
  rc = 0;

done:
  dyn_free(&b);
  return rc;
}

/* ------------------------------------------------------------------ */
/* Cache                                                             */
/* ------------------------------------------------------------------ */

static void struct_cache_clear(struct afl_struct_session *s) {
  size_t i;

  for (i = 0; i < s->ncache; ++i) free(s->cache[i].bytes);
  free(s->cache);
  free(s->slots);
  s->cache = NULL;
  s->slots = NULL;
  s->ncache = s->cache_cap = s->nslots = 0;
}

static int struct_cache_rehash(struct afl_struct_session *s, size_t want) {
  size_t nslots = 16, i;
  size_t *slots;

  while (nslots < want) nslots *= 2;
  slots = (size_t *)calloc(nslots, sizeof(*slots));
  if (!slots) return -1;

  for (i = 0; i < s->ncache; ++i) {
    size_t pos = (size_t)(s->cache[i].hash & (nslots - 1));
    while (slots[pos]) pos = (pos + 1) & (nslots - 1);
    slots[pos] = i + 1;
  }

  free(s->slots);
  s->slots = slots;
  s->nslots = nslots;
  return 0;
}

static int struct_cache_lookup(const struct afl_struct_session *s,
                               const unsigned char *bytes, size_t len,
                               int *result) {
  uint64_t h;
  size_t pos;

  if (!s->nslots) return 0;

  h = fnv1a(bytes, len);
  pos = (size_t)(h & (s->nslots - 1));

  for (;;) {
    size_t slot = s->slots[pos];
    struct_cache_entry *e;
    if (!slot) return 0;
    e = &s->cache[slot - 1];
    if (e->hash == h && e->len == len &&
        (len == 0 || memcmp(e->bytes, bytes, len) == 0)) {
      *result = e->result;
      return 1;
    }
    pos = (pos + 1) & (s->nslots - 1);
  }
}

static int struct_cache_insert(struct afl_struct_session *s,
                               const unsigned char *bytes, size_t len,
                               int result) {
  uint64_t h;
  size_t pos;
  struct_cache_entry *e;

  if (s->ncache && s->ncache * 4 >= s->nslots * 3) {
    if (struct_cache_rehash(s, s->nslots ? s->nslots * 2 : 16)) return -1;
  } else if (!s->nslots) {
    if (struct_cache_rehash(s, 16)) return -1;
  }

  h = fnv1a(bytes, len);
  pos = (size_t)(h & (s->nslots - 1));

  for (;;) {
    size_t slot = s->slots[pos];
    if (!slot) break;
    e = &s->cache[slot - 1];
    if (e->hash == h && e->len == len &&
        (len == 0 || memcmp(e->bytes, bytes, len) == 0))
      return 0; /* already present; keep the first outcome */
    pos = (pos + 1) & (s->nslots - 1);
  }

  if (s->ncache == s->cache_cap) {
    size_t ncap = s->cache_cap ? s->cache_cap * 2 : 16;
    struct_cache_entry *ne =
        (struct_cache_entry *)realloc(s->cache, ncap * sizeof(*ne));
    if (!ne) return -1;
    s->cache = ne;
    s->cache_cap = ncap;
  }

  e = &s->cache[s->ncache];
  memset(e, 0, sizeof(*e));
  if (len) {
    e->bytes = (unsigned char *)malloc(len);
    if (!e->bytes) return -1;
    memcpy(e->bytes, bytes, len);
  }
  e->len = len;
  e->hash = h;
  e->result = result;

  s->slots[pos] = s->ncache + 1;
  s->ncache++;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Fragment lookup and candidate construction                         */
/* ------------------------------------------------------------------ */

static int struct_find_id(const struct afl_struct_session *s, uint64_t id) {
  size_t lo = 0, hi = s->nfrags;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    uint64_t cur = s->frags[s->by_id[mid]].id;
    if (cur == id) return (int)s->by_id[mid];
    if (cur < id) lo = mid + 1;
    else hi = mid;
  }
  return -1;
}

/* Build the accepted bytes, or a candidate when `exclude` is non-NULL. */

static int struct_build_concat(const struct afl_struct_session *s,
                               const unsigned char *exclude,
                               unsigned char **out, size_t *out_len) {
  size_t total = 0, i, at = 0;
  unsigned char *buf;

  for (i = 0; i < s->nfrags; ++i) {
    size_t fi = s->by_start[i];
    if (!s->active[fi]) continue;
    if (exclude && exclude[fi]) continue;
    total += s->frags[fi].end - s->frags[fi].start;
  }

  buf = (unsigned char *)malloc(total ? total : 1);
  if (!buf) return -1;

  for (i = 0; i < s->nfrags; ++i) {
    size_t fi = s->by_start[i];
    size_t n;
    if (!s->active[fi]) continue;
    if (exclude && exclude[fi]) continue;
    n = s->frags[fi].end - s->frags[fi].start;
    if (n) {
      memcpy(buf + at, s->orig + s->frags[fi].start, n);
      at += n;
    }
  }

  *out = buf;
  *out_len = total;
  return 0;
}

/*
  Mark the active seed plus every active fragment that depends on it.  The
  reachability walk follows the full dependency graph (including links through
  fragments that are no longer active), because a still-active fragment can
  depend on the seed through a chain that passes an already deleted node.
  Cycles converge because the set only grows.
*/

static int struct_closure(const struct afl_struct_session *s, size_t seed,
                          unsigned char *mark) {
  unsigned char *reach = NULL;
  int changed = 1;
  size_t i;

  if (s->nfrags) {
    reach = (unsigned char *)calloc(s->nfrags, 1);
    if (!reach) return -1;
  }

  memset(mark, 0, s->nfrags);
  if (s->nfrags) reach[seed] = 1;

  while (changed) {
    changed = 0;
    for (i = 0; i < s->nfrags; ++i) {
      size_t j;
      if (reach[i]) continue;
      for (j = 0; j < s->frags[i].nneeds; ++j) {
        int dep = struct_find_id(s, s->frags[i].needs[j]);
        if (dep >= 0 && reach[dep]) {
          reach[i] = 1;
          changed = 1;
          break;
        }
      }
    }
  }

  for (i = 0; i < s->nfrags; ++i)
    if (reach[i] && s->active[i]) mark[i] = 1;

  free(reach);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Public session API                                                 */
/* ------------------------------------------------------------------ */

afl_struct_session *afl_struct_create(const unsigned char *data, size_t len,
                                      const char *table) {
  afl_struct_session *s = NULL;
  struct_id_order *id_order = NULL;
  size_t i;
  const char *p;
  size_t tlen;

  if (!table) return NULL;
  if (len && !data) return NULL;

  tlen = strlen(table);
  if (tlen > (size_t)64 * 1024 * 1024) return NULL;

  s = (afl_struct_session *)calloc(1, sizeof(*s));
  if (!s) return NULL;

  if (len) {
    s->orig = (unsigned char *)malloc(len);
    if (!s->orig) goto fail;
    memcpy(s->orig, data, len);
  }
  s->orig_len = len;

  s->table = (char *)malloc(tlen + 1);
  if (!s->table) goto fail;
  memcpy(s->table, table, tlen + 1);

  /* First pass: parse and count fragments. */

  p = table;
  while (p < table + tlen) {
    const char *nl = (const char *)memchr(p, '\n', (size_t)(table + tlen - p));
    const char *end = nl ? nl : table + tlen;
    struct_frag f;
    int r = struct_parse_line(p, end, &f);

    if (r < 0) goto fail;

    if (r == 0) {
      struct_frag *nf =
          (struct_frag *)realloc(s->frags, (s->nfrags + 1) * sizeof(*nf));
      if (!nf) { free(f.needs); goto fail; }
      s->frags = nf;
      s->frags[s->nfrags++] = f;
    }

    p = nl ? nl + 1 : table + tlen;
  }

  /* Basic interval sanity, ID uniqueness, positional order. */

  if (s->nfrags) {
    id_order =
        (struct_id_order *)malloc(s->nfrags * sizeof(struct_id_order));
    if (!id_order) goto fail;
  }

  for (i = 0; i < s->nfrags; ++i) {
    if (s->frags[i].start > s->frags[i].end ||
        s->frags[i].end > len)
      goto fail;
    id_order[i].id = s->frags[i].id;
    id_order[i].idx = i;
  }

  if (s->nfrags)
    qsort(id_order, s->nfrags, sizeof(*id_order), struct_cmp_id_order);

  s->by_id = (size_t *)malloc((s->nfrags ? s->nfrags : 1) * sizeof(size_t));
  if (!s->by_id) goto fail;
  for (i = 0; i < s->nfrags; ++i) {
    if (i && id_order[i].id == id_order[i - 1].id) goto fail;
    s->by_id[i] = id_order[i].idx;
  }

  /* Positional order: candidate bytes follow the original layout. */

  {
    size_t *starts = NULL;
    if (s->nfrags) {
      starts = (size_t *)malloc(s->nfrags * sizeof(*starts));
      if (!starts) goto fail;
      for (i = 0; i < s->nfrags; ++i) starts[i] = i;
      struct_position_sort(s->frags, starts, s->nfrags);
    }

    s->by_start = (size_t *)malloc((s->nfrags ? s->nfrags : 1) * sizeof(size_t));
    if (!s->by_start) { free(starts); goto fail; }
    for (i = 0; i < s->nfrags; ++i) s->by_start[i] = starts[i];
    free(starts);
  }

  /* Every non-empty interval must form an exact partition of the input. */

  {
    size_t covered = 0;
    int any = 0;
    for (i = 0; i < s->nfrags; ++i) {
      struct_frag *f = &s->frags[s->by_start[i]];
      if (f->end == f->start) continue;
      if (!any) {
        if (f->start != 0) goto fail;
      } else if (f->start != covered) {
        goto fail;
      }
      covered = f->end;
      any = 1;
    }
    if (len == 0) {
      if (any) goto fail; /* cannot happen: end <= len == 0 */
    } else if (!any || covered != len) {
      goto fail;
    }
  }

  /* Dependency references must exist (cycles are fine). */

  for (i = 0; i < s->nfrags; ++i) {
    size_t j;
    for (j = 0; j < s->frags[i].nneeds; ++j)
      if (struct_find_id(s, s->frags[i].needs[j]) < 0) goto fail;
  }

  s->active = (unsigned char *)malloc(s->nfrags ? s->nfrags : 1);
  if (!s->active) goto fail;
  memset(s->active, 1, s->nfrags);
  s->active_count = s->nfrags;
  s->cursor = 0;

  if (struct_build_canon(s)) goto fail;

  if (struct_build_concat(s, NULL, &s->cur, &s->cur_len)) goto fail;

  /* The initial accepted bytes are accepted by definition. */
  if (struct_cache_insert(s, s->cur, s->cur_len, 1)) goto fail;

  free(id_order);
  return s;

fail:
  free(id_order);
  afl_struct_destroy(s);
  return NULL;
}

const unsigned char *afl_struct_data(const afl_struct_session *s, size_t *len) {
  if (!s) {
    if (len) *len = 0;
    return NULL;
  }
  if (len) *len = s->cur_len;
  return s->cur;
}

void afl_struct_destroy(afl_struct_session *s) {
  size_t i;

  if (!s) return;

  for (i = 0; i < s->nfrags; ++i) free(s->frags[i].needs);
  free(s->frags);
  free(s->by_id);
  free(s->by_start);
  free(s->active);
  free(s->orig);
  free(s->table);
  free(s->canon);
  free(s->cur);
  struct_cache_clear(s);
  free(s);
}

int afl_struct_step(afl_struct_session *s, size_t budget,
                    int (*observe)(const unsigned char *, size_t, void *),
                    void *context) {
  unsigned char *mark = NULL;

  if (!s || !observe) return -1;

  if (s->nfrags && !(mark = (unsigned char *)malloc(s->nfrags))) return -1;

  for (;;) {
    size_t fi;
    unsigned char *cand = NULL;
    size_t cand_len = 0;
    int cached_result;
    int r;

    /* Locate the next active fragment at or after the cursor. */

    while (s->cursor < s->nfrags && !s->active[s->by_id[s->cursor]])
      ++s->cursor;

    if (s->cursor >= s->nfrags) {
      free(mark);
      return 0;
    }

    fi = s->by_id[s->cursor];

    if (struct_closure(s, fi, mark)) {
      free(mark);
      return -1;
    }

    if (struct_build_concat(s, mark, &cand, &cand_len)) {
      free(mark);
      return -1;
    }

    if (struct_cache_lookup(s, cand, cand_len, &cached_result)) {
      free(cand);
      if (cached_result == 1) {
        size_t i;
        for (i = 0; i < s->nfrags; ++i)
          if (mark[i] && s->active[i]) {
            s->active[i] = 0;
            --s->active_count;
          }
        free(s->cur);
        s->cur = NULL;
        if (struct_build_concat(s, NULL, &s->cur, &s->cur_len)) {
          free(mark);
          return -1;
        }
        s->cursor = 0;
      } else {
        ++s->cursor;
      }
      continue;
    }

    if (budget == 0) {
      free(cand);
      free(mark);
      return 1; /* real candidate remains, no budget */
    }

    r = observe(cand, cand_len, context);

    if (r != 0 && r != 1) {
      /* Errors neither advance the cursor nor populate the cache. */
      free(cand);
      free(mark);
      return -1;
    }

    if (struct_cache_insert(s, cand, cand_len, r)) {
      free(cand);
      free(mark);
      return -1;
    }

    free(cand);

    if (r == 1) {
      size_t i;
      for (i = 0; i < s->nfrags; ++i)
        if (mark[i] && s->active[i]) {
          s->active[i] = 0;
          --s->active_count;
        }
      free(s->cur);
      s->cur = NULL;
      if (struct_build_concat(s, NULL, &s->cur, &s->cur_len)) {
        free(mark);
        return -1;
      }
      s->cursor = 0;
    } else {
      ++s->cursor;
    }

    if (budget != (size_t)-1) --budget;
  }
}

/* ------------------------------------------------------------------ */
/* Checkpoint state                                                   */
/* ------------------------------------------------------------------ */

static int struct_write_atomic(const char *path, const unsigned char *data,
                               size_t len) {
  size_t plen = strlen(path);
  char *tmp;
  int fd;
  size_t off = 0;
  int rc = -1;

  if (plen > (size_t)-1 - 16) return -1;
  tmp = (char *)malloc(plen + 16);
  if (!tmp) return -1;
  memcpy(tmp, path, plen);
  memcpy(tmp + plen, ".tmpXXXXXX", 11);

  fd = mkstemp(tmp);
  if (fd < 0) { free(tmp); return -1; }

  while (off < len) {
    ssize_t n = write(fd, data + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      goto done;
    }
    if (n == 0) goto done;
    off += (size_t)n;
  }

  if (fsync(fd)) goto done;
  if (close(fd)) { fd = -1; goto done; }
  fd = -1;

  if (rename(tmp, path)) goto done;
  rc = 0;

done:
  if (fd >= 0) close(fd);
  if (rc) unlink(tmp);
  free(tmp);
  return rc;
}

int afl_struct_state_save(const afl_struct_session *s, const char *path,
                          const unsigned char *meta, size_t meta_len) {
  struct_dyn b;
  uint64_t checksum;
  size_t i;
  int rc = -1;

  if (!s || !path || (meta_len && !meta)) return -1;

  memset(&b, 0, sizeof(b));

  if (dyn_printf(&b, "%s\n", STRUCT_STATE_MAGIC)) goto done;

  if (dyn_printf(&b, "data %zu ", s->orig_len) ||
      dyn_hex(&b, s->orig, s->orig_len) ||
      dyn_putc(&b, '\n'))
    goto done;

  if (dyn_printf(&b, "table %zu ", s->canon_len) ||
      dyn_hex(&b, (const unsigned char *)s->canon, s->canon_len) ||
      dyn_putc(&b, '\n'))
    goto done;

  if (dyn_printf(&b, "active %zu", s->active_count)) goto done;
  for (i = 0; i < s->nfrags; ++i) {
    if (!s->active[i]) continue;
    if (dyn_printf(&b, " %llu", (unsigned long long)s->frags[i].id)) goto done;
  }
  if (dyn_putc(&b, '\n')) goto done;

  if (dyn_printf(&b, "cursor %zu\n", s->cursor)) goto done;
  if (dyn_printf(&b, "cache %zu\n", s->ncache)) goto done;

  for (i = 0; i < s->ncache; ++i) {
    if (dyn_printf(&b, "entry %zu ", s->cache[i].len) ||
        dyn_hex(&b, s->cache[i].bytes, s->cache[i].len) ||
        dyn_printf(&b, " %d\n", s->cache[i].result))
      goto done;
  }

  if (dyn_printf(&b, "meta %zu ", meta_len) ||
      dyn_hex(&b, meta, meta_len) ||
      dyn_putc(&b, '\n'))
    goto done;

  checksum = fnv1a(b.p, b.len);
  if (dyn_printf(&b, "checksum %016llx\n", (unsigned long long)checksum))
    goto done;

  rc = struct_write_atomic(path, b.p, b.len);

done:
  dyn_free(&b);
  return rc;
}

/* Bounded line reader over the checksum-free prefix. */

typedef struct {
  char *p;
  char *end;
} struct_lines;

static int struct_next_line(struct_lines *ls, char **line) {
  char *nl, *le;

  if (ls->p >= ls->end) return 0;
  nl = (char *)memchr(ls->p, '\n', (size_t)(ls->end - ls->p));
  le = nl ? nl : ls->end;
  if (nl) *nl = 0;
  if (le > ls->p && le[-1] == '\r') le[-1] = 0;
  *line = ls->p;
  ls->p = nl ? nl + 1 : ls->end;
  return 1;
}

static int struct_hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Parse "len <hex|->" from a token pair. */

static int struct_decode_hex(const char *len_tok, const char *hex_tok,
                             unsigned char **out, size_t *out_len) {
  uint64_t len64;
  size_t len, i;
  unsigned char *buf;

  if (struct_parse_dec(&len_tok, len_tok + strlen(len_tok), &len64))
    return -1;
  if (len64 > (uint64_t)(size_t)-1) return -1;
  len = (size_t)len64;

  if (len == 0) {
    if (!hex_tok || strcmp(hex_tok, "-")) return -1;
    *out = NULL;
    *out_len = 0;
    return 0;
  }

  if (!hex_tok || strlen(hex_tok) != len * 2) return -1;
  buf = (unsigned char *)malloc(len);
  if (!buf) return -1;

  for (i = 0; i < len; ++i) {
    int hi = struct_hexval((unsigned char)hex_tok[i * 2]);
    int lo = struct_hexval((unsigned char)hex_tok[i * 2 + 1]);
    if (hi < 0 || lo < 0) { free(buf); return -1; }
    buf[i] = (unsigned char)((hi << 4) | lo);
  }

  *out = buf;
  *out_len = len;
  return 0;
}

/* Split a line on spaces into at most `max` tokens. */

static int struct_tokens(char *line, char **tok, int max) {
  int n = 0;
  char *p = line;

  for (;;) {
    while (*p == ' ' || *p == '\t' || *p == '\r') ++p;
    if (!*p) break;
    if (n == max) return -1;
    tok[n++] = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r') ++p;
    if (*p) *p++ = 0;
  }
  return n;
}

static int struct_parse_u64_tok(const char *tok, uint64_t *v) {
  const char *p = tok;
  size_t n = strlen(tok);
  return struct_parse_dec(&p, tok + n, v) || p != tok + n ? -1 : 0;
}

int afl_struct_state_load(const char *path, const unsigned char *data,
                          size_t len, const char *table,
                          unsigned char *meta_out, size_t meta_cap,
                          size_t *meta_len_out,
                          afl_struct_session **session_out) {
  int fd;
  struct stat st;
  unsigned char *buf = NULL;
  size_t size = 0, off = 0;
  uint64_t stored_ck;
  size_t ck_off = 0;
  int found_ck = 0;
  struct_lines ls;
  char *line;
  afl_struct_session *s = NULL;
  unsigned char *stored_data = NULL, *stored_table = NULL, *stored_meta = NULL;
  size_t stored_data_len = 0, stored_table_len = 0, stored_meta_len = 0;
  int rc = AFL_STRUCT_STATE_ERROR;
  int first = 1;

  if (session_out) *session_out = NULL;
  if (meta_len_out) *meta_len_out = 0;
  if (!path || !table) return AFL_STRUCT_STATE_ERROR;

  fd = open(path, O_RDONLY);
  if (fd < 0) return errno == ENOENT ? AFL_STRUCT_STATE_MISSING
                                     : AFL_STRUCT_STATE_ERROR;

  if (fstat(fd, &st) || st.st_size <= 0) { close(fd); return AFL_STRUCT_STATE_ERROR; }
  if ((uintmax_t)st.st_size > (uintmax_t)((size_t)-1)) { close(fd); return AFL_STRUCT_STATE_ERROR; }

  size = (size_t)st.st_size;
  buf = (unsigned char *)malloc(size + 1);
  if (!buf) { close(fd); return AFL_STRUCT_STATE_ERROR; }

  while (off < size) {
    ssize_t n = read(fd, buf + off, size - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      free(buf); close(fd); return AFL_STRUCT_STATE_ERROR;
    }
    if (n == 0) break;
    off += (size_t)n;
  }
  close(fd);
  if (off != size) { free(buf); return AFL_STRUCT_STATE_ERROR; }
  buf[size] = 0;

  /* Locate the trailing checksum line and verify integrity over the rest. */

  {
    size_t pos = 0;
    while (pos < size) {
      size_t nl = pos;
      while (nl < size && buf[nl] != '\n') ++nl;
      if (nl - pos >= 9 && !memcmp(buf + pos, "checksum ", 9)) {
        ck_off = pos;
        found_ck = 1;
        {
          size_t end = nl;
          uint64_t v = 0;
          size_t i;
          if (end - (pos + 9) != 16) { found_ck = 0; break; }
          for (i = 0; i < 16; ++i) {
            int hv = struct_hexval(buf[pos + 9 + i]);
            if (hv < 0) { found_ck = 0; break; }
            v = (v << 4) | (uint64_t)hv;
          }
          if (!found_ck) break;
          stored_ck = v;
          if (nl == size) {
            /* fine: checksum line is last, no trailing newline */
          } else if (nl + 1 != size) {
            found_ck = 0; /* trailing junk after checksum */
          }
        }
        break;
      }
      pos = nl < size ? nl + 1 : size;
    }
  }

  if (!found_ck) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
  if (fnv1a(buf, ck_off) != stored_ck) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
  if (ck_off > 0 && buf[ck_off - 1] != '\n') {
    rc = AFL_STRUCT_STATE_MISMATCH;
    goto done;
  }

  /* Parse the header + records. */

  buf[ck_off ? ck_off - 1 : 0] = 0; /* cut at the newline before checksum */
  ls.p = (char *)buf;
  ls.end = (char *)buf + (ck_off ? ck_off - 1 : 0);

  if (!struct_next_line(&ls, &line)) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
  if (strcmp(line, STRUCT_STATE_MAGIC)) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }

  /* data */
  if (!struct_next_line(&ls, &line)) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
  {
    char *tok[3];
    if (struct_tokens(line, tok, 3) != 3) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
    if (strcmp(tok[0], "data")) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
    if (struct_decode_hex(tok[1], tok[2], &stored_data, &stored_data_len)) {
      rc = AFL_STRUCT_STATE_MISMATCH; goto done;
    }
  }
  if (stored_data_len != len ||
      (len && memcmp(stored_data, data, len))) {
    rc = AFL_STRUCT_STATE_MISMATCH; goto done;
  }

  /* table identity */
  if (!struct_next_line(&ls, &line)) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
  {
    char *tok[3];
    if (struct_tokens(line, tok, 3) != 3) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
    if (strcmp(tok[0], "table")) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
    if (struct_decode_hex(tok[1], tok[2], &stored_table, &stored_table_len)) {
      rc = AFL_STRUCT_STATE_MISMATCH; goto done;
    }
  }

  s = afl_struct_create(data, len, table);
  if (!s) { rc = AFL_STRUCT_STATE_ERROR; goto done; }

  if (stored_table_len != s->canon_len ||
      (stored_table_len &&
       memcmp(stored_table, s->canon, stored_table_len))) {
    rc = AFL_STRUCT_STATE_MISMATCH; goto done;
  }

  /* Fresh accept set, cursor, and cache come from the state file. */
  memset(s->active, 0, s->nfrags);
  s->active_count = 0;
  s->cursor = 0;
  struct_cache_clear(s);

  /* active */
  if (!struct_next_line(&ls, &line)) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
  if (strncmp(line, "active ", 7)) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
  {
    const char *p = line + 7;
    const char *end = line + strlen(line);
    uint64_t count, i;
    if (struct_parse_dec(&p, end, &count)) {
      rc = AFL_STRUCT_STATE_MISMATCH; goto done;
    }
    if (count > s->nfrags) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
    for (i = 0; i < count; ++i) {
      uint64_t id;
      int fi;
      struct_skip_ws(&p, end);
      if (struct_parse_dec(&p, end, &id)) {
        rc = AFL_STRUCT_STATE_MISMATCH; goto done;
      }
      fi = struct_find_id(s, id);
      if (fi < 0 || s->active[fi]) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
      s->active[fi] = 1;
      ++s->active_count;
    }
    struct_skip_ws(&p, end);
    if (p != end) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
  }

  /* cursor */
  if (!struct_next_line(&ls, &line)) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
  {
    char *tok[2];
    uint64_t c;
    if (struct_tokens(line, tok, 2) != 2 || strcmp(tok[0], "cursor")) {
      rc = AFL_STRUCT_STATE_MISMATCH; goto done;
    }
    if (struct_parse_u64_tok(tok[1], &c) || c > s->nfrags) {
      rc = AFL_STRUCT_STATE_MISMATCH; goto done;
    }
    s->cursor = (size_t)c;
  }

  /* cache */
  if (!struct_next_line(&ls, &line)) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
  {
    char *tok[2];
    uint64_t count, i;
    if (struct_tokens(line, tok, 2) != 2 || strcmp(tok[0], "cache")) {
      rc = AFL_STRUCT_STATE_MISMATCH; goto done;
    }
    if (struct_parse_u64_tok(tok[1], &count)) {
      rc = AFL_STRUCT_STATE_MISMATCH; goto done;
    }
    if (count > size / 8 + 1) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
    for (i = 0; i < count; ++i) {
      char *et[4];
      unsigned char *bytes = NULL;
      size_t blen = 0;
      uint64_t res;
      if (!struct_next_line(&ls, &line)) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
      if (struct_tokens(line, et, 4) != 4 || strcmp(et[0], "entry")) {
        rc = AFL_STRUCT_STATE_MISMATCH; goto done;
      }
      if (struct_decode_hex(et[1], et[2], &bytes, &blen) || blen > len ||
          struct_parse_u64_tok(et[3], &res) || res > 1) {
        free(bytes);
        rc = AFL_STRUCT_STATE_MISMATCH; goto done;
      }
      if (struct_cache_insert(s, bytes, blen, (int)res)) {
        free(bytes);
        rc = AFL_STRUCT_STATE_ERROR; goto done;
      }
      free(bytes);
    }
  }

  /* meta */
  if (!struct_next_line(&ls, &line)) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }
  {
    char *tok[3];
    if (struct_tokens(line, tok, 3) != 3 || strcmp(tok[0], "meta")) {
      rc = AFL_STRUCT_STATE_MISMATCH; goto done;
    }
    if (struct_decode_hex(tok[1], tok[2], &stored_meta, &stored_meta_len)) {
      rc = AFL_STRUCT_STATE_MISMATCH; goto done;
    }
  }

  if (struct_next_line(&ls, &line)) { rc = AFL_STRUCT_STATE_MISMATCH; goto done; }

  if (stored_meta_len > meta_cap) { rc = AFL_STRUCT_STATE_ERROR; goto done; }
  if (meta_out && stored_meta_len)
    memcpy(meta_out, stored_meta, stored_meta_len);
  if (meta_len_out) *meta_len_out = stored_meta_len;

  /* Recompute the accepted bytes; they must be a recorded accept. */

  free(s->cur);
  s->cur = NULL;
  s->cur_len = 0;
  if (struct_build_concat(s, NULL, &s->cur, &s->cur_len)) {
    rc = AFL_STRUCT_STATE_ERROR; goto done;
  }
  {
    int cached;
    if (!struct_cache_lookup(s, s->cur, s->cur_len, &cached) || cached != 1) {
      rc = AFL_STRUCT_STATE_MISMATCH; goto done;
    }
  }

  if (session_out) *session_out = s;
  else afl_struct_destroy(s);
  s = NULL;
  rc = AFL_STRUCT_STATE_OK;

done:
  (void)first;
  if (rc != AFL_STRUCT_STATE_OK && s) afl_struct_destroy(s);
  free(stored_data);
  free(stored_table);
  free(stored_meta);
  free(buf);
  return rc;
}
