/*****************************************************************************
 * pool_undo.c
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

#include "common/internal.h" /* must be placed first */

#include <stdlib.h>
#include <string.h>

#include "box.h"
#include "pool_undo.h"

/* A point-in-time copy of every piece of per-track state that appending a
 * sample may mutate before the sample's payload is actually written out.
 * Restoring a snapshot makes the track look as if the samples appended
 * after the snapshot was taken had never been appended.
 *
 * Only the samples still sitting in the chunk pool can be revoked, so a
 * snapshot never covers data already committed to the output stream:
 *  - stsc/stco entry counts are taken after the commit that started the
 *    current pool generation, so committed chunk records are kept.
 *  - Media data bytes, file->size and mdat->media_size are never rewound.
 *
 * The variable-length parts are bounded by the number of pooled samples
 * and the number of not yet completed roll recovery groups; nothing here
 * scales with the number of chunks already written. */
typedef struct
{
    /* Sample Size Box */
    uint32_t stsz_sample_count;
    uint32_t stsz_sample_size;
    uint32_t stsz_entry_count;
    int      stsz_had_list;
    /* Decoding Time to Sample Box */
    uint32_t          stts_entry_count;
    isom_stts_entry_t stts_tail;
    /* Composition Time to Sample Box */
    int               ctts_existed;
    uint32_t          ctts_version;
    uint32_t          ctts_entry_count;
    isom_ctts_entry_t ctts_tail;
    /* Sync / Partial Sync / Independent and Disposable Samples Boxes */
    int      stss_existed;
    uint32_t stss_entry_count;
    int      stps_existed;
    uint32_t stps_entry_count;
    int      sdtp_existed;
    uint32_t sdtp_entry_count;
    /* Chunk tables (kept entries include the ones committed so far) */
    uint32_t stsc_entry_count;
    uint32_t stco_entry_count;
    /* Random access point grouping */
    uint32_t                     rap_sbgp_entry_count;
    isom_group_assignment_entry_t rap_sbgp_tail[2]; /* [0]: tail, [1]: previous */
    uint32_t                     rap_sgpd_entry_count;
    isom_rap_entry_t             rap_sgpd_tail;
    uint8_t  rap_cache_existed;
    uint8_t  rap_is_prev_rap;
    uint8_t  rap_random_access_open;
    /* Roll recovery grouping */
    uint32_t                      roll_sbgp_entry_count;
    uint32_t                      roll_sbgp_keep;       /* number of copied tail entries */
    isom_group_assignment_entry_t *roll_sbgp_entries;   /* [roll_sbgp_keep] */
    uint32_t                      roll_sgpd_entry_count;
    uint32_t                      roll_sgpd_ref_count;
    struct pool_undo_roll_ref
    {
        uint32_t         index;     /* 1-based group_description_index */
        isom_roll_entry_t entry;
    }                            *roll_sgpd_refs;       /* [roll_sgpd_ref_count] */
    int               roll_pool_existed;
    uint32_t          roll_pool_count;
    isom_roll_group_t *roll_groups;                     /* [roll_pool_count] */
    /* Cached timestamps and chunk state */
    uint64_t dts;
    uint64_t cts;
    int32_t  ctd_shift;
    uint8_t  all_sync;
    uint32_t chunk_number;
    uint32_t chunk_sample_description_index;
    uint64_t chunk_first_dts;
} pool_undo_snapshot_t;

struct isom_pool_undo_tag
{
    int                  armed; /* 'gen' is a valid restore point for the current pool */
    pool_undo_snapshot_t gen;   /* restore point of the current pool generation */
    pool_undo_snapshot_t last;  /* state as of the latest append, used when that
                                 * append itself commits the previous pool */
};

