/*****************************************************************************
 * eit_extended.c
 * Copyright (C) 2026 VideoLAN
 *
 * Authors: libdvbpsi contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#if defined(HAVE_INTTYPES_H)
#include <inttypes.h>
#elif defined(HAVE_STDINT_H)
#include <stdint.h>
#endif

#include "../dvbpsi.h"
#include "../descriptor.h"
#include "../descriptors/dr_4e.h"
#include "eit.h"
#include "eit_extended.h"

/* DVB text encoding selection byte for the ISO/IEC 10646 UTF-8 table. */
#define DVBPSI_TEXT_UTF8               0x15
/* Maximum payload of one descriptor. */
#define DVBPSI_DR_PAYLOAD_MAX          255
/* Fixed payload bytes of an extended event descriptor:
 * descriptor_number/last (1) + language (3) + items_length (1) +
 * text_length (1). */
#define DVBPSI_EE_FIXED_BYTES          6
/* Bytes left for the items loop and the text field. */
#define DVBPSI_EE_CONTENT_BUDGET       (DVBPSI_DR_PAYLOAD_MAX - \
                                        DVBPSI_EE_FIXED_BYTES)
/* A series numbers descriptors 0..15, so 16 descriptors at most. */
#define DVBPSI_EE_MAX_DESCRIPTORS      16
/* Largest legal EIT event: 12 header bytes plus the descriptor loop. */
#define DVBPSI_EIT_EVENT_MAX           4076
#define DVBPSI_EIT_EVENT_HEADER        12

/*****************************************************************************
 * Small UTF-8 helpers
 *****************************************************************************/

/* A lead byte starts a new code point: ASCII or a 2/3/4 byte lead.
 * Continuation bytes are 0x80..0xBF. */
static bool dvbpsi_utf8_is_lead(uint8_t b)
{
    return b < 0x80 || b >= 0xC0;
}

/* Strict UTF-8 validation: rejects overlong encodings, surrogates and code
 * points above U+10FFFF as well as stray continuation bytes. */
static bool dvbpsi_utf8_is_valid(const uint8_t *p, int i_length)
{
    int i = 0;

    if (p == NULL && i_length != 0)
        return false;

    while (i < i_length)
    {
        uint8_t b = p[i];
        int need;
        uint8_t min_next, max_next;

        if (b < 0x80)
        {
            i++;
            continue;
        }
        if (b < 0xC2)
            return false;       /* continuation or overlong 2 byte lead */
        if (b < 0xE0)
        {
            need = 1;
            min_next = 0x80;
            max_next = 0xBF;
        }
        else if (b == 0xE0)
        {
            need = 2;
            min_next = 0xA0;   /* no overlong encoding */
            max_next = 0xBF;
        }
        else if (b < 0xED)
        {
            need = 2;
            min_next = 0x80;
            max_next = 0xBF;
        }
        else if (b == 0xED)
        {
            need = 2;
            min_next = 0x80;
            max_next = 0x9F;   /* no UTF-16 surrogates */
        }
        else if (b < 0xF0)
        {
            need = 2;
            min_next = 0x80;
            max_next = 0xBF;
        }
        else if (b == 0xF0)
        {
            need = 3;
            min_next = 0x90;   /* no overlong encoding */
            max_next = 0xBF;
        }
        else if (b < 0xF4)
        {
            need = 3;
            min_next = 0x80;
            max_next = 0xBF;
        }
        else if (b == 0xF4)
        {
            need = 3;
            min_next = 0x80;
            max_next = 0x8F;   /* code points above U+10FFFF */
        }
        else
            return false;

        if (i + need >= i_length + 1)
            return false;

        for (int j = 0; j < need; j++)
        {
            uint8_t c = p[i + 1 + j];
            if (j == 0)
            {
                if (c < min_next || c > max_next)
                    return false;
            }
            else if (c < 0x80 || c > 0xBF)
                return false;
        }
        i += 1 + need;
    }

    return true;
}

