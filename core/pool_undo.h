/*****************************************************************************
 * pool_undo.h
 *****************************************************************************
 * Copyright (C) 2010-2017 L-SMASH project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license. */

#ifndef LSMASH_POOL_UNDO_H
#define LSMASH_POOL_UNDO_H

#include "box.h"

/* Per-track state to revoke pooled (not yet written out) samples.
 * Allocated only when the undo mode is enabled on the track. */
typedef struct isom_pool_undo_tag isom_pool_undo_t;

/* Hooks called from the normal append/flush paths in isom.c.
 * Each of them is a no-op when the undo mode is disabled on the track. */

/* Called at the top of appending a sample, before any sample table is updated. */
int  isom_pool_undo_append_prologue( isom_trak_t *trak );
/* Called right after the currently pooled samples are committed because
 * the newly appended sample closed the current chunk (thresholds or
 * sample description switch). The new sample itself stays revocable. */
void isom_pool_undo_self_committed( isom_trak_t *trak );
/* Called right after the pooled samples of a track are committed by any
 * other path (explicit flush or inter-track arbitration). */
void isom_pool_undo_track_committed( isom_trak_t *trak );

void isom_pool_undo_remove( isom_pool_undo_t *undo );

/* Approximate heap bytes currently held by the undo state of a track.
 * Used to check the state does not grow with committed history. */
uint64_t isom_pool_undo_footprint( isom_trak_t *trak );

#endif