static void snapshot_release( pool_undo_snapshot_t *snapshot )
{
    lsmash_free( snapshot->roll_sbgp_entries );
    lsmash_free( snapshot->roll_sgpd_refs );
    lsmash_free( snapshot->roll_groups );
    snapshot->roll_sbgp_entries  = NULL;
    snapshot->roll_sgpd_refs     = NULL;
    snapshot->roll_groups        = NULL;
    snapshot->roll_sbgp_keep     = 0;
    snapshot->roll_sgpd_ref_count = 0;
    snapshot->roll_pool_count    = 0;
}

void isom_pool_undo_remove( isom_pool_undo_t *undo )
{
    if( !undo )
        return;
    snapshot_release( &undo->gen );
    snapshot_release( &undo->last );
    lsmash_free( undo );
}

static void truncate_list( lsmash_entry_list_t *list, uint32_t entry_count )
{
    while( list->entry_count > entry_count )
        lsmash_list_remove_entry_tail( list );
}

static int snapshot_capture( isom_trak_t *trak, pool_undo_snapshot_t *snapshot )
{
    isom_cache_t *cache = trak->cache;
    isom_stbl_t  *stbl  = trak->mdia->minf->stbl;
    snapshot_release( snapshot );
    /* Sample Size Box */
    isom_stsz_t *stsz = stbl->stsz;
    if( LSMASH_IS_EXISTING_BOX( stsz ) )
    {
        snapshot->stsz_sample_count = stsz->sample_count;
        snapshot->stsz_sample_size  = stsz->sample_size;
        snapshot->stsz_had_list     = stsz->list != NULL;
        snapshot->stsz_entry_count  = stsz->list ? stsz->list->entry_count : 0;
    }
    else
    {
        snapshot->stsz_sample_count = 0;
        snapshot->stsz_sample_size  = 0;
        snapshot->stsz_had_list     = 0;
        snapshot->stsz_entry_count  = 0;
    }
    /* Decoding Time to Sample Box */
    snapshot->stts_entry_count = stbl->stts->list ? stbl->stts->list->entry_count : 0;
    if( snapshot->stts_entry_count )
        snapshot->stts_tail = *(isom_stts_entry_t *)stbl->stts->list->tail->data;
    /* Composition Time to Sample Box */
    isom_ctts_t *ctts = stbl->ctts;
    snapshot->ctts_existed = LSMASH_IS_EXISTING_BOX( ctts );
    if( snapshot->ctts_existed )
    {
        snapshot->ctts_version     = ctts->version;
        snapshot->ctts_entry_count = ctts->list ? ctts->list->entry_count : 0;
        if( snapshot->ctts_entry_count )
            snapshot->ctts_tail = *(isom_ctts_entry_t *)ctts->list->tail->data;
    }
    /* Sync Sample Box */
    isom_stss_t *stss = stbl->stss;
    snapshot->stss_existed     = LSMASH_IS_EXISTING_BOX( stss );
    snapshot->stss_entry_count = snapshot->stss_existed && stss->list ? stss->list->entry_count : 0;
    /* Partial Sync Sample Box */
    isom_stps_t *stps = stbl->stps;
    snapshot->stps_existed     = LSMASH_IS_EXISTING_BOX( stps );
    snapshot->stps_entry_count = snapshot->stps_existed && stps->list ? stps->list->entry_count : 0;
    /* Independent and Disposable Samples Box */
    isom_sdtp_t *sdtp = stbl->sdtp;
    snapshot->sdtp_existed     = LSMASH_IS_EXISTING_BOX( sdtp );
    snapshot->sdtp_entry_count = snapshot->sdtp_existed && sdtp->list ? sdtp->list->entry_count : 0;
    /* Chunk tables */
    snapshot->stsc_entry_count = stbl->stsc->list ? stbl->stsc->list->entry_count : 0;
    snapshot->stco_entry_count = stbl->stco->list ? stbl->stco->list->entry_count : 0;
    /* Random access point grouping */
    isom_sbgp_t *rap_sbgp = isom_get_sample_to_group         ( stbl, ISOM_GROUP_TYPE_RAP );
    isom_sgpd_t *rap_sgpd = isom_get_sample_group_description( stbl, ISOM_GROUP_TYPE_RAP );
    snapshot->rap_sbgp_entry_count = 0;
    snapshot->rap_sgpd_entry_count = 0;
    if( LSMASH_IS_EXISTING_BOX( rap_sbgp ) && rap_sbgp->list )
    {
        snapshot->rap_sbgp_entry_count = rap_sbgp->list->entry_count;
        lsmash_entry_t *tail = rap_sbgp->list->tail;
        if( tail && tail->data )
            snapshot->rap_sbgp_tail[0] = *(isom_group_assignment_entry_t *)tail->data;
        if( tail && tail->prev && tail->prev->data )
            snapshot->rap_sbgp_tail[1] = *(isom_group_assignment_entry_t *)tail->prev->data;
    }
    if( LSMASH_IS_EXISTING_BOX( rap_sgpd ) && rap_sgpd->list )
    {
        snapshot->rap_sgpd_entry_count = rap_sgpd->list->entry_count;
        if( rap_sgpd->list->tail && rap_sgpd->list->tail->data )
            snapshot->rap_sgpd_tail = *(isom_rap_entry_t *)rap_sgpd->list->tail->data;
    }
    snapshot->rap_cache_existed = cache->rap != NULL;
    if( cache->rap )
    {
        snapshot->rap_is_prev_rap        = cache->rap->is_prev_rap;
        snapshot->rap_random_access_open = cache->rap->random_access != NULL;
    }
    /* Roll recovery grouping */
    isom_sbgp_t *roll_sbgp = isom_get_roll_recovery_sample_to_group         ( &stbl->sbgp_list );
    isom_sgpd_t *roll_sgpd = isom_get_roll_recovery_sample_group_description( &stbl->sgpd_list );
    lsmash_entry_list_t *roll_pool  = cache->roll.pool;
    uint32_t             pool_count = roll_pool ? roll_pool->entry_count : 0;
    snapshot->roll_pool_existed = roll_pool != NULL;
    snapshot->roll_pool_count   = pool_count;
    snapshot->roll_sbgp_entry_count = 0;
    snapshot->roll_sgpd_entry_count = 0;
    if( LSMASH_IS_EXISTING_BOX( roll_sbgp ) && roll_sbgp->list )
    {
        snapshot->roll_sbgp_entry_count = roll_sbgp->list->entry_count;
        /* Only the tail region covering the pooled groups plus the entry
         * right before them can be modified until the next snapshot. */
        uint32_t keep = LSMASH_MIN( snapshot->roll_sbgp_entry_count, pool_count + 1 );
        if( keep )
        {
            snapshot->roll_sbgp_entries = lsmash_malloc( keep * sizeof(isom_group_assignment_entry_t) );
            if( !snapshot->roll_sbgp_entries )
                return LSMASH_ERR_MEMORY_ALLOC;
            snapshot->roll_sbgp_keep = keep;
            for( uint32_t i = 0; i < keep; i++ )
            {
                isom_group_assignment_entry_t *data
                    = (isom_group_assignment_entry_t *)lsmash_list_get_entry_data( roll_sbgp->list,
                                                                                   snapshot->roll_sbgp_entry_count - keep + 1 + i );
                snapshot->roll_sbgp_entries[i] = data ? *data : (isom_group_assignment_entry_t){ 0, 0 };
            }
        }
    }
    if( LSMASH_IS_EXISTING_BOX( roll_sgpd ) && roll_sgpd->list )
        snapshot->roll_sgpd_entry_count = roll_sgpd->list->entry_count;
    if( pool_count )
    {
        snapshot->roll_groups = lsmash_malloc( pool_count * sizeof(isom_roll_group_t) );
        if( !snapshot->roll_groups )
            return LSMASH_ERR_MEMORY_ALLOC;
        snapshot->roll_sgpd_refs = lsmash_malloc( pool_count * sizeof(*snapshot->roll_sgpd_refs) );
        if( !snapshot->roll_sgpd_refs )
            return LSMASH_ERR_MEMORY_ALLOC;
        uint32_t ref_count = 0;
        uint32_t i = 0;
        for( lsmash_entry_t *entry = roll_pool->head; entry; entry = entry->next, i++ )
        {
            isom_roll_group_t *group = (isom_roll_group_t *)entry->data;
            if( !group )
                return LSMASH_ERR_INVALID_DATA;
            snapshot->roll_groups[i] = *group;
            uint32_t index = group->assignment ? group->assignment->group_description_index : 0;
            if( !index )
                continue;
            /* Descriptions referenced by pooled groups may be updated in
             * place later; keep a copy of each distinct referenced entry. */
            uint32_t j = 0;
            while( j < ref_count && snapshot->roll_sgpd_refs[j].index != index )
                ++j;
            if( j < ref_count )
                continue;
            isom_roll_entry_t *description
                = (isom_roll_entry_t *)lsmash_list_get_entry_data( roll_sgpd->list, index );
            if( !description )
                continue;
            snapshot->roll_sgpd_refs[ref_count].index = index;
            snapshot->roll_sgpd_refs[ref_count].entry = *description;
            ++ref_count;
        }
        snapshot->roll_sgpd_ref_count = ref_count;
    }
    /* Cached timestamps and chunk state */
    snapshot->dts                           = cache->timestamp.dts;
    snapshot->cts                           = cache->timestamp.cts;
    snapshot->ctd_shift                     = cache->timestamp.ctd_shift;
    snapshot->all_sync                      = cache->all_sync;
    snapshot->chunk_number                  = cache->chunk.chunk_number;
    snapshot->chunk_sample_description_index = cache->chunk.sample_description_index;
    snapshot->chunk_first_dts               = cache->chunk.first_dts;
    return 0;
}

