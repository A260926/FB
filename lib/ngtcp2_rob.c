/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "ngtcp2_rob.h"

#include <string.h>
#include <assert.h>

#include "ngtcp2_macro.h"
#include "ngtcp2_err.h"

static int rob_data_new(uint8_t **pd, size_t chunk, const ngtcp2_mem *mem) {
  *pd = ngtcp2_mem_malloc(mem, chunk);
  if (*pd == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  return 0;
}

static void rob_data_del(uint8_t *d, const ngtcp2_mem *mem) {
  ngtcp2_mem_free(mem, d);
}

/*
 * rob_jentry_kind identifies a single mutation made by an atomic push
 * which has to be undone if the operation fails.
 */
typedef enum rob_jentry_kind {
  /* ROB_JOURNAL_GAP_REMOVE records that a whole gap range was
     removed from gapksl. */
  ROB_JOURNAL_GAP_REMOVE = 1,
  /* ROB_JOURNAL_GAP_UPDATE records that a gap key was replaced. */
  ROB_JOURNAL_GAP_UPDATE,
  /* ROB_JOURNAL_GAP_INSERT records that a new right gap range was
     inserted into gapksl. */
  ROB_JOURNAL_GAP_INSERT,
  /* ROB_JOURNAL_CHUNK records a data chunk allocated by this
     operation and inserted into dataksl. */
  ROB_JOURNAL_CHUNK,
  /* ROB_JOURNAL_BYTES records bytes overwritten in a chunk which
     existed before this operation. */
  ROB_JOURNAL_BYTES
} rob_jentry_kind;

typedef struct rob_jentry {
  rob_jentry_kind kind;
  /* range is the removed/old gap range, the dataksl chunk key or the
     destination range of a byte overwrite. */
  ngtcp2_range range;
  /* new_range is the replacement gap key for GAP_UPDATE. */
  ngtcp2_range new_range;
  /* ptr is the chunk buffer of CHUNK, or the destination of BYTES. */
  uint8_t *ptr;
  /* saved stores the previous bytes of a BYTES entry. */
  uint8_t *saved;
} rob_jentry;

static int rob_journal_push(rob_jentry **pjournal, size_t *pcap, size_t *pn,
                            rob_jentry entry, const ngtcp2_mem *mem) {
  rob_jentry *journal;

  if (*pn == *pcap) {
    size_t ncap = *pcap ? *pcap * 2 : 8;

    journal = ngtcp2_mem_realloc(mem, *pjournal, ncap * sizeof(*journal));
    if (journal == NULL) {
      return NGTCP2_ERR_NOMEM;
    }

    *pjournal = journal;
    *pcap = ncap;
  }

  (*pjournal)[(*pn)++] = entry;

  return 0;
}

/*
 * rob_journal_rollback undoes every recorded mutation in the reverse
 * order.  The inverse tree operations only touch slots freed or kept
 * by this operation, so they do not require new block allocations.
 */
static void rob_journal_rollback(ngtcp2_rob *rob, rob_jentry *journal,
                                 size_t jn) {
  for (; jn--;) {
    rob_jentry *entry = &journal[jn];

    switch (entry->kind) {
    case ROB_JOURNAL_GAP_REMOVE:
      ngtcp2_ksl_insert(&rob->gapksl, NULL, &entry->range, NULL);
      break;
    case ROB_JOURNAL_GAP_UPDATE:
      ngtcp2_ksl_update_key(&rob->gapksl, &entry->new_range, &entry->range);
      break;
    case ROB_JOURNAL_GAP_INSERT:
      ngtcp2_ksl_remove(&rob->gapksl, NULL, &entry->range);
      break;
    case ROB_JOURNAL_CHUNK: {
      int rv = ngtcp2_ksl_remove(&rob->dataksl, NULL, &entry->range);
      (void)rv;
      assert(rv == 0);
      rob_data_del(entry->ptr, rob->mem);
      break;
    }
    case ROB_JOURNAL_BYTES:
      memcpy(entry->ptr, entry->saved,
             (size_t)(entry->range.end - entry->range.begin));
      rob_data_del(entry->saved, rob->mem);
      break;
    }
  }
}

int ngtcp2_rob_init(ngtcp2_rob *rob, size_t chunk, const ngtcp2_mem *mem) {
  int rv;
  static const ngtcp2_range g = {
    .end = UINT64_MAX,
  };

  ngtcp2_ksl_init(&rob->gapksl, ngtcp2_ksl_range_compar,
                  ngtcp2_ksl_range_search, sizeof(ngtcp2_range), mem);

  rv = ngtcp2_ksl_insert(&rob->gapksl, NULL, &g, NULL);
  if (rv != 0) {
    ngtcp2_ksl_free(&rob->gapksl);
    return rv;
  }

  ngtcp2_ksl_init(&rob->dataksl, ngtcp2_ksl_range_compar,
                  ngtcp2_ksl_range_search, sizeof(ngtcp2_range), mem);

  rob->chunk = chunk;
  rob->mem = mem;

  return 0;
}

void ngtcp2_rob_free(ngtcp2_rob *rob) {
  ngtcp2_ksl_it it;

  if (rob == NULL) {
    return;
  }

  for (it = ngtcp2_ksl_begin(&rob->dataksl); !ngtcp2_ksl_it_end(&it);
       ngtcp2_ksl_it_next(&it)) {
    rob_data_del(ngtcp2_ksl_it_get(&it), rob->mem);
  }

  ngtcp2_ksl_free(&rob->dataksl);
  ngtcp2_ksl_free(&rob->gapksl);
}

/*
 * rob_journal_discard releases the journal itself and the byte
 * snapshots it owns.  Chunk buffers recorded in the journal stay live
 * because dataksl keeps owning them after a successful commit.
 */
static void rob_journal_discard(rob_jentry *journal, size_t jn,
                                const ngtcp2_mem *mem) {
  size_t i;

  for (i = 0; i < jn; ++i) {
    if (journal[i].kind == ROB_JOURNAL_BYTES) {
      rob_data_del(journal[i].saved, mem);
    }
  }

  ngtcp2_mem_free(mem, journal);
}

/*
 * rob_write_data_atomic is the journaled counterpart of
 * rob_write_data.  New chunks are recorded as ROB_JOURNAL_CHUNK and
 * overwrites of pre-existing chunks as ROB_JOURNAL_BYTES with a
 * snapshot of the previous bytes.  On failure, all the changes made
 * here are undone by the caller through the journal.
 */
static int rob_write_data_atomic(ngtcp2_rob *rob, uint64_t offset,
                                 const uint8_t *data, size_t len,
                                 rob_jentry **pjournal, size_t *pcap,
                                 size_t *pn) {
  size_t n;
  int rv;
  uint8_t *d;
  ngtcp2_range range = {
    .begin = offset,
    .end = offset + len,
  };
  ngtcp2_ksl_it it;
  const ngtcp2_range *r;
  uint64_t chunk_offset;

  for (it = ngtcp2_ksl_lower_bound_search(&rob->dataksl, &range,
                                          ngtcp2_ksl_range_exclusive_search);
       len; ngtcp2_ksl_it_next(&it)) {
    if (ngtcp2_ksl_it_end(&it)) {
      d = NULL;
    } else {
      r = ngtcp2_ksl_it_key(&it);
      d = ngtcp2_ksl_it_get(&it);
    }

    if (d == NULL || offset < r->begin) {
      ngtcp2_range chunk_range;

      rv = rob_data_new(&d, rob->chunk, rob->mem);
      if (rv != 0) {
        return rv;
      }

      chunk_offset = (offset / rob->chunk) * rob->chunk;
      chunk_range = (ngtcp2_range){
        .begin = chunk_offset,
        .end = chunk_offset + rob->chunk,
      };

      rv = ngtcp2_ksl_insert_atomic(&rob->dataksl, &it, &chunk_range, d);
      if (rv != 0) {
        rob_data_del(d, rob->mem);
        return rv;
      }

      rv = rob_journal_push(
        pjournal, pcap, pn,
        (rob_jentry){
          .kind = ROB_JOURNAL_CHUNK, .range = chunk_range, .ptr = d,
        },
        rob->mem);
      if (rv != 0) {
        ngtcp2_ksl_remove(&rob->dataksl, NULL, &chunk_range);
        rob_data_del(d, rob->mem);
        return rv;
      }

      r = ngtcp2_ksl_it_key(&it);
    }

    n = (size_t)ngtcp2_min((uint64_t)len, r->end - offset);

    if (n) {
      uint8_t *saved = ngtcp2_mem_malloc(rob->mem, n);

      if (saved == NULL) {
        return NGTCP2_ERR_NOMEM;
      }

      memcpy(saved, d + (offset - r->begin), n);

      rv = rob_journal_push(
        pjournal, pcap, pn,
        (rob_jentry){
          .kind = ROB_JOURNAL_BYTES,
          .range = {
            .begin = offset,
            .end = offset + n,
          },
          .ptr = d + (offset - r->begin),
          .saved = saved,
        },
        rob->mem);
      if (rv != 0) {
        rob_data_del(saved, rob->mem);
        return rv;
      }
    }

    memcpy(d + (offset - r->begin), data, n);
    offset += n;
    data += n;
    len -= n;
  }

  return 0;
}

ngtcp2_ssize ngtcp2_rob_push_atomic(ngtcp2_rob *rob, uint64_t offset,
                                    const uint8_t *data, size_t datalen) {
  int rv;
  ngtcp2_range g;
  ngtcp2_range m, l, r;
  ngtcp2_range q = {
    .begin = offset,
    .end = offset + datalen,
  };
  ngtcp2_ksl_it it;
  ngtcp2_ssize nwrite = 0;
  size_t mlen;
  rob_jentry *journal = NULL;
  size_t jcap = 0, jn = 0;

  it = ngtcp2_ksl_lower_bound_search(&rob->gapksl, &q,
                                     ngtcp2_ksl_range_exclusive_search);

  for (; !ngtcp2_ksl_it_end(&it);) {
    g = *(const ngtcp2_range *)ngtcp2_ksl_it_key(&it);
    m = ngtcp2_range_intersect(&q, &g);

    mlen = (size_t)ngtcp2_range_len(&m);
    if (mlen == 0) {
      break;
    }

    if (ngtcp2_range_eq(&g, &m)) {
      ngtcp2_ksl_remove_hint(&rob->gapksl, &it, &it, &g);

      rv = rob_journal_push(
        &journal, &jcap, &jn,
        (rob_jentry){
          .kind = ROB_JOURNAL_GAP_REMOVE, .range = g,
        },
        rob->mem);
      if (rv != 0) {
        ngtcp2_ksl_insert_atomic(&rob->gapksl, NULL, &g, NULL);
        goto fail;
      }

      rv = rob_write_data_atomic(rob, m.begin, data + (m.begin - offset),
                                 mlen, &journal, &jcap, &jn);
      if (rv != 0) {
        goto fail;
      }

      nwrite += (ngtcp2_ssize)mlen;

      continue;
    }

    ngtcp2_range_cut(&l, &r, &g, &m);

    if (ngtcp2_range_len(&l)) {
      rv = rob_journal_push(
        &journal, &jcap, &jn,
        (rob_jentry){
          .kind = ROB_JOURNAL_GAP_UPDATE, .range = g, .new_range = l,
        },
        rob->mem);
      if (rv != 0) {
        goto fail;
      }

      ngtcp2_ksl_update_key(&rob->gapksl, &g, &l);

      if (ngtcp2_range_len(&r)) {
        rv = ngtcp2_ksl_insert_atomic(&rob->gapksl, &it, &r, NULL);
        if (rv != 0) {
          goto fail;
        }

        rv = rob_journal_push(
          &journal, &jcap, &jn,
          (rob_jentry){
            .kind = ROB_JOURNAL_GAP_INSERT, .range = r,
          },
          rob->mem);
        if (rv != 0) {
          ngtcp2_ksl_remove(&rob->gapksl, NULL, &r);
          goto fail;
        }
      }
    } else if (ngtcp2_range_len(&r)) {
      rv = rob_journal_push(
        &journal, &jcap, &jn,
        (rob_jentry){
          .kind = ROB_JOURNAL_GAP_UPDATE, .range = g, .new_range = r,
        },
        rob->mem);
      if (rv != 0) {
        goto fail;
      }

      ngtcp2_ksl_update_key(&rob->gapksl, &g, &r);
    }

    rv = rob_write_data_atomic(rob, m.begin, data + (m.begin - offset), mlen,
                               &journal, &jcap, &jn);
    if (rv != 0) {
      goto fail;
    }

    nwrite += (ngtcp2_ssize)mlen;

    ngtcp2_ksl_it_next(&it);
  }

  rob_journal_discard(journal, jn, rob->mem);

  return nwrite;

fail:
  rob_journal_rollback(rob, journal, jn);
  rob_journal_discard(journal, jn, rob->mem);

  return NGTCP2_ERR_NOMEM;
}

ngtcp2_ssize ngtcp2_rob_push(ngtcp2_rob *rob, uint64_t offset,
                             const uint8_t *data, size_t datalen) {
  return ngtcp2_rob_push_atomic(rob, offset, data, datalen);
}

void ngtcp2_rob_remove_prefix(ngtcp2_rob *rob, uint64_t offset) {
  ngtcp2_range g;
  ngtcp2_range r;
  uint8_t *d;
  ngtcp2_ksl_it it;

  it = ngtcp2_ksl_begin(&rob->gapksl);

  for (; !ngtcp2_ksl_it_end(&it);) {
    g = *(const ngtcp2_range *)ngtcp2_ksl_it_key(&it);
    if (offset <= g.begin) {
      break;
    }

    if (offset < g.end) {
      ngtcp2_ksl_update_key(&rob->gapksl, &g,
                            &(ngtcp2_range){
                              .begin = offset,
                              .end = g.end,
                            });

      break;
    }

    ngtcp2_ksl_remove_hint(&rob->gapksl, &it, &it, &g);
  }

  it = ngtcp2_ksl_begin(&rob->dataksl);

  for (; !ngtcp2_ksl_it_end(&it);) {
    r = *(const ngtcp2_range *)ngtcp2_ksl_it_key(&it);
    if (offset < r.end) {
      return;
    }

    d = ngtcp2_ksl_it_get(&it);

    ngtcp2_ksl_remove_hint(&rob->dataksl, &it, &it, &r);
    rob_data_del(d, rob->mem);
  }
}

size_t ngtcp2_rob_data_at(const ngtcp2_rob *rob, const uint8_t **pdest,
                          uint64_t offset) {
  const ngtcp2_range *g;
  const ngtcp2_range *r;
  uint8_t *d;
  ngtcp2_ksl_it it;

  it = ngtcp2_ksl_begin(&rob->gapksl);
  if (ngtcp2_ksl_it_end(&it)) {
    return 0;
  }

  g = ngtcp2_ksl_it_key(&it);

  if (g->begin <= offset) {
    return 0;
  }

  it = ngtcp2_ksl_begin(&rob->dataksl);
  r = ngtcp2_ksl_it_key(&it);
  d = ngtcp2_ksl_it_get(&it);

  assert(d);
  assert(r->begin <= offset);
  assert(offset < r->end);

  *pdest = d + (offset - r->begin);

  return (size_t)(ngtcp2_min(g->begin, r->end) - offset);
}

void ngtcp2_rob_pop(ngtcp2_rob *rob, uint64_t offset, size_t len) {
  ngtcp2_ksl_it it;
  ngtcp2_range r;
  uint8_t *d;

  it = ngtcp2_ksl_begin(&rob->dataksl);
  r = *(const ngtcp2_range *)ngtcp2_ksl_it_key(&it);
  d = ngtcp2_ksl_it_get(&it);

  assert(d);

  if (offset + len < r.end) {
    return;
  }

  ngtcp2_ksl_remove_hint(&rob->dataksl, NULL, &it, &r);
  rob_data_del(d, rob->mem);
}

uint64_t ngtcp2_rob_first_gap_offset(const ngtcp2_rob *rob) {
  ngtcp2_ksl_it it = ngtcp2_ksl_begin(&rob->gapksl);
  const ngtcp2_range *g;

  if (ngtcp2_ksl_it_end(&it)) {
    return UINT64_MAX;
  }

  g = ngtcp2_ksl_it_key(&it);

  return g->begin;
}

int ngtcp2_rob_data_buffered(const ngtcp2_rob *rob) {
  return ngtcp2_ksl_len(&rob->dataksl) != 0;
}
