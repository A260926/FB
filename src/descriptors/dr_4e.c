/*****************************************************************************
 * dr_4e.c
 * Copyright (C) 2005-2011 VideoLAN
 * $Id: dr_4d.c,v 1.7 2003/07/25 20:20:40 fenrir Exp $
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
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
#include "../dvbpsi_private.h"
#include "../descriptor.h"

#include "dr_4e.h"

/*****************************************************************************
 * dvbpsi_DecodeExtendedEventDr
 *****************************************************************************
 * The descriptor_number/last_descriptor_number nibble, the ISO 639 language
 * code, the items loop and the text field are all bounded by the descriptor
 * payload. Every length field is checked against the payload so a truncated
 * descriptor cannot make the parser read past p_data, and the decoded text
 * and item pointers always point at the internal i_buffer instead of at the
 * descriptor payload (so deleting the descriptor never leaves dangling
 * pointers behind).
 *****************************************************************************/
dvbpsi_extended_event_dr_t * dvbpsi_DecodeExtendedEventDr(dvbpsi_descriptor_t * p_descriptor)
{
    dvbpsi_extended_event_dr_t * p_decoded;
    int i_items_length;
    int i_pos;
    const uint8_t *p;
    const uint8_t *p_items_end;
    const uint8_t *p_payload_end;

    /* Check the tag and the fixed layout:
     * number/last (1) + language (3) + items_length (1) + text_length (1) */
    if (!dvbpsi_CanDecodeAsDescriptor(p_descriptor, 0x4e) ||
            p_descriptor->i_length < 6 )
        return NULL;

    /* Don't decode twice */
    if (dvbpsi_IsDescriptorDecoded(p_descriptor))
        return p_descriptor->p_decoded;

    p_payload_end = &p_descriptor->p_data[p_descriptor->i_length];

    i_items_length = p_descriptor->p_data[4];
    /* The items loop must end exactly where the text_length byte begins. */
    if (5 + i_items_length + 1 > p_descriptor->i_length)
        return NULL;
    p_items_end = &p_descriptor->p_data[5 + i_items_length];

    /* Allocate memory */
    p_decoded = calloc(1, sizeof(dvbpsi_extended_event_dr_t));
    if (!p_decoded)
        return NULL;

    /* Decode */
    p_decoded->i_descriptor_number = (p_descriptor->p_data[0] >> 4)&0xf;
    p_decoded->i_last_descriptor_number = p_descriptor->p_data[0]&0x0f;
    memcpy( &p_decoded->i_iso_639_code[0], &p_descriptor->p_data[1], 3 );
    p_decoded->i_entry_count = 0;
    i_pos = 0;
    for( p = &p_descriptor->p_data[5]; p < p_items_end; )
    {
        int idx;
        int i_description_length;
        int i_item_length;

        /* description length byte */
        i_description_length = p[0];
        p += 1;
        if (p + i_description_length > p_items_end)
            goto error;

        /* item length byte follows the description bytes */
        if (p + i_description_length + 1 > p_items_end)
            goto error;
        i_item_length = p[i_description_length];

        /* the whole item pair must fit inside the items loop */
        if (p + i_description_length + 1 + i_item_length > p_items_end)
            goto error;

        if (p_decoded->i_entry_count >= 126 ||
            i_pos + i_description_length + i_item_length >
                (int)sizeof(p_decoded->i_buffer))
            goto error;

        idx = p_decoded->i_entry_count;
        p_decoded->i_item_description_length[idx] = i_description_length;
        p_decoded->i_item_description[idx] = &p_decoded->i_buffer[i_pos];
        memcpy( &p_decoded->i_buffer[i_pos], p, i_description_length );
        i_pos += i_description_length;
        p += i_description_length + 1;

        p_decoded->i_item_length[idx] = i_item_length;
        p_decoded->i_item[idx] = &p_decoded->i_buffer[i_pos];
        memcpy( &p_decoded->i_buffer[i_pos], p, i_item_length );
        i_pos += i_item_length;
        p += i_item_length;

        p_decoded->i_entry_count++;
    }

    p_decoded->i_text_length = p_items_end[0];
    if (p_items_end + 1 + p_decoded->i_text_length > p_payload_end)
        goto error;
    if (i_pos + p_decoded->i_text_length > (int)sizeof(p_decoded->i_buffer))
        goto error;

    if( p_decoded->i_text_length > 0 )
    {
        memcpy( &p_decoded->i_buffer[i_pos],
                &p_items_end[1], p_decoded->i_text_length );
        p_decoded->i_text = &p_decoded->i_buffer[i_pos];
    }
    else
    {
        p_decoded->i_text = &p_decoded->i_buffer[i_pos];
    }

    p_descriptor->p_decoded = (void*)p_decoded;

    return p_decoded;

error:
    free(p_decoded);
    return NULL;
}


/*****************************************************************************
 * dvbpsi_GenExtendedEventDr
 *****************************************************************************
 * The items loop length is computed from the item pairs only, so an empty
 * item list always yields an items_length of 0. All length fields are
 * validated against the 255 byte payload limit and every pointer is
 * dereferenced before use, so a malformed decoded structure cannot overflow
 * the descriptor payload.
 *****************************************************************************/