static int snapshot_restore( isom_trak_t *trak, pool_undo_snapshot_t *snapshot )
{
    isom_cache_t *cache = trak->cache;
    isom_stbl_t  *stbl  = trak->mdia->minf->stbl;
    /* Sample Size Box */
    isom_stsz_t *stsz = stbl->stsz;
    if( LSMASH_IS_EXISTING_BOX( stsz ) )
    {
        if( snapshot->stsz_had_list )
        {
            if( !stsz->list )
                return LSMASH_ERR_NAMELESS;
            truncate_list( stsz->list, snapshot->stsz_entry_count );
        }
        else if( stsz->list )
        {
            /* The list was created by the samples being revoked. */
            lsmash_list_destroy( stsz->list );
            stsz->list = NULL;
        }
        stsz->sample_count = snapshot->stsz_sample_count;
        stsz->sample_size  = snapshot->stsz_sample_size;
    }
    /* Decoding Time to Sample Box */
    if( stbl->stts->list )
    {
        truncate_list( stbl->stts->list, snapshot->stts_entry_count );
        if( snapshot->stts_entry_count )
            *(isom_stts_entry_t *)stbl->stts->list->tail->data = snapshot->stts_tail;
    }
    /* Composition Time to Sample Box */
    if( snapshot->ctts_existed )
    {
        isom_ctts_t *ctts = stbl->ctts;
        if( LSMASH_IS_NON_EXISTING_BOX( ctts ) || !ctts->list )
            return LSMASH_ERR_NAMELESS;
        truncate_list( ctts->list, snapshot->ctts_entry_count );
        if( snapshot->ctts_entry_count )
            *(isom_ctts_entry_t *)ctts->list->tail->data = snapshot->ctts_tail;
        ctts->version = snapshot->ctts_version;
    }
    else if( LSMASH_IS_EXISTING_BOX( stbl->ctts ) )
        isom_remove_box_by_itself( stbl->ctts );
    /* Sync Sample Box */
    if( snapshot->stss_existed )
    {
        if( LSMASH_IS_NON_EXISTING_BOX( stbl->stss ) || !stbl->stss->list )
            return LSMASH_ERR_NAMELESS;
        truncate_list( stbl->stss->list, snapshot->stss_entry_count );
    }
    else if( LSMASH_IS_EXISTING_BOX( stbl->stss ) )
        isom_remove_box_by_itself( stbl->stss );
    /* Partial Sync Sample Box */
    if( snapshot->stps_existed )
    {
        if( LSMASH_IS_NON_EXISTING_BOX( stbl->stps ) || !stbl->stps->list )
            return LSMASH_ERR_NAMELESS;
        truncate_list( stbl->stps->list, snapshot->stps_entry_count );
    }
    else if( LSMASH_IS_EXISTING_BOX( stbl->stps ) )
        isom_remove_box_by_itself( stbl->stps );
    /* Independent and Disposable Samples Box */
    if( snapshot->sdtp_existed )
    {
        if( LSMASH_IS_NON_EXISTING_BOX( stbl->sdtp ) || !stbl->sdtp->list )
            return LSMASH_ERR_NAMELESS;
        truncate_list( stbl->sdtp->list, snapshot->sdtp_entry_count );
    }
    else if( LSMASH_IS_EXISTING_BOX( stbl->sdtp ) )
        isom_remove_box_by_itself( stbl->sdtp );
    /* Chunk tables: entries of chunks already written out are kept since
     * the counts were taken after the commit that started this generation. */
    if( stbl->stsc->list )
        truncate_list( stbl->stsc->list, snapshot->stsc_entry_count );
    if( stbl->stco->list )
        truncate_list( stbl->stco->list, snapshot->stco_entry_count );
    /* Random access point grouping */
    isom_sbgp_t *rap_sbgp = isom_get_sample_to_group         ( stbl, ISOM_GROUP_TYPE_RAP );
    isom_sgpd_t *rap_sgpd = isom_get_sample_group_description( stbl, ISOM_GROUP_TYPE_RAP );
    if( LSMASH_IS_EXISTING_BOX( rap_sbgp ) && rap_sbgp->list )
    {
        truncate_list( rap_sbgp->list, snapshot->rap_sbgp_entry_count );
        lsmash_entry_t *tail = rap_sbgp->list->tail;
        if( snapshot->rap_sbgp_entry_count >= 1 && tail && tail->data )
            *(isom_group_assignment_entry_t *)tail->data = snapshot->rap_sbgp_tail[0];
        if( snapshot->rap_sbgp_entry_count >= 2 && tail && tail->prev && tail->prev->data )
            *(isom_group_assignment_entry_t *)tail->prev->data = snapshot->rap_sbgp_tail[1];
    }
    if( LSMASH_IS_EXISTING_BOX( rap_sgpd ) && rap_sgpd->list )
    {
        truncate_list( rap_sgpd->list, snapshot->rap_sgpd_entry_count );
        if( snapshot->rap_sgpd_entry_count >= 1 && rap_sgpd->list->tail && rap_sgpd->list->tail->data )
            *(isom_rap_entry_t *)rap_sgpd->list->tail->data = snapshot->rap_sgpd_tail;
    }
    if( snapshot->rap_cache_existed )
    {
        if( !cache->rap )
        {
            cache->rap = lsmash_malloc_zero( sizeof(isom_rap_group_t) );
            if( !cache->rap )
                return LSMASH_ERR_MEMORY_ALLOC;
        }
        cache->rap->is_prev_rap    = snapshot->rap_is_prev_rap;
        cache->rap->assignment     = NULL;
        cache->rap->prev_assignment = NULL;
        if( LSMASH_IS_EXISTING_BOX( rap_sbgp ) && rap_sbgp->list && rap_sbgp->list->tail )
        {
            cache->rap->assignment = (isom_group_assignment_entry_t *)rap_sbgp->list->tail->data;
            if( rap_sbgp->list->tail->prev )
                cache->rap->prev_assignment = (isom_group_assignment_entry_t *)rap_sbgp->list->tail->prev->data;
        }
        cache->rap->random_access = NULL;
        if( snapshot->rap_random_access_open
         && LSMASH_IS_EXISTING_BOX( rap_sgpd ) && rap_sgpd->list && rap_sgpd->list->tail )
            cache->rap->random_access = (isom_rap_entry_t *)rap_sgpd->list->tail->data;
    }
    else
        lsmash_freep( &cache->rap );
    /* Roll recovery grouping */
    isom_sbgp_t *roll_sbgp = isom_get_roll_recovery_sample_to_group         ( &stbl->sbgp_list );
    isom_sgpd_t *roll_sgpd = isom_get_roll_recovery_sample_group_description( &stbl->sgpd_list );
    if( LSMASH_IS_EXISTING_BOX( roll_sbgp ) && roll_sbgp->list )
    {
        /* Rebuild the variable tail region from the copies. */
        truncate_list( roll_sbgp->list, snapshot->roll_sbgp_entry_count - snapshot->roll_sbgp_keep );
        for( uint32_t i = 0; i < snapshot->roll_sbgp_keep; i++ )
        {
            isom_group_assignment_entry_t *data
                = (isom_group_assignment_entry_t *)lsmash_memdup( &snapshot->roll_sbgp_entries[i],
                                                                  sizeof(isom_group_assignment_entry_t) );
            if( !data )
                return LSMASH_ERR_MEMORY_ALLOC;
            if( lsmash_list_add_entry( roll_sbgp->list, data ) < 0 )
            {
                lsmash_free( data );
                return LSMASH_ERR_MEMORY_ALLOC;
            }
        }
    }
    if( LSMASH_IS_EXISTING_BOX( roll_sgpd ) && roll_sgpd->list )
    {
        truncate_list( roll_sgpd->list, snapshot->roll_sgpd_entry_count );
        for( uint32_t i = 0; i < snapshot->roll_sgpd_ref_count; i++ )
        {
            isom_roll_entry_t *data
                = (isom_roll_entry_t *)lsmash_list_get_entry_data( roll_sgpd->list, snapshot->roll_sgpd_refs[i].index );
            if( data )
                *data = snapshot->roll_sgpd_refs[i].entry;
        }
    }
    if( snapshot->roll_pool_existed )
    {
        if( !cache->roll.pool )
        {
            cache->roll.pool = lsmash_list_create_simple();
            if( !cache->roll.pool )
                return LSMASH_ERR_MEMORY_ALLOC;
        }
        else
            lsmash_list_remove_entries( cache->roll.pool );
        for( uint32_t i = 0; i < snapshot->roll_pool_count; i++ )
        {
            isom_roll_group_t *group = (isom_roll_group_t *)lsmash_memdup( &snapshot->roll_groups[i],
                                                                           sizeof(isom_roll_group_t) );
            if( !group )
                return LSMASH_ERR_MEMORY_ALLOC;
            /* Pooled groups correspond to the tail entries of the Sample to
             * Group Box in order; relink them to the rebuilt entries. */
            group->assignment
                = LSMASH_IS_EXISTING_BOX( roll_sbgp ) && roll_sbgp->list
                ? (isom_group_assignment_entry_t *)lsmash_list_get_entry_data( roll_sbgp->list,
                                                                               snapshot->roll_sbgp_entry_count
                                                                               - snapshot->roll_pool_count + 1 + i )
                : NULL;
            group->sgpd = roll_sgpd;
            if( lsmash_list_add_entry( cache->roll.pool, group ) < 0 )
            {
                lsmash_free( group );
                return LSMASH_ERR_MEMORY_ALLOC;
            }
        }
    }
    else if( cache->roll.pool )
    {
        lsmash_list_destroy( cache->roll.pool );
        cache->roll.pool = NULL;
    }
    /* Cached timestamps and chunk state */
    cache->timestamp.dts                    = snapshot->dts;
    cache->timestamp.cts                    = snapshot->cts;
    cache->timestamp.ctd_shift              = snapshot->ctd_shift;
    cache->all_sync                         = snapshot->all_sync;
    cache->chunk.chunk_number               = snapshot->chunk_number;
    cache->chunk.sample_description_index   = snapshot->chunk_sample_description_index;
    cache->chunk.first_dts                  = snapshot->chunk_first_dts;
    return 0;
}

