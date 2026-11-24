/*
 * ngtcp2
 *
 * Copyright (c) 2018 ngtcp2 contributors
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
#include "ngtcp2_ksl.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "ngtcp2_macro.h"
#include "ngtcp2_mem.h"
#include "ngtcp2_err.h"

static ngtcp2_ksl_blk null_blk;

ngtcp2_objalloc_def(ksl_blk, ngtcp2_ksl_blk, oplent)

#define NGTCP2_KSL_ALIGNED_BLKLEN                                              \
  ((sizeof(ngtcp2_ksl_blk) + 0x7U) & ~(size_t)0x7U)

static size_t ksl_blklen(size_t aligned_keylen) {
  return NGTCP2_KSL_ALIGNED_BLKLEN + NGTCP2_KSL_MAX_NBLK * aligned_keylen;
}

/*
 * ksl_set_nth_key sets |key| to |n|th node under |blk|.
 */
static void ksl_set_nth_key(const ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk,
                            size_t n, const ngtcp2_ksl_key *key) {
  memcpy(blk->keys + n * ksl->aligned_keylen, key, ksl->keylen);
}

void ngtcp2_ksl_init(ngtcp2_ksl *ksl, ngtcp2_ksl_compar compar,
                     ngtcp2_ksl_search search, size_t keylen,
                     const ngtcp2_mem *mem) {
  size_t aligned_keylen;

  assert(keylen >= sizeof(uint64_t));

  aligned_keylen = (keylen + 0x7U) & ~(size_t)0x7U;

  assert(aligned_keylen <= UINT16_MAX);

  ngtcp2_objalloc_init(
    &ksl->blkalloc, (ksl_blklen(aligned_keylen) + 0xFU) & ~(size_t)0xFU, mem);

  ksl->root = NULL;
  ksl->front = ksl->back = NULL;
  ksl->compar = compar;
  ksl->search = search;
  ksl->n = 0;
  ksl->keylen = keylen;
  ksl->aligned_keylen = aligned_keylen;
}

static ngtcp2_ksl_blk *ksl_blk_objalloc_new(ngtcp2_ksl *ksl) {
  ngtcp2_ksl_blk *blk = ngtcp2_objalloc_ksl_blk_len_get(
    &ksl->blkalloc, ksl_blklen(ksl->aligned_keylen));

  if (!blk) {
    return NULL;
  }

  blk->keys = (uint8_t *)blk + NGTCP2_KSL_ALIGNED_BLKLEN;
  blk->aligned_keylen = (uint16_t)ksl->aligned_keylen;

  return blk;
}

static void ksl_blk_objalloc_del(ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk) {
  ngtcp2_objalloc_ksl_blk_release(&ksl->blkalloc, blk);
}

static int ksl_root_init(ngtcp2_ksl *ksl) {
  ngtcp2_ksl_blk *root = ksl_blk_objalloc_new(ksl);

  if (!root) {
    return NGTCP2_ERR_NOMEM;
  }

  root->next = root->prev = NULL;
  root->n = 0;
  root->leaf = 1;

  ksl->root = root;
  ksl->front = ksl->back = root;

  return 0;
}

/*
 * ksl_jentry_kind identifies the structural change recorded in a
 * ksl_jentry.
 */
typedef enum ksl_jentry_kind {
  /* KSL_JOURNAL_ROOT_INIT records that the root block was lazily
     created for an initially empty skip list. */
  KSL_JOURNAL_ROOT_INIT = 1,
  /* KSL_JOURNAL_ROOT_SPLIT records that a new root block was created
     and ksl->root was replaced. */
  KSL_JOURNAL_ROOT_SPLIT,
  /* KSL_JOURNAL_BLK_SPLIT records that blk was split and the new
     right block rblk was linked next to it. */
  KSL_JOURNAL_BLK_SPLIT,
  /* KSL_JOURNAL_NODE_SPLIT records that a parent block was expanded
     at the index i to reference rblk. */
  KSL_JOURNAL_NODE_SPLIT,
  /* KSL_JOURNAL_KEY_UPDATE records that the bounding key at the
     index i of blk was overwritten while descending to the rightmost
     subtree.  rblk is the child block whose maximum key is the value
     to restore. */
  KSL_JOURNAL_KEY_UPDATE
} ksl_jentry_kind;

/*
 * ksl_jentry is a single rollback record.  Leaf node insertion and key
 * updates are the final commits of an insertion, so they do not need
 * entries.
 */
typedef struct ksl_jentry {
  ksl_jentry_kind kind;
  /* blk is the block kept on the left side, or the old root block. */
  ngtcp2_ksl_blk *blk;
  /* rblk is the newly allocated right block, or the new root block. */
  ngtcp2_ksl_blk *rblk;
  /* i is the expanded node index for KSL_JOURNAL_NODE_SPLIT. */
  size_t i;
  /* n preserves the node count the block had before the change. */
  uint32_t n;
} ksl_jentry;