dvbpsi_descriptor_t * dvbpsi_GenExtendedEventDr(dvbpsi_extended_event_dr_t * p_decoded,
                                                bool b_duplicate)
{
    int i_items_length;
    int i_payload_length;
    dvbpsi_descriptor_t * p_descriptor;

    if (p_decoded == NULL || p_decoded->i_entry_count < 0 ||
        p_decoded->i_entry_count > 126 ||
        p_decoded->i_text_length < 0 || p_decoded->i_text_length > 255)
        return NULL;

    i_items_length = 0;
    for (int i = 0; i < p_decoded->i_entry_count; i++)
    {
        int i_description_length = p_decoded->i_item_description_length[i];
        int i_item_length = p_decoded->i_item_length[i];
        if (i_description_length < 0 || i_description_length > 255 ||
            i_item_length < 0 || i_item_length > 255)
            return NULL;
        if ((i_description_length > 0 &&
                p_decoded->i_item_description[i] == NULL) ||
            (i_item_length > 0 && p_decoded->i_item[i] == NULL))
            return NULL;
        /* 2 length bytes per pair, each field length fits in one byte. */
        i_items_length += 2 + i_description_length + i_item_length;
        if (i_items_length > 255)
            return NULL;
    }

    if (p_decoded->i_text_length > 0 && p_decoded->i_text == NULL)
        return NULL;

    /* Fixed part: number/last (1) + language (3) + items_length (1) +
     * items loop + text_length (1) + text. */
    i_payload_length = 1 + 3 + 1 + i_items_length + 1 +
                       p_decoded->i_text_length;
    if (i_payload_length > 255)
        return NULL;

    /* Create the descriptor */
    p_descriptor = dvbpsi_NewDescriptor(0x4e, i_payload_length, NULL );
    if (!p_descriptor)
        return NULL;

    uint8_t *p = &p_descriptor->p_data[0];

    /* Encode data */
    p[0] = (p_decoded->i_descriptor_number << 4 ) |
            p_decoded->i_last_descriptor_number;
    memcpy( &p[1], p_decoded->i_iso_639_code, 3 );
    p[4] = (uint8_t)i_items_length;

    p += 5;

    for (int i = 0; i < p_decoded->i_entry_count; i++)
    {
        p[0] = p_decoded->i_item_description_length[i];
        if (p[0] > 0)
            memcpy( &p[1], p_decoded->i_item_description[i], p[0] );
        p += 1 + p_decoded->i_item_description_length[i];

        p[0] = p_decoded->i_item_length[i];
        if (p[0] > 0)
            memcpy( &p[1], p_decoded->i_item[i], p[0] );
        p += 1 + p_decoded->i_item_length[i];
    }

    p[0] = (uint8_t)p_decoded->i_text_length;
    if (p[0] > 0)
        memcpy( &p[1], p_decoded->i_text, p[0] );

    if (b_duplicate)
    {
        /* Build a self contained duplicate. The source structure may point
         * at caller memory rather than at its own i_buffer, so the bytes of
         * every item field and of the text are copied into the duplicate's
         * i_buffer and the pointers are rewritten there. This mirrors the
         * layout produced by the decoder and makes deleting the descriptor
         * free everything without touching caller memory. */
        dvbpsi_extended_event_dr_t *p_duplicate =
                dvbpsi_DuplicateDecodedDescriptor(p_decoded,
                                                  sizeof(*p_duplicate));
        if (p_duplicate == NULL)
        {
            dvbpsi_DeleteDescriptors(p_descriptor);
            return NULL;
        }

        int i_pos = 0;
        bool b_ok = true;
        for (int i = 0; i < p_duplicate->i_entry_count; i++)
        {
            int i_description_length =
                    p_duplicate->i_item_description_length[i];
            int i_item_length = p_duplicate->i_item_length[i];

            if (i_pos + i_description_length + i_item_length >
                    (int)sizeof(p_duplicate->i_buffer))
            {
                b_ok = false;
                break;
            }

            p_duplicate->i_item_description[i] =
                    (i_description_length > 0) ?
                        &p_duplicate->i_buffer[i_pos] : NULL;
            if (i_description_length > 0)
            {
                memcpy(&p_duplicate->i_buffer[i_pos],
                       p_decoded->i_item_description[i],
                       i_description_length);
                i_pos += i_description_length;
            }

            p_duplicate->i_item[i] = (i_item_length > 0) ?
                        &p_duplicate->i_buffer[i_pos] : NULL;
            if (i_item_length > 0)
            {
                memcpy(&p_duplicate->i_buffer[i_pos],
                       p_decoded->i_item[i], i_item_length);
                i_pos += i_item_length;
            }
        }

        if (b_ok && i_pos + p_duplicate->i_text_length >
                        (int)sizeof(p_duplicate->i_buffer))
            b_ok = false;

        if (b_ok)
        {
            p_duplicate->i_text = (p_duplicate->i_text_length > 0) ?
                        &p_duplicate->i_buffer[i_pos] : NULL;
            if (p_duplicate->i_text_length > 0)
                memcpy(&p_duplicate->i_buffer[i_pos],
                       p_decoded->i_text, p_duplicate->i_text_length);
        }

        if (!b_ok)
        {
            free(p_duplicate);
            dvbpsi_DeleteDescriptors(p_descriptor);
            return NULL;
        }

        p_descriptor->p_decoded = p_duplicate;
    }

    return p_descriptor;
}