/*****************************************************************************
 * Encoded text field: 0x15 followed by the raw UTF-8 bytes, or empty.
 *****************************************************************************/
typedef struct
{
    uint8_t *p_data;
    int      i_length;
} dvbpsi_text_blob_t;

static bool dvbpsi_text_blob_init(dvbpsi_text_blob_t *p_blob,
                                  const uint8_t *p_text, int i_length)
{
    p_blob->i_length = 0;
    p_blob->p_data = NULL;

    if (i_length == 0)
        return true;

    p_blob->p_data = malloc((size_t)i_length + 1);
    if (!p_blob->p_data)
        return false;
    p_blob->p_data[0] = DVBPSI_TEXT_UTF8;
    memcpy(&p_blob->p_data[1], p_text, i_length);
    p_blob->i_length = i_length + 1;
    return true;
}

static void dvbpsi_text_blob_clean(dvbpsi_text_blob_t *p_blob)
{
    free(p_blob->p_data);
    p_blob->p_data = NULL;
    p_blob->i_length = 0;
}

/*****************************************************************************
 * One descriptor while packing: its items loop bytes, its text bytes and
 * the ordered item pairs that produced the items loop.
 *****************************************************************************/
#define DVBPSI_EE_MAX_ITEMS 126

typedef struct
{
    uint8_t  i_items[DVBPSI_DR_PAYLOAD_MAX];
    int      i_items_length;
    uint8_t  i_text[DVBPSI_DR_PAYLOAD_MAX];
    int      i_text_length;

    int      i_entry_count;
    const dvbpsi_text_blob_t *p_description[DVBPSI_EE_MAX_ITEMS];
    const dvbpsi_text_blob_t *p_item[DVBPSI_EE_MAX_ITEMS];
} dvbpsi_ee_builder_t;

static void dvbpsi_ee_builder_init(dvbpsi_ee_builder_t *p_builder)
{
    p_builder->i_items_length = 0;
    p_builder->i_text_length = 0;
    p_builder->i_entry_count = 0;
}

static int dvbpsi_ee_builder_used(const dvbpsi_ee_builder_t *p_builder)
{
    return p_builder->i_items_length + p_builder->i_text_length;
}

/*****************************************************************************
 * dvbpsi_extended_text_delete
 *****************************************************************************/
void dvbpsi_extended_text_delete(dvbpsi_extended_text_t *p_result)
{
    if (!p_result)
        return;

    if (p_result->p_items)
    {
        for (int i = 0; i < p_result->i_item_count; i++)
        {
            free(p_result->p_items[i].p_name);
            free(p_result->p_items[i].p_value);
        }
        free(p_result->p_items);
    }
    free(p_result->p_text);
    free(p_result);
}

/*****************************************************************************
 * dvbpsi_eit_event_set_extended_text
 *****************************************************************************/