int isom_pool_undo_append_prologue( isom_trak_t *trak )
{
    isom_pool_undo_t *undo = trak->cache->pool_undo;
    isom_chunk_t     *chunk = &trak->cache->chunk;
    if( !chunk->pool || chunk->pool->sample_count == 0 )
    {
        /* This sample starts a new pool generation. The restore point is
         * the current state, before this sample touches any table. */
        int err = snapshot_capture( trak, &undo->gen );
        if( err < 0 )
            return err;
        undo->armed = 1;
    }
    else
        /* Remember the state as of this append. If this sample commits the
         * current pool, it becomes the first revocable sample of the next
         * generation and this state turns into its restore point. */
        return snapshot_capture( trak, &undo->last );
    return 0;
}

void isom_pool_undo_self_committed( isom_trak_t *trak )
{
    isom_pool_undo_t *undo = trak->cache->pool_undo;
    if( !undo )
        return;
    /* The sample that triggered this commit belongs to the next generation
     * whose restore point is the state as of this append, except that the
     * chunk table entries written by the commit must be kept. */
    snapshot_release( &undo->gen );
    undo->gen = undo->last;
    memset( &undo->last, 0, sizeof(pool_undo_snapshot_t) );
    isom_stbl_t *stbl = trak->mdia->minf->stbl;
    undo->gen.stsc_entry_count = stbl->stsc->list ? stbl->stsc->list->entry_count : 0;
    undo->gen.stco_entry_count = stbl->stco->list ? stbl->stco->list->entry_count : 0;
    undo->armed = 1;
}