static int ksl_journal_push(ksl_jentry **pjournal, size_t *pcap, size_t *pn,
                            ksl_jentry entry, const ngtcp2_mem *mem) {
  ksl_jentry *journal;

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
 * ksl_journal_rollback undoes all the recorded structural changes in
 * the reverse order.  Every block allocated by the failed operation
 * is released, and the blocks which existed before the operation keep
 * their addresses and contents.
 */
static void ksl_journal_rollback(ngtcp2_ksl *ksl, ksl_jentry *journal,
                                 size_t jn) {
  for (; jn--;) {
    ksl_jentry *entry = &journal[jn];

    switch (entry->kind) {
    case KSL_JOURNAL_ROOT_INIT:
      ksl_blk_objalloc_del(ksl, entry->blk);
      ksl->root = ksl->front = ksl->back = NULL;
      break;
    case KSL_JOURNAL_ROOT_SPLIT:
      ksl_blk_objalloc_del(ksl, entry->rblk);
      ksl->root = entry->blk;
      break;
    case KSL_JOURNAL_NODE_SPLIT:
      /* The last key of rblk is the maximum key of the unsplit child,
         which is the parent key overwritten by the split. */
      ksl_set_nth_key(ksl, entry->blk, entry->i,
                      ngtcp2_ksl_blk_nth_key(entry->rblk,
                                             entry->rblk->n - 1));
      entry->blk->n = entry->n;
      break;
    case KSL_JOURNAL_BLK_SPLIT:
      entry->blk->next = entry->rblk->next;

      if (entry->rblk->next) {
        entry->rblk->next->prev = entry->blk;
      } else if (ksl->back == entry->rblk) {
        ksl->back = entry->blk;
      }

      /* The moved node and key arrays were copied, not moved, so
         restoring n brings back the original entries. */
      entry->blk->n = entry->n;
      ksl_blk_objalloc_del(ksl, entry->rblk);
      break;
    case KSL_JOURNAL_KEY_UPDATE:
      /* The child block is still linked at this point of the reverse
         walk, so its maximum key gives the overwritten bounding key. */
      ksl_set_nth_key(ksl, entry->blk, entry->i,
                      ngtcp2_ksl_blk_nth_key(entry->rblk,
                                             entry->rblk->n - 1));
      break;
    }
  }
}

#ifdef NOMEMPOOL
/*
 * ksl_free_blk frees |blk| recursively.
 */
static void ksl_free_blk(ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk) {
  size_t i;

  if (!blk->leaf) {
    for (i = 0; i < blk->n; ++i) {
      ksl_free_blk(ksl, blk->nodes[i].blk);
    }
  }

  ksl_blk_objalloc_del(ksl, blk);
}
#endif /* defined(NOMEMPOOL) */

void ngtcp2_ksl_free(ngtcp2_ksl *ksl) {
  if (!ksl) {
    return;
  }

#ifdef NOMEMPOOL
  if (ksl->root) {
    ksl_free_blk(ksl, ksl->root);
  }
#endif /* defined(NOMEMPOOL) */

  /* The object allocator is initialized by ngtcp2_ksl_init and owns
     every block ever allocated, including blocks returned to its free
     list after an atomic insertion rollback.  A NULL root therefore
     does not imply an empty allocator, so it must always be freed. */
  ngtcp2_objalloc_free(&ksl->blkalloc);
}

/*
 * ksl_split_blk splits |blk| into 2 ngtcp2_ksl_blk objects.  The new
 * ngtcp2_ksl_blk is always the "right" block.
 *
 * It returns the pointer to the ngtcp2_ksl_blk created which is the
 * located at the right of |blk|, or NULL which indicates out of
 * memory error.  On success, the change is recorded in |journal| so
 * that it can be rolled back later.
 */
static ngtcp2_ksl_blk *ksl_split_blk(ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk,
                                     ksl_jentry **pjournal, size_t *pcap,
                                     size_t *pn) {
  ngtcp2_ksl_blk *rblk;
  int rv;

  rblk = ksl_blk_objalloc_new(ksl);
  if (rblk == NULL) {
    return NULL;
  }

  rblk->next = blk->next;
  blk->next = rblk;

  if (rblk->next) {
    rblk->next->prev = rblk;
  } else if (ksl->back == blk) {
    ksl->back = rblk;
  }

  rblk->prev = blk;
  rblk->leaf = blk->leaf;

  rblk->n = blk->n / 2;
  blk->n -= rblk->n;

  memcpy(rblk->nodes, blk->nodes + blk->n, rblk->n * sizeof(ngtcp2_ksl_node));

  memcpy(rblk->keys, blk->keys + blk->n * ksl->aligned_keylen,
         rblk->n * ksl->aligned_keylen);

  assert(blk->n >= NGTCP2_KSL_MIN_NBLK);
  assert(rblk->n >= NGTCP2_KSL_MIN_NBLK);

  rv = ksl_journal_push(
    pjournal, pcap, pn,
    (ksl_jentry){
      .kind = KSL_JOURNAL_BLK_SPLIT, .blk = blk, .rblk = rblk,
      /* blk->n was reduced above; the entry stores the original
         count so that rollback can restore it. */
      .n = blk->n + rblk->n,
    },
    ksl->blkalloc.balloc.mem);
  if (rv != 0) {
    /* Undo this split inline so that the tree stays consistent with
       the journal entries recorded so far; the caller performs the
       single final rollback. */
    blk->next = rblk->next;

    if (rblk->next) {
      rblk->next->prev = blk;
    } else if (ksl->back == rblk) {
      ksl->back = blk;
    }

    blk->n += rblk->n;
    ksl_blk_objalloc_del(ksl, rblk);

    return NULL;
  }

  return rblk;
}

/*
 * ksl_split_node splits a node included in |blk| at the position |i|
 * into 2 adjacent nodes.  The new node is always inserted at the
 * position |i+1|.  The change is recorded in |journal|.
 *
 * It returns 0 if it succeeds, or one of the following negative error
 * codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.  All the changes made in this call are rolled
 *     back.
 */
static int ksl_split_node(ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk, size_t i,
                          ksl_jentry **pjournal, size_t *pcap, size_t *pn) {
  ngtcp2_ksl_blk *lblk = blk->nodes[i].blk, *rblk;
  int rv;

  rblk = ksl_split_blk(ksl, lblk, pjournal, pcap, pn);
  if (rblk == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  memmove(blk->nodes + (i + 2), blk->nodes + (i + 1),
          (blk->n - (i + 1)) * sizeof(ngtcp2_ksl_node));

  memmove(blk->keys + (i + 1) * ksl->aligned_keylen,
          blk->keys + i * ksl->aligned_keylen,
          (blk->n - i) * ksl->aligned_keylen);

  blk->nodes[i + 1].blk = rblk;
  ++blk->n;

  ksl_set_nth_key(ksl, blk, i, ngtcp2_ksl_blk_nth_key(lblk, lblk->n - 1));

  rv = ksl_journal_push(
    pjournal, pcap, pn,
    (ksl_jentry){
      .kind = KSL_JOURNAL_NODE_SPLIT, .blk = blk, .rblk = rblk, .i = i,
      /* blk->n was just incremented; the entry stores the previous
         count. */
      .n = blk->n - 1,
    },
    ksl->blkalloc.balloc.mem);
  if (rv != 0) {
    /* Undo the parent expansion inline; the child split stays
       journaled and is undone by the final rollback. */
    ksl_set_nth_key(ksl, blk, i,
                    ngtcp2_ksl_blk_nth_key(rblk, rblk->n - 1));

    memmove(blk->nodes + (i + 1), blk->nodes + (i + 2),
            (blk->n - (i + 2)) * sizeof(ngtcp2_ksl_node));

    memmove(blk->keys + i * ksl->aligned_keylen,
            blk->keys + (i + 1) * ksl->aligned_keylen,
            (blk->n - (i + 1)) * ksl->aligned_keylen);

    blk->n -= 1;

    return NGTCP2_ERR_NOMEM;
  }

  return 0;
}

/*
 * ksl_split_root splits a root block.  It increases the height of
 * skip list by 1.  The change is recorded in |journal|.
 *
 * It returns 0 if it succeeds, or one of the following negative error
 * codes:
 *
 * NGTCP2_ERR_NOMEM
 *     Out of memory.  All the changes made in this call are rolled
 *     back.
 */
static int ksl_split_root(ngtcp2_ksl *ksl, ksl_jentry **pjournal,
                          size_t *pcap, size_t *pn) {
  ngtcp2_ksl_blk *rblk, *lblk, *nroot;
  ngtcp2_ksl_blk *old_root = ksl->root;
  int rv;

  nroot = ksl_blk_objalloc_new(ksl);
  if (nroot == NULL) {
    return NGTCP2_ERR_NOMEM;
  }

  rblk = ksl_split_blk(ksl, ksl->root, pjournal, pcap, pn);
  if (rblk == NULL) {
    ksl_blk_objalloc_del(ksl, nroot);
    return NGTCP2_ERR_NOMEM;
  }

  lblk = ksl->root;

  nroot->next = nroot->prev = NULL;
  nroot->n = 2;
  nroot->leaf = 0;

  ksl_set_nth_key(ksl, nroot, 0, ngtcp2_ksl_blk_nth_key(lblk, lblk->n - 1));
  nroot->nodes[0].blk = lblk;

  ksl_set_nth_key(ksl, nroot, 1, ngtcp2_ksl_blk_nth_key(rblk, rblk->n - 1));
  nroot->nodes[1].blk = rblk;

  ksl->root = nroot;

  rv = ksl_journal_push(
    pjournal, pcap, pn,
    (ksl_jentry){
      .kind = KSL_JOURNAL_ROOT_SPLIT, .blk = old_root, .rblk = nroot,
    },
    ksl->blkalloc.balloc.mem);
  if (rv != 0) {
    /* Restore the old root inline; nroot is not journaled yet and is
       released here.  The leaf block split stays journaled and is
       undone by the final rollback. */
    ksl->root = old_root;
    ksl_blk_objalloc_del(ksl, nroot);

    return NGTCP2_ERR_NOMEM;
  }

  return 0;
}

/*
 * ksl_journal_key_update overwrites the bounding key at the index i
 * of blk with the inserted key and records the previous value, which
 * is derived from the maximum key of the linked child block.
 */
static int ksl_journal_key_update(ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk,
                                  size_t i, ngtcp2_ksl_blk *child,
                                  const ngtcp2_ksl_key *key,
                                  ksl_jentry **pjournal, size_t *pcap,
                                  size_t *pn) {
  int rv = ksl_journal_push(
    pjournal, pcap, pn,
    (ksl_jentry){
      .kind = KSL_JOURNAL_KEY_UPDATE, .blk = blk, .rblk = child, .i = i,
    },
    ksl->blkalloc.balloc.mem);

  if (rv != 0) {
    return rv;
  }

  ksl_set_nth_key(ksl, blk, i, key);

  return 0;
}

/*
 * ksl_insert_node inserts a node whose key is |key| with the
 * associated |data| at the index of |i|.  This function assumes that
 * the number of nodes contained by |blk| is strictly less than
 * NGTCP2_KSL_MAX_NBLK.
 */
static void ksl_insert_node(ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk, size_t i,
                            const ngtcp2_ksl_key *key, void *data) {
  assert(blk->n < NGTCP2_KSL_MAX_NBLK);

  memmove(blk->nodes + (i + 1), blk->nodes + i,
          (blk->n - i) * sizeof(ngtcp2_ksl_node));

  memmove(blk->keys + (i + 1) * ksl->aligned_keylen,
          blk->keys + i * ksl->aligned_keylen,
          (blk->n - i) * ksl->aligned_keylen);

  ksl_set_nth_key(ksl, blk, i, key);
  blk->nodes[i].data = data;

  ++blk->n;
}

static int ksl_insert_atomic(ngtcp2_ksl *ksl, ngtcp2_ksl_it *it,
                             const ngtcp2_ksl_key *key, void *data) {
  ngtcp2_ksl_blk *blk;
  ngtcp2_ksl_node *node;
  ksl_jentry *journal = NULL;
  size_t jcap = 0, jn = 0;
  size_t i;
  int rv;

  if (!ksl->root) {
    rv = ksl_root_init(ksl);
    if (rv != 0) {
      return rv;
    }

    rv = ksl_journal_push(
      &journal, &jcap, &jn,
      (ksl_jentry){
        .kind = KSL_JOURNAL_ROOT_INIT, .blk = ksl->root,
      },
      ksl->blkalloc.balloc.mem);
    if (rv != 0) {
      /* The entry could not be recorded, so undo the lazy root
         creation inline. */
      ksl_blk_objalloc_del(ksl, ksl->root);
      ksl->root = ksl->front = ksl->back = NULL;
      goto fail;
    }
  }

  if (ksl->root->n == NGTCP2_KSL_MAX_NBLK) {
    rv = ksl_split_root(ksl, &journal, &jcap, &jn);
    if (rv != 0) {
      ksl_journal_rollback(ksl, journal, jn);
      goto fail;
    }
  }

  blk = ksl->root;

  for (;;) {
    i = ksl->search(ksl, blk, key);

    if (blk->leaf) {
      if (i < blk->n && !ksl->compar(key, ngtcp2_ksl_blk_nth_key(blk, i))) {
        if (it) {
          *it = ngtcp2_ksl_end(ksl);
        }

        /* The duplicated key check happens before the leaf node is
           published, so roll the structural changes back and keep the
           original error code. */
        ksl_journal_rollback(ksl, journal, jn);
        ngtcp2_mem_free(ksl->blkalloc.balloc.mem, journal);

        return NGTCP2_ERR_INVALID_ARGUMENT;
      }

      /* Leaf node insertion and the element count update are the only
         commits of this operation; from here on no allocation can
         fail. */
      ksl_insert_node(ksl, blk, i, key, data);
      ++ksl->n;

      if (it) {
        ngtcp2_ksl_it_init(it, blk, i);
      }

      ngtcp2_mem_free(ksl->blkalloc.balloc.mem, journal);

      return 0;
    }

    if (i == blk->n) {
      /* This insertion extends the largest key in this subtree. */
      for (; !blk->leaf;) {
        node = &blk->nodes[blk->n - 1];
        if (node->blk->n == NGTCP2_KSL_MAX_NBLK) {
          rv = ksl_split_node(ksl, blk, blk->n - 1, &journal, &jcap, &jn);
          if (rv != 0) {
            ksl_journal_rollback(ksl, journal, jn);
            goto fail;
          }

          node = &blk->nodes[blk->n - 1];
        }

        rv = ksl_journal_key_update(ksl, blk, blk->n - 1, node->blk, key,
                                    &journal, &jcap, &jn);
        if (rv != 0) {
          ksl_journal_rollback(ksl, journal, jn);
          goto fail;
        }

        blk = node->blk;
      }

      ksl_insert_node(ksl, blk, blk->n, key, data);
      ++ksl->n;

      if (it) {
        ngtcp2_ksl_it_init(it, blk, blk->n - 1);
      }

      ngtcp2_mem_free(ksl->blkalloc.balloc.mem, journal);

      return 0;
    }

    node = &blk->nodes[i];

    if (node->blk->n == NGTCP2_KSL_MAX_NBLK) {
      rv = ksl_split_node(ksl, blk, i, &journal, &jcap, &jn);
      if (rv != 0) {
        ksl_journal_rollback(ksl, journal, jn);
        goto fail;
      }

      if (ksl->compar(ngtcp2_ksl_blk_nth_key(blk, i), key)) {
        node = &blk->nodes[i + 1];
      }
    }

    blk = node->blk;
  }

fail:
  ngtcp2_mem_free(ksl->blkalloc.balloc.mem, journal);

  return NGTCP2_ERR_NOMEM;
}

int ngtcp2_ksl_insert(ngtcp2_ksl *ksl, ngtcp2_ksl_it *it,
                      const ngtcp2_ksl_key *key, void *data) {
  return ksl_insert_atomic(ksl, it, key, data);
}

int ngtcp2_ksl_insert_atomic(ngtcp2_ksl *ksl, ngtcp2_ksl_it *it,
                             const ngtcp2_ksl_key *key, void *data) {
  return ksl_insert_atomic(ksl, it, key, data);
}

/*
 * ksl_remove_node removes the node included in |blk| at the index of
 * |i|.
 */
static void ksl_remove_node(ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk, size_t i) {
  memmove(blk->nodes + i, blk->nodes + (i + 1),
          (blk->n - (i + 1)) * sizeof(ngtcp2_ksl_node));

  memmove(blk->keys + i * ksl->aligned_keylen,
          blk->keys + (i + 1) * ksl->aligned_keylen,
          (blk->n - (i + 1)) * ksl->aligned_keylen);

  --blk->n;
}

/*
 * ksl_merge_node merges 2 nodes which are the nodes at the index of
 * |i| and |i + 1|.
 *
 * If |blk| is the root block and it contains just 2 nodes before
 * merging nodes, the merged block becomes root block, which decreases
 * the height of |ksl| by 1.
 *
 * This function returns the pointer to the merged block.
 */
static ngtcp2_ksl_blk *ksl_merge_node(ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk,
                                      size_t i) {
  ngtcp2_ksl_node *lnode;
  ngtcp2_ksl_blk *lblk, *rblk;

  assert(i + 1 < blk->n);

  lnode = &blk->nodes[i];

  lblk = lnode->blk;
  rblk = blk->nodes[i + 1].blk;

  assert(lblk->n + rblk->n <= NGTCP2_KSL_MAX_NBLK);

  memcpy(lblk->nodes + lblk->n, rblk->nodes, rblk->n * sizeof(ngtcp2_ksl_node));

  memcpy(lblk->keys + lblk->n * ksl->aligned_keylen, rblk->keys,
         rblk->n * ksl->aligned_keylen);

  lblk->n += rblk->n;
  lblk->next = rblk->next;

  if (lblk->next) {
    lblk->next->prev = lblk;
  } else if (ksl->back == rblk) {
    ksl->back = lblk;
  }

  ksl_blk_objalloc_del(ksl, rblk);

  if (ksl->root == blk && blk->n == 2) {
    ksl_blk_objalloc_del(ksl, ksl->root);
    ksl->root = lblk;
  } else {
    ksl_remove_node(ksl, blk, i + 1);
    ksl_set_nth_key(ksl, blk, i, ngtcp2_ksl_blk_nth_key(lblk, lblk->n - 1));
  }

  return lblk;
}

/*
 * ksl_shift_left moves the first nodes in blk->nodes[i]->blk->nodes
 * to blk->nodes[i - 1]->blk->nodes in a manner that they have the
 * same amount of nodes as much as possible.
 */
static void ksl_shift_left(ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk, size_t i) {
  ngtcp2_ksl_node *lnode, *rnode;
  ngtcp2_ksl_blk *lblk, *rblk;
  size_t n;

  assert(i > 0);

  lnode = &blk->nodes[i - 1];
  rnode = &blk->nodes[i];

  lblk = lnode->blk;
  rblk = rnode->blk;

  assert(lblk->n < NGTCP2_KSL_MAX_NBLK);
  assert(rblk->n > NGTCP2_KSL_MIN_NBLK);

  n = (lblk->n + rblk->n + 1) / 2 - lblk->n;

  assert(n > 0);
  assert(lblk->n <= NGTCP2_KSL_MAX_NBLK - n);
  assert(rblk->n >= NGTCP2_KSL_MIN_NBLK + n);

  memcpy(lblk->nodes + lblk->n, rblk->nodes, n * sizeof(ngtcp2_ksl_node));

  memcpy(lblk->keys + lblk->n * ksl->aligned_keylen, rblk->keys,
         n * ksl->aligned_keylen);

  lblk->n += (uint32_t)n;
  rblk->n -= (uint32_t)n;

  ksl_set_nth_key(ksl, blk, i - 1, ngtcp2_ksl_blk_nth_key(lblk, lblk->n - 1));

  memmove(rblk->nodes, rblk->nodes + n, rblk->n * sizeof(ngtcp2_ksl_node));

  memmove(rblk->keys, rblk->keys + n * ksl->aligned_keylen,
          rblk->n * ksl->aligned_keylen);
}

/*
 * ksl_shift_right moves the last nodes in blk->nodes[i]->blk->nodes
 * to blk->nodes[i + 1]->blk->nodes in a manner that they have the
 * same amount of nodes as much as possible.
 */
static void ksl_shift_right(ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk, size_t i) {
  ngtcp2_ksl_node *lnode, *rnode;
  ngtcp2_ksl_blk *lblk, *rblk;
  size_t n;

  assert(i < blk->n - 1);

  lnode = &blk->nodes[i];
  rnode = &blk->nodes[i + 1];

  lblk = lnode->blk;
  rblk = rnode->blk;

  assert(lblk->n > NGTCP2_KSL_MIN_NBLK);
  assert(rblk->n < NGTCP2_KSL_MAX_NBLK);

  n = (lblk->n + rblk->n + 1) / 2 - rblk->n;

  assert(n > 0);
  assert(lblk->n >= NGTCP2_KSL_MIN_NBLK + n);
  assert(rblk->n <= NGTCP2_KSL_MAX_NBLK - n);

  memmove(rblk->nodes + n, rblk->nodes, rblk->n * sizeof(ngtcp2_ksl_node));

  memmove(rblk->keys + n * ksl->aligned_keylen, rblk->keys,
          rblk->n * ksl->aligned_keylen);

  rblk->n += (uint32_t)n;
  lblk->n -= (uint32_t)n;

  memcpy(rblk->nodes, lblk->nodes + lblk->n, n * sizeof(ngtcp2_ksl_node));

  memcpy(rblk->keys, lblk->keys + lblk->n * ksl->aligned_keylen,
         n * ksl->aligned_keylen);

  ksl_set_nth_key(ksl, blk, i, ngtcp2_ksl_blk_nth_key(lblk, lblk->n - 1));
}

/*
 * key_equal returns nonzero if |lhs| and |rhs| are equal using the
 * function |compar|.
 */
static int key_equal(ngtcp2_ksl_compar compar, const ngtcp2_ksl_key *lhs,
                     const ngtcp2_ksl_key *rhs) {
  return !compar(lhs, rhs) && !compar(rhs, lhs);
}

int ngtcp2_ksl_remove_hint(ngtcp2_ksl *ksl, ngtcp2_ksl_it *it,
                           const ngtcp2_ksl_it *hint,
                           const ngtcp2_ksl_key *key) {
  ngtcp2_ksl_blk *blk = hint->blk;

  assert(ksl->root);

  if (blk != ksl->root && blk->n == NGTCP2_KSL_MIN_NBLK) {
    return ngtcp2_ksl_remove(ksl, it, key);
  }

  ksl_remove_node(ksl, blk, hint->i);

  --ksl->n;

  if (it) {
    if (hint->i == blk->n && blk->next) {
      ngtcp2_ksl_it_init(it, blk->next, 0);
    } else {
      ngtcp2_ksl_it_init(it, blk, hint->i);
    }
  }

  return 0;
}

int ngtcp2_ksl_remove(ngtcp2_ksl *ksl, ngtcp2_ksl_it *it,
                      const ngtcp2_ksl_key *key) {
  ngtcp2_ksl_blk *blk = ksl->root;
  ngtcp2_ksl_node *node;
  size_t i;

  if (!blk) {
    return NGTCP2_ERR_INVALID_ARGUMENT;
  }

  if (!blk->leaf && blk->n == 2 &&
      blk->nodes[0].blk->n == NGTCP2_KSL_MIN_NBLK &&
      blk->nodes[1].blk->n == NGTCP2_KSL_MIN_NBLK) {
    blk = ksl_merge_node(ksl, blk, 0);
  }

  for (;;) {
    i = ksl->search(ksl, blk, key);

    if (i == blk->n) {
      if (it) {
        *it = ngtcp2_ksl_end(ksl);
      }

      return NGTCP2_ERR_INVALID_ARGUMENT;
    }

    if (blk->leaf) {
      if (ksl->compar(key, ngtcp2_ksl_blk_nth_key(blk, i))) {
        if (it) {
          *it = ngtcp2_ksl_end(ksl);
        }

        return NGTCP2_ERR_INVALID_ARGUMENT;
      }

      ksl_remove_node(ksl, blk, i);
      --ksl->n;

      if (it) {
        if (blk->n == i && blk->next) {
          ngtcp2_ksl_it_init(it, blk->next, 0);
        } else {
          ngtcp2_ksl_it_init(it, blk, i);
        }
      }

      return 0;
    }

    node = &blk->nodes[i];

    if (node->blk->n > NGTCP2_KSL_MIN_NBLK) {
      blk = node->blk;
      continue;
    }

    assert(node->blk->n == NGTCP2_KSL_MIN_NBLK);

    if (i + 1 < blk->n && blk->nodes[i + 1].blk->n > NGTCP2_KSL_MIN_NBLK) {
      ksl_shift_left(ksl, blk, i + 1);
      blk = node->blk;

      continue;
    }

    if (i > 0 && blk->nodes[i - 1].blk->n > NGTCP2_KSL_MIN_NBLK) {
      ksl_shift_right(ksl, blk, i - 1);
      blk = node->blk;

      continue;
    }

    if (i + 1 < blk->n) {
      blk = ksl_merge_node(ksl, blk, i);
      continue;
    }

    assert(i > 0);

    blk = ksl_merge_node(ksl, blk, i - 1);
  }
}

ngtcp2_ksl_it ngtcp2_ksl_lower_bound(const ngtcp2_ksl *ksl,
                                     const ngtcp2_ksl_key *key) {
  return ngtcp2_ksl_lower_bound_search(ksl, key, ksl->search);
}

ngtcp2_ksl_it ngtcp2_ksl_lower_bound_search(const ngtcp2_ksl *ksl,
                                            const ngtcp2_ksl_key *key,
                                            ngtcp2_ksl_search search) {
  ngtcp2_ksl_blk *blk = ksl->root;
  ngtcp2_ksl_it it;
  size_t i;

  if (!blk) {
    ngtcp2_ksl_it_init(&it, &null_blk, 0);
    return it;
  }

  for (;;) {
    i = search(ksl, blk, key);

    if (blk->leaf) {
      if (i == blk->n && blk->next) {
        blk = blk->next;
        i = 0;
      }

      ngtcp2_ksl_it_init(&it, blk, i);

      return it;
    }

    if (i == blk->n) {
      /* This happens if descendant has smaller key.  Fast forward to
         find last node in this subtree. */
      for (; !blk->leaf; blk = blk->nodes[blk->n - 1].blk)
        ;

      if (blk->next) {
        blk = blk->next;
        i = 0;
      } else {
        i = blk->n;
      }

      ngtcp2_ksl_it_init(&it, blk, i);

      return it;
    }

    blk = blk->nodes[i].blk;
  }
}

void ngtcp2_ksl_update_key(ngtcp2_ksl *ksl, const ngtcp2_ksl_key *old_key,
                           const ngtcp2_ksl_key *new_key) {
  ngtcp2_ksl_blk *blk = ksl->root;
  ngtcp2_ksl_node *node;
  const ngtcp2_ksl_key *node_key;
  size_t i;

  assert(ksl->root);

  for (;;) {
    i = ksl->search(ksl, blk, old_key);

    assert(i < blk->n);
    node = &blk->nodes[i];
    node_key = ngtcp2_ksl_blk_nth_key(blk, i);

    if (blk->leaf) {
      assert(key_equal(ksl->compar, node_key, old_key));
      ksl_set_nth_key(ksl, blk, i, new_key);

      return;
    }

    if (key_equal(ksl->compar, node_key, old_key) ||
        ksl->compar(node_key, new_key)) {
      ksl_set_nth_key(ksl, blk, i, new_key);
    }

    blk = node->blk;
  }
}

size_t ngtcp2_ksl_len(const ngtcp2_ksl *ksl) { return ksl->n; }

void ngtcp2_ksl_clear(ngtcp2_ksl *ksl) {
  if (!ksl->root) {
    return;
  }

#ifdef NOMEMPOOL
  ksl_free_blk(ksl, ksl->root);
#endif /* defined(NOMEMPOOL) */

  ksl->front = ksl->back = ksl->root = NULL;
  ksl->n = 0;

  ngtcp2_objalloc_clear(&ksl->blkalloc);
}

#ifndef WIN32
static void ksl_print(const ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk,
                      size_t level) {
  size_t i;

  fprintf(stderr, "LV=%zu n=%u\n", level, blk->n);

  if (blk->leaf) {
    for (i = 0; i < blk->n; ++i) {
      fprintf(stderr, " %" PRId64, *(int64_t *)ngtcp2_ksl_blk_nth_key(blk, i));
    }

    fprintf(stderr, "\n");

    return;
  }

  for (i = 0; i < blk->n; ++i) {
    ksl_print(ksl, blk->nodes[i].blk, level + 1);
  }
}

void ngtcp2_ksl_print(const ngtcp2_ksl *ksl) {
  if (!ksl->root) {
    return;
  }

  ksl_print(ksl, ksl->root, 0);
}
#endif /* !defined(WIN32) */

ngtcp2_ksl_it ngtcp2_ksl_begin(const ngtcp2_ksl *ksl) {
  ngtcp2_ksl_it it;

  if (ksl->root) {
    ngtcp2_ksl_it_init(&it, ksl->front, 0);
  } else {
    ngtcp2_ksl_it_init(&it, &null_blk, 0);
  }

  return it;
}

ngtcp2_ksl_it ngtcp2_ksl_end(const ngtcp2_ksl *ksl) {
  ngtcp2_ksl_it it;

  if (ksl->root) {
    ngtcp2_ksl_it_init(&it, ksl->back, ksl->back->n);
  } else {
    ngtcp2_ksl_it_init(&it, &null_blk, 0);
  }

  return it;
}

void ngtcp2_ksl_it_init(ngtcp2_ksl_it *it, ngtcp2_ksl_blk *blk, size_t i) {
  it->blk = blk;
  it->i = i;
}

void ngtcp2_ksl_it_prev(ngtcp2_ksl_it *it) {
  assert(!ngtcp2_ksl_it_begin(it));

  if (it->i == 0) {
    it->blk = it->blk->prev;
    it->i = it->blk->n - 1;
  } else {
    --it->i;
  }
}

int ngtcp2_ksl_it_begin(const ngtcp2_ksl_it *it) {
  return it->i == 0 && it->blk->prev == NULL;
}

ngtcp2_ksl_search_def(range, ngtcp2_ksl_range_compar)

size_t ngtcp2_ksl_range_search(const ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk,
                               const ngtcp2_ksl_key *key) {
  return ksl_range_search(ksl, blk, key);
}

ngtcp2_ksl_search_def(range_exclusive, ngtcp2_ksl_range_exclusive_compar)

size_t ngtcp2_ksl_range_exclusive_search(const ngtcp2_ksl *ksl,
                                         ngtcp2_ksl_blk *blk,
                                         const ngtcp2_ksl_key *key) {
  return ksl_range_exclusive_search(ksl, blk, key);
}

ngtcp2_ksl_search_def(uint64_less, ngtcp2_ksl_uint64_less)

size_t ngtcp2_ksl_uint64_less_search(const ngtcp2_ksl *ksl, ngtcp2_ksl_blk *blk,
                                     const ngtcp2_ksl_key *key) {
  return ksl_uint64_less_search(ksl, blk, key);
}

ngtcp2_ksl_search_def(int64_greater, ngtcp2_ksl_int64_greater)

size_t ngtcp2_ksl_int64_greater_search(const ngtcp2_ksl *ksl,
                                       ngtcp2_ksl_blk *blk,
                                       const ngtcp2_ksl_key *key) {
  return ksl_int64_greater_search(ksl, blk, key);
}