bool dvbpsi_eit_event_set_extended_text(dvbpsi_eit_event_t *p_event,
                       const uint8_t i_iso_639_code[3],
                       const uint8_t *p_text, int i_text_length,
                       const dvbpsi_extended_text_item_t *p_items,
                       int i_item_count,
                       dvbpsi_eit_extended_error_t *p_error)
{
    dvbpsi_text_blob_t *p_name_blobs = NULL;
    dvbpsi_text_blob_t *p_value_blobs = NULL;
    dvbpsi_ee_builder_t *p_builders = NULL;
    int i_builder_count = 0;
    int i_builder_capacity = 0;
    dvbpsi_descriptor_t **pp_new = NULL;
    int i_new_count = 0;
    bool b_result = false;

    if (p_error)
        *p_error = DVBPSI_EIT_EXTENDED_OK;

    if (!p_event || !i_iso_639_code || i_text_length < 0 ||
        i_item_count < 0 || (i_item_count > 0 && !p_items) ||
        (i_text_length > 0 && !p_text))
    {
        if (p_error)
            *p_error = DVBPSI_EIT_EXTENDED_ERR_ARGUMENT;
        return false;
    }

    for (int i = 0; i < i_item_count; i++)
    {
        if (p_items[i].i_name_length < 0 || p_items[i].i_value_length < 0 ||
            (p_items[i].i_name_length > 0 && !p_items[i].p_name) ||
            (p_items[i].i_value_length > 0 && !p_items[i].p_value))
        {
            if (p_error)
                *p_error = DVBPSI_EIT_EXTENDED_ERR_ARGUMENT;
            return false;
        }
    }

    /* Every input string must be valid UTF-8. */
    if (!dvbpsi_utf8_is_valid(p_text, i_text_length))
    {
        if (p_error)
            *p_error = DVBPSI_EIT_EXTENDED_ERR_UTF8;
        return false;
    }
    for (int i = 0; i < i_item_count; i++)
    {
        if (!dvbpsi_utf8_is_valid(p_items[i].p_name,
                                  p_items[i].i_name_length) ||
            !dvbpsi_utf8_is_valid(p_items[i].p_value,
                                  p_items[i].i_value_length))
        {
            if (p_error)
                *p_error = DVBPSI_EIT_EXTENDED_ERR_UTF8;
            return false;
        }
    }

    /* Encode the item fields once; each non-empty field gets its own 0x15. */
    p_name_blobs = calloc((size_t)i_item_count, sizeof(*p_name_blobs));
    p_value_blobs = calloc((size_t)i_item_count, sizeof(*p_value_blobs));
    if ((i_item_count > 0 && (!p_name_blobs || !p_value_blobs)))
    {
        if (p_error)
            *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
        goto out;
    }
    for (int i = 0; i < i_item_count; i++)
    {
        if (!dvbpsi_text_blob_init(&p_name_blobs[i], p_items[i].p_name,
                                   p_items[i].i_name_length) ||
            !dvbpsi_text_blob_init(&p_value_blobs[i], p_items[i].p_value,
                                   p_items[i].i_value_length))
        {
            if (p_error)
                *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
            goto out;
        }
    }

    /* Pack the items. A pair always lands in a single descriptor. */
    for (int i = 0; i < i_item_count; i++)
    {
        int i_pair_length = 2 + p_name_blobs[i].i_length +
                            p_value_blobs[i].i_length;

        /* Even an empty descriptor cannot hold this pair. */
        if (i_pair_length > DVBPSI_EE_CONTENT_BUDGET)
        {
            if (p_error)
                *p_error = DVBPSI_EIT_EXTENDED_ERR_ITEM_TOO_LARGE;
            goto out;
        }

        if (i_builder_count == 0 ||
            dvbpsi_ee_builder_used(&p_builders[i_builder_count - 1]) +
                i_pair_length > DVBPSI_EE_CONTENT_BUDGET)
        {
            if (i_builder_count == i_builder_capacity)
            {
                int i_new_capacity = i_builder_capacity ?
                                     i_builder_capacity * 2 : 4;
                dvbpsi_ee_builder_t *p_tmp = realloc(p_builders,
                        (size_t)i_new_capacity * sizeof(*p_builders));
                if (!p_tmp)
                {
                    if (p_error)
                        *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
                    goto out;
                }
                p_builders = p_tmp;
                i_builder_capacity = i_new_capacity;
            }
            dvbpsi_ee_builder_init(&p_builders[i_builder_count]);
            i_builder_count++;
        }

        dvbpsi_ee_builder_t *p_builder = &p_builders[i_builder_count - 1];
        if (p_builder->i_entry_count >= DVBPSI_EE_MAX_ITEMS)
        {
            if (p_error)
                *p_error = DVBPSI_EIT_EXTENDED_ERR_TOO_MANY_DESCRIPTORS;
            goto out;
        }

        uint8_t *p = &p_builder->i_items[p_builder->i_items_length];
        p[0] = (uint8_t)p_name_blobs[i].i_length;
        if (p_name_blobs[i].i_length > 0)
            memcpy(&p[1], p_name_blobs[i].p_data,
                   p_name_blobs[i].i_length);
        p += 1 + p_name_blobs[i].i_length;

        p[0] = (uint8_t)p_value_blobs[i].i_length;
        if (p_value_blobs[i].i_length > 0)
            memcpy(&p[1], p_value_blobs[i].p_data,
                   p_value_blobs[i].i_length);

        p_builder->i_items_length += i_pair_length;
        p_builder->p_description[p_builder->i_entry_count] =
                &p_name_blobs[i];
        p_builder->p_item[p_builder->i_entry_count] = &p_value_blobs[i];
        p_builder->i_entry_count++;
    }

    /* Split the description on UTF-8 code point boundaries. */
    {
        int i_pos = 0;

        while (i_pos < i_text_length)
        {
            dvbpsi_ee_builder_t *p_builder;
            int i_room;
            int i_take;

            if (i_builder_count == 0)
            {
                dvbpsi_ee_builder_t *p_tmp = realloc(p_builders,
                        sizeof(*p_builders));
                if (!p_tmp)
                {
                    if (p_error)
                        *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
                    goto out;
                }
                p_builders = p_tmp;
                i_builder_capacity = 1;
                dvbpsi_ee_builder_init(&p_builders[0]);
                i_builder_count = 1;
            }
            p_builder = &p_builders[i_builder_count - 1];

            /* The 0x15 selection byte needs one byte of the room. */
            i_room = DVBPSI_EE_CONTENT_BUDGET -
                     dvbpsi_ee_builder_used(p_builder);
            if (i_room < 2)
            {
                /* Current descriptor is full: start another one. */
                dvbpsi_ee_builder_t *p_tmp = realloc(p_builders,
                        (size_t)(i_builder_count + 1) * sizeof(*p_builders));
                if (!p_tmp)
                {
                    if (p_error)
                        *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
                    goto out;
                }
                p_builders = p_tmp;
                i_builder_capacity = i_builder_count + 1;
                dvbpsi_ee_builder_init(&p_builders[i_builder_count]);
                i_builder_count++;
                continue;
            }

            i_take = i_text_length - i_pos;
            if (i_take > i_room - 1)
                i_take = i_room - 1;

            /* Never cut inside a multi-byte sequence. When the fragment
             * reaches the end of the string it is already on a boundary
             * because the whole string was validated as UTF-8. */
            while (i_take > 0 && i_pos + i_take < i_text_length &&
                   !dvbpsi_utf8_is_lead(p_text[i_pos + i_take]))
                i_take--;

            if (i_take == 0)
            {
                /* The remaining code point does not fit: use a new
                 * descriptor, which always has room for at least one code
                 * point (sequences are at most 4 bytes). */
                dvbpsi_ee_builder_t *p_tmp = realloc(p_builders,
                        (size_t)(i_builder_count + 1) * sizeof(*p_builders));
                if (!p_tmp)
                {
                    if (p_error)
                        *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
                    goto out;
                }
                p_builders = p_tmp;
                i_builder_capacity = i_builder_count + 1;
                dvbpsi_ee_builder_init(&p_builders[i_builder_count]);
                i_builder_count++;
                continue;
            }

            p_builder->i_text[p_builder->i_text_length] = DVBPSI_TEXT_UTF8;
            memcpy(&p_builder->i_text[p_builder->i_text_length + 1],
                   &p_text[i_pos], i_take);
            p_builder->i_text_length += 1 + i_take;
            i_pos += i_take;
        }
    }

    /* Empty input produces no descriptor at all. */
    if (i_builder_count == 0)
    {
        /* Fall through to the replacement step with an empty new series. */
    }
    else if (i_builder_count > DVBPSI_EE_MAX_DESCRIPTORS)
    {
        if (p_error)
            *p_error = DVBPSI_EIT_EXTENDED_ERR_TOO_MANY_DESCRIPTORS;
        goto out;
    }

    /* Turn the builders into real descriptors through the low level
     * generator, with contiguous numbers from zero. */
    pp_new = calloc((size_t)i_builder_count + 1, sizeof(*pp_new));
    if (!pp_new && i_builder_count > 0)
    {
        if (p_error)
            *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
        goto out;
    }
    for (int i = 0; i < i_builder_count; i++)
    {
        dvbpsi_extended_event_dr_t dr;
        memset(&dr, 0, sizeof(dr));
        dr.i_descriptor_number = (uint8_t)i;
        dr.i_last_descriptor_number =
                (uint8_t)(i_builder_count - 1);
        memcpy(dr.i_iso_639_code, i_iso_639_code, 3);
        dr.i_entry_count = p_builders[i].i_entry_count;
        for (int j = 0; j < dr.i_entry_count; j++)
        {
            dr.i_item_description_length[j] =
                    p_builders[i].p_description[j]->i_length;
            dr.i_item_description[j] =
                    p_builders[i].p_description[j]->p_data;
            dr.i_item_length[j] = p_builders[i].p_item[j]->i_length;
            dr.i_item[j] = p_builders[i].p_item[j]->p_data;
        }
        dr.i_text_length = p_builders[i].i_text_length;
        dr.i_text = p_builders[i].i_text_length ?
                    p_builders[i].i_text : NULL;

        pp_new[i] = dvbpsi_GenExtendedEventDr(&dr, false);
        if (!pp_new[i])
        {
            if (p_error)
                *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
            goto out;
        }
        i_new_count++;
    }

    /* Measure the event and make sure the replacement still fits into a
     * legal EIT event before touching the descriptor list. */
    {
        int i_event_size = DVBPSI_EIT_EVENT_HEADER;
        int i_old_series_size = 0;
        int i_new_series_size = 0;

        for (dvbpsi_descriptor_t *p = p_event->p_first_descriptor; p;
             p = p->p_next)
        {
            i_event_size += 2 + p->i_length;
            if (p->i_tag == 0x4e && p->i_length >= 4 &&
                memcmp(&p->p_data[1], i_iso_639_code, 3) == 0)
                i_old_series_size += 2 + p->i_length;
        }
        for (int i = 0; i < i_builder_count; i++)
            i_new_series_size += 2 + pp_new[i]->i_length;

        if (i_event_size - i_old_series_size + i_new_series_size >
            DVBPSI_EIT_EVENT_MAX)
        {
            if (p_error)
                *p_error = DVBPSI_EIT_EXTENDED_ERR_EVENT_TOO_LARGE;
            goto out;
        }
    }

    /* Unlink the old series of this language, keeping every other
     * descriptor in its original position. */
    {
        dvbpsi_descriptor_t **pp = &p_event->p_first_descriptor;
        dvbpsi_descriptor_t **pp_insert = NULL;
        dvbpsi_descriptor_t *p_old_chain = NULL;
        int i_old_loop = 0;
        int i_new_loop = 0;

        while (*pp)
        {
            dvbpsi_descriptor_t *p = *pp;
            bool b_match = (p->i_tag == 0x4e && p->i_length >= 4 &&
                            memcmp(&p->p_data[1], i_iso_639_code, 3) == 0);
            if (!b_match)
            {
                pp = &p->p_next;
                continue;
            }

            if (!pp_insert)
                pp_insert = pp;
            i_old_loop += 2 + p->i_length;

            *pp = p->p_next;
            p->p_next = p_old_chain;
            p_old_chain = p;
        }

        if (i_builder_count > 0)
        {
            dvbpsi_descriptor_t *p_chain = NULL;
            for (int i = i_builder_count - 1; i >= 0; i--)
            {
                pp_new[i]->p_next = p_chain;
                p_chain = pp_new[i];
                i_new_loop += 2 + pp_new[i]->i_length;
            }

            if (pp_insert)
            {
                dvbpsi_descriptor_t *p_after = *pp_insert;
                *pp_insert = p_chain;
                /* p_chain ends with the last new descriptor. */
                pp_new[i_builder_count - 1]->p_next = p_after;
            }
            else
            {
                /* No previous series: append after every existing
                 * descriptor, preserving their order. */
                dvbpsi_descriptor_t **pp_tail =
                        &p_event->p_first_descriptor;
                while (*pp_tail)
                    pp_tail = &(*pp_tail)->p_next;
                *pp_tail = p_chain;
            }
            /* The new descriptors now belong to the event. */
            i_new_count = 0;
        }

        /* The old descriptors are no longer reachable from the event. */
        dvbpsi_DeleteDescriptors(p_old_chain);

        p_event->i_descriptors_length =
                (uint16_t)(p_event->i_descriptors_length -
                           i_old_loop + i_new_loop);
    }

    b_result = true;

out:
    if (!b_result)
    {
        /* Only descriptors not yet handed over to the event are freed here. */
        for (int i = 0; i < i_new_count; i++)
            dvbpsi_DeleteDescriptors(pp_new[i]);
    }
    free(pp_new);
    free(p_builders);
    if (p_name_blobs)
    {
        for (int i = 0; i < i_item_count; i++)
            dvbpsi_text_blob_clean(&p_name_blobs[i]);
        free(p_name_blobs);
    }
    if (p_value_blobs)
    {
        for (int i = 0; i < i_item_count; i++)
            dvbpsi_text_blob_clean(&p_value_blobs[i]);
        free(p_value_blobs);
    }
    return b_result;
}