void isom_pool_undo_track_committed( isom_trak_t *trak )
{
    isom_pool_undo_t *undo = trak->cache->pool_undo;
    if( undo )
        /* Everything pooled so far has been written out; there is nothing
         * revocable until the next sample starts a new generation. */
        undo->armed = 0;
}

uint64_t isom_pool_undo_footprint( isom_trak_t *trak )
{
    if( LSMASH_IS_NON_EXISTING_BOX( trak ) || !trak->cache || !trak->cache->pool_undo )
        return 0;
    isom_pool_undo_t *undo = trak->cache->pool_undo;
    uint64_t bytes = sizeof(isom_pool_undo_t);
    for( int i = 0; i < 2; i++ )
    {
        pool_undo_snapshot_t *snapshot = i ? &undo->last : &undo->gen;
        bytes += snapshot->roll_sbgp_keep      * sizeof(isom_group_assignment_entry_t)
               + snapshot->roll_sgpd_ref_count * sizeof(*snapshot->roll_sgpd_refs)
               + snapshot->roll_pool_count     * sizeof(isom_roll_group_t);
    }
    return bytes;
}

int lsmash_set_pool_undo( lsmash_root_t *root, uint32_t track_ID, int enabled )
{
    if( isom_check_initializer_present( root ) < 0
     || track_ID == 0
     || (enabled != 0 && enabled != 1) )
        return LSMASH_ERR_FUNCTION_PARAM;
    lsmash_file_t *file = root->file;
    if( file->fragment
     || (file->flags & LSMASH_FILE_MODE_FRAGMENTED)
     || file != file->initializer )
        return LSMASH_ERR_INVALID_DATA;
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( LSMASH_IS_NON_EXISTING_BOX( trak )
     || LSMASH_IS_NON_EXISTING_BOX( trak->mdia )
     || LSMASH_IS_NON_EXISTING_BOX( trak->mdia->minf )
     || LSMASH_IS_NON_EXISTING_BOX( trak->mdia->minf->stbl )
     || LSMASH_IS_NON_EXISTING_BOX( trak->mdia->minf->stbl->stsd )
     || !trak->cache )
        return LSMASH_ERR_NAMELESS;
    isom_stbl_t *stbl = trak->mdia->minf->stbl;
    /* Sample descriptions must be configured beforehand. */
    if( stbl->stsd->list.entry_count == 0 )
        return LSMASH_ERR_INVALID_DATA;
    /* The mode cannot be switched once the track accepted any sample. */
    if( isom_get_sample_count( trak ) != 0
     || (trak->cache->chunk.pool && trak->cache->chunk.pool->sample_count != 0) )
        return LSMASH_ERR_INVALID_DATA;
    if( !enabled )
    {
        isom_pool_undo_remove( trak->cache->pool_undo );
        trak->cache->pool_undo = NULL;
        return 0;
    }
    /* The undo mode supports only avc1/mp4a sample descriptions. */
    for( lsmash_entry_t *entry = stbl->stsd->list.head; entry; entry = entry->next )
    {
        isom_sample_entry_t *sample_entry = (isom_sample_entry_t *)entry->data;
        if( LSMASH_IS_NON_EXISTING_BOX( sample_entry )
         || (!lsmash_check_codec_type_identical( sample_entry->type, ISOM_CODEC_TYPE_AVC1_VIDEO )
          && !lsmash_check_codec_type_identical( sample_entry->type, ISOM_CODEC_TYPE_MP4A_AUDIO )) )
            return LSMASH_ERR_INVALID_DATA;
    }
    /* The media data must be self-contained in the same file. */
    if( LSMASH_IS_EXISTING_BOX( trak->mdia->minf->dinf )
     && LSMASH_IS_EXISTING_BOX( trak->mdia->minf->dinf->dref ) )
        for( lsmash_entry_t *entry = trak->mdia->minf->dinf->dref->list.head; entry; entry = entry->next )
        {
            isom_dref_entry_t *dref_entry = (isom_dref_entry_t *)entry->data;
            if( LSMASH_IS_NON_EXISTING_BOX( dref_entry ) || !(dref_entry->flags & 0x000001) )
                return LSMASH_ERR_INVALID_DATA;
        }
    if( !trak->cache->pool_undo )
    {
        trak->cache->pool_undo = lsmash_malloc_zero( sizeof(isom_pool_undo_t) );
        if( !trak->cache->pool_undo )
            return LSMASH_ERR_MEMORY_ALLOC;
    }
    return 0;
}