/*****************************************************************************
 * Reader
 *****************************************************************************/

/* Check one encoded text field: empty is legal, otherwise it must use the
 * UTF-8 selection byte. Returns a pointer to the raw bytes (or NULL) and its
 * length through the output arguments. */
static bool dvbpsi_extended_field_decode(const uint8_t *p_field,
                                         int i_field_length,
                                         const uint8_t **pp_raw,
                                         int *pi_raw_length,
                                         dvbpsi_eit_extended_error_t *p_error)
{
    if (i_field_length == 0)
    {
        *pp_raw = NULL;
        *pi_raw_length = 0;
        return true;
    }

    if (p_field[0] != DVBPSI_TEXT_UTF8)
    {
        *p_error = DVBPSI_EIT_EXTENDED_ERR_ENCODING;
        return false;
    }

    *pp_raw = &p_field[1];
    *pi_raw_length = i_field_length - 1;
    return true;
}

static uint8_t *dvbpsi_extended_memdup(const uint8_t *p, int i_length)
{
    if (i_length == 0)
        return NULL;
    uint8_t *p_copy = malloc((size_t)i_length);
    if (p_copy)
        memcpy(p_copy, p, i_length);
    return p_copy;
}

dvbpsi_extended_text_t *dvbpsi_eit_event_get_extended_text(
                       dvbpsi_eit_event_t *p_event,
                       const uint8_t i_iso_639_code[3],
                       dvbpsi_eit_extended_error_t *p_error)
{
    dvbpsi_extended_event_dr_t **pp_decoded = NULL;
    int i_count = 0;
    int i_capacity = 0;
    dvbpsi_extended_text_t *p_result = NULL;

    if (p_error)
        *p_error = DVBPSI_EIT_EXTENDED_OK;

    if (!p_event || !i_iso_639_code)
    {
        if (p_error)
            *p_error = DVBPSI_EIT_EXTENDED_ERR_ARGUMENT;
        return NULL;
    }

    /* Gather the series of the requested language, whatever its physical
     * position in the descriptor list. */
    for (dvbpsi_descriptor_t *p = p_event->p_first_descriptor; p;
         p = p->p_next)
    {
        dvbpsi_extended_event_dr_t *p_decoded;

        if (p->i_tag != 0x4e || p->i_length < 4)
            continue;
        if (memcmp(&p->p_data[1], i_iso_639_code, 3) != 0)
            continue;

        p_decoded = dvbpsi_DecodeExtendedEventDr(p);
        if (!p_decoded)
        {
            if (p_error)
                *p_error = DVBPSI_EIT_EXTENDED_ERR_MALFORMED;
            goto error;
        }

        if (i_count == i_capacity)
        {
            int i_new_capacity = i_capacity ? i_capacity * 2 : 4;
            dvbpsi_extended_event_dr_t **pp_tmp = realloc(pp_decoded,
                    (size_t)i_new_capacity * sizeof(*pp_tmp));
            if (!pp_tmp)
            {
                if (p_error)
                    *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
                goto error;
            }
            pp_decoded = pp_tmp;
            i_capacity = i_new_capacity;
        }
        pp_decoded[i_count++] = p_decoded;
    }

    if (i_count == 0)
    {
        if (p_error)
            *p_error = DVBPSI_EIT_EXTENDED_ERR_NOT_FOUND;
        goto error;
    }

    /* Numbers must be contiguous from zero with no duplicate, and every
     * last_descriptor_number must agree. */
    {
        bool b_seen[DVBPSI_EE_MAX_DESCRIPTORS] = { false };
        int i_last = -1;

        for (int i = 0; i < i_count; i++)
        {
            int i_number = pp_decoded[i]->i_descriptor_number;
            if (i_number >= DVBPSI_EE_MAX_DESCRIPTORS || b_seen[i_number])
            {
                if (p_error)
                    *p_error = DVBPSI_EIT_EXTENDED_ERR_MALFORMED;
                goto error;
            }
            b_seen[i_number] = true;

            if (i_last < 0)
                i_last = pp_decoded[i]->i_last_descriptor_number;
            else if (i_last != pp_decoded[i]->i_last_descriptor_number)
            {
                if (p_error)
                    *p_error = DVBPSI_EIT_EXTENDED_ERR_MALFORMED;
                goto error;
            }
        }

        if (i_last != i_count - 1)
        {
            if (p_error)
                *p_error = DVBPSI_EIT_EXTENDED_ERR_MALFORMED;
            goto error;
        }
        for (int i = 0; i < i_count; i++)
        {
            if (!b_seen[i])
            {
                if (p_error)
                    *p_error = DVBPSI_EIT_EXTENDED_ERR_MALFORMED;
                goto error;
            }
        }
    }

    /* Order the descriptors by descriptor_number. */
    {
        dvbpsi_extended_event_dr_t *p_sorted[DVBPSI_EE_MAX_DESCRIPTORS];
        int i_total_items = 0;
        int i_text_raw_length = 0;

        for (int i = 0; i < i_count; i++)
            p_sorted[pp_decoded[i]->i_descriptor_number] = pp_decoded[i];

        /* First pass: encoding checks and sizes, nothing allocated yet that
         * would need freeing on a late error. */
        for (int i = 0; i < i_count; i++)
        {
            dvbpsi_extended_event_dr_t *p_decoded = p_sorted[i];
            const uint8_t *p_raw;
            int i_raw_length;

            for (int j = 0; j < p_decoded->i_entry_count; j++)
            {
                if (!dvbpsi_extended_field_decode(
                        p_decoded->i_item_description[j],
                        p_decoded->i_item_description_length[j],
                        &p_raw, &i_raw_length, p_error) ||
                    !dvbpsi_extended_field_decode(
                        p_decoded->i_item[j],
                        p_decoded->i_item_length[j],
                        &p_raw, &i_raw_length, p_error))
                    goto error;
            }
            if (!dvbpsi_extended_field_decode(p_decoded->i_text,
                                              p_decoded->i_text_length,
                                              &p_raw, &i_raw_length,
                                              p_error))
                goto error;
            i_text_raw_length += i_raw_length;
            i_total_items += p_decoded->i_entry_count;
        }

        p_result = calloc(1, sizeof(*p_result));
        if (!p_result)
        {
            if (p_error)
                *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
            goto error;
        }
        memcpy(p_result->i_iso_639_code, i_iso_639_code, 3);
        p_result->i_text_length = i_text_raw_length;
        p_result->i_item_count = i_total_items;

        if (i_text_raw_length > 0)
        {
            p_result->p_text = malloc((size_t)i_text_raw_length);
            if (!p_result->p_text)
            {
                if (p_error)
                    *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
                goto error;
            }
        }
        if (i_total_items > 0)
        {
            p_result->p_items = calloc((size_t)i_total_items,
                                       sizeof(*p_result->p_items));
            if (!p_result->p_items)
            {
                if (p_error)
                    *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
                goto error;
            }
        }

        /* Second pass: independent copies, in descriptor number order. */
        {
            int i_text_pos = 0;
            int i_item_index = 0;

            for (int i = 0; i < i_count; i++)
            {
                dvbpsi_extended_event_dr_t *p_decoded = p_sorted[i];
                const uint8_t *p_raw;
                int i_raw_length;

                for (int j = 0; j < p_decoded->i_entry_count; j++)
                {
                    dvbpsi_extended_text_item_result_t *p_item =
                            &p_result->p_items[i_item_index++];

                    dvbpsi_extended_field_decode(
                            p_decoded->i_item_description[j],
                            p_decoded->i_item_description_length[j],
                            &p_raw, &i_raw_length, p_error);
                    if (i_raw_length > 0)
                    {
                        p_item->p_name =
                                dvbpsi_extended_memdup(p_raw, i_raw_length);
                        if (!p_item->p_name)
                        {
                            if (p_error)
                                *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
                            goto error;
                        }
                    }
                    p_item->i_name_length = i_raw_length;

                    dvbpsi_extended_field_decode(
                            p_decoded->i_item[j],
                            p_decoded->i_item_length[j],
                            &p_raw, &i_raw_length, p_error);
                    if (i_raw_length > 0)
                    {
                        p_item->p_value =
                                dvbpsi_extended_memdup(p_raw, i_raw_length);
                        if (!p_item->p_value)
                        {
                            if (p_error)
                                *p_error = DVBPSI_EIT_EXTENDED_ERR_MEMORY;
                            goto error;
                        }
                    }
                    p_item->i_value_length = i_raw_length;
                }

                dvbpsi_extended_field_decode(p_decoded->i_text,
                                             p_decoded->i_text_length,
                                             &p_raw, &i_raw_length, p_error);
                if (i_raw_length > 0)
                {
                    memcpy(&p_result->p_text[i_text_pos], p_raw,
                           i_raw_length);
                    i_text_pos += i_raw_length;
                }
            }
        }

        /* Validate every reassembled UTF-8 string. */
        if (!dvbpsi_utf8_is_valid(p_result->p_text,
                                  p_result->i_text_length))
        {
            if (p_error)
                *p_error = DVBPSI_EIT_EXTENDED_ERR_MALFORMED;
            goto error;
        }
        for (int i = 0; i < p_result->i_item_count; i++)
        {
            if (!dvbpsi_utf8_is_valid(p_result->p_items[i].p_name,
                                      p_result->p_items[i].i_name_length) ||
                !dvbpsi_utf8_is_valid(p_result->p_items[i].p_value,
                                      p_result->p_items[i].i_value_length))
            {
                if (p_error)
                    *p_error = DVBPSI_EIT_EXTENDED_ERR_MALFORMED;
                goto error;
            }
        }
    }

    free(pp_decoded);
    return p_result;

error:
    if (p_result)
        dvbpsi_extended_text_delete(p_result);
    free(pp_decoded);
    return NULL;
}