int lsmash_discard_pooled_samples( lsmash_root_t *root, uint32_t track_ID, uint32_t *discarded )
{
    if( isom_check_initializer_present( root ) < 0
     || track_ID == 0
     || !discarded )
        return LSMASH_ERR_FUNCTION_PARAM;
    lsmash_file_t *file = root->file;
    if( file != file->initializer )
        return LSMASH_ERR_INVALID_DATA;
    isom_trak_t *trak = isom_get_trak( file, track_ID );
    if( LSMASH_IS_NON_EXISTING_BOX( trak )
     || !trak->cache
     || !trak->cache->pool_undo )
        return LSMASH_ERR_NAMELESS;
    isom_pool_undo_t   *undo = trak->cache->pool_undo;
    isom_sample_pool_t *pool = trak->cache->chunk.pool;
    if( !pool || pool->sample_count == 0 )
    {
        /* Nothing pooled: success with no sample revoked. */
        *discarded = 0;
        return 0;
    }
    if( !undo->armed )
        return LSMASH_ERR_NAMELESS;
    int err = snapshot_restore( trak, &undo->gen );
    if( err < 0 )
        return err;
    *discarded = pool->sample_count;
    pool->sample_count = 0;
    pool->size         = 0;
    undo->armed = 0;
    return 0;
}
