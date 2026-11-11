/*****************************************************************************
 * eit_extended.h
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

/*!
 * \file <eit_extended.h>
 * \brief High level write/read interface for the EIT "extended event"
 * descriptor (0x4e) series of one language.
 *
 * This interface groups the whole extended event descriptor series
 * (ETSI EN 300 468 section 6.2.15) of one ISO 639-2 language behind two
 * calls: one writes a UTF-8 description plus an ordered list of name/value
 * items into an EIT event, the other reads them back.
 *
 * Only the DVB UTF-8 character table (selection byte 0x15) and empty text
 * fields are handled. Every non-empty text field is stored with its own
 * 0x15 selection byte; an empty field is stored with length zero and no
 * selection byte. The low level dvbpsi_DecodeExtendedEventDr() entry point
 * keeps accepting every other encoding.
 */

#ifndef _DVBPSI_EIT_EXTENDED_H_
#define _DVBPSI_EIT_EXTENDED_H_

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * Error codes
 *****************************************************************************/
/*!
 * \enum dvbpsi_eit_extended_error_s
 * \brief Error codes reported by the extended event text interface.
 */
/*!
 * \typedef enum dvbpsi_eit_extended_error_s dvbpsi_eit_extended_error_t
 */
enum dvbpsi_eit_extended_error_s
{
  DVBPSI_EIT_EXTENDED_OK = 0,              /*!< no error */
  DVBPSI_EIT_EXTENDED_ERR_ARGUMENT,        /*!< a mandatory argument is wrong */
  DVBPSI_EIT_EXTENDED_ERR_MEMORY,          /*!< memory allocation failed */
  DVBPSI_EIT_EXTENDED_ERR_UTF8,            /*!< an input byte string is not
                                                valid UTF-8 */
  DVBPSI_EIT_EXTENDED_ERR_ITEM_TOO_LARGE,  /*!< a single name/value item pair
                                                cannot fit in one descriptor */
  DVBPSI_EIT_EXTENDED_ERR_TOO_MANY_DESCRIPTORS, /*!< more than 16 descriptors
                                                would be needed */
  DVBPSI_EIT_EXTENDED_ERR_EVENT_TOO_LARGE, /*!< the event would exceed the
                                                4076 byte EIT event limit */
  DVBPSI_EIT_EXTENDED_ERR_NOT_FOUND,       /*!< the event holds no extended
                                                event descriptor in that
                                                language */
  DVBPSI_EIT_EXTENDED_ERR_ENCODING,        /*!< the stored text uses a character
                                                table other than UTF-8 (0x15) */
  DVBPSI_EIT_EXTENDED_ERR_MALFORMED        /*!< the descriptor series is broken:
                                                gap, duplicate number,
                                                contradictory last descriptor
                                                number, truncated item or
                                                invalid UTF-8 on the wire */
};
typedef enum dvbpsi_eit_extended_error_s dvbpsi_eit_extended_error_t;

/*****************************************************************************
 * dvbpsi_extended_text_item_t
 *****************************************************************************/
/*!
 * \struct dvbpsi_extended_text_item_s
 * \brief One ordered name/value item given to the writer.
 *
 * The two fields are a pair and are always stored in the same descriptor.
 * A NULL pointer or a length of zero denotes an empty field. The byte
 * strings are borrowed by dvbpsi_eit_event_set_extended_text() for the
 * duration of the call only and are fully copied into the event.
 */
/*!
 * \typedef struct dvbpsi_extended_text_item_s dvbpsi_extended_text_item_t
 */
typedef struct dvbpsi_extended_text_item_s
{
  const uint8_t * p_name;         /*!< item name, UTF-8, may be NULL */
  int             i_name_length;  /*!< item name length in bytes */
  const uint8_t * p_value;        /*!< item value, UTF-8, may be NULL */
  int             i_value_length; /*!< item value length in bytes */
} dvbpsi_extended_text_item_t;

/*****************************************************************************
 * dvbpsi_extended_text_item_result_t
 *****************************************************************************/
/*!
 * \struct dvbpsi_extended_text_item_result_s
 * \brief One name/value item returned by the reader.
 *
 * The byte strings are freshly allocated copies. An empty field is reported
 * with a length of zero and a NULL pointer.
 */
typedef struct dvbpsi_extended_text_item_result_s
{
  uint8_t *       p_name;         /*!< item name, UTF-8, may be NULL */
  int             i_name_length;  /*!< item name length in bytes */
  uint8_t *       p_value;        /*!< item value, UTF-8, may be NULL */
  int             i_value_length; /*!< item value length in bytes */
} dvbpsi_extended_text_item_result_t;

/*****************************************************************************
 * dvbpsi_extended_text_t
 *****************************************************************************/
/*!
 * \struct dvbpsi_extended_text_s
 * \brief The complete extended event text of one language.
 *
 * This structure and every byte string it points to are owned by the caller
 * and are independent of the dvbpsi_eit_event_t, the descriptor decoder
 * caches and any temporary workspace used by the library. It remains valid
 * after the event, the EIT table or the dvbpsi handle have been deleted.
 *
 * Release it with dvbpsi_extended_text_delete(); it must not be freed with
 * free() directly. p_text is NULL (and i_text_length zero) when the stored
 * description is empty. The item order matches the descriptor series order.
 */
/*!
 * \typedef struct dvbpsi_extended_text_s dvbpsi_extended_text_t
 */
typedef struct dvbpsi_extended_text_s
{
  uint8_t                            i_iso_639_code[3]; /*!< ISO 639-2
                                                            language code */
  uint8_t *                          p_text;  /*!< full description, UTF-8 */
  int                                i_text_length; /*!< description length
                                                         in bytes */
  int                                i_item_count; /*!< number of items */
  dvbpsi_extended_text_item_result_t *p_items; /*!< items, in order */
} dvbpsi_extended_text_t;

/*****************************************************************************
 * dvbpsi_extended_text_delete
 *****************************************************************************/
/*!
 * \fn void dvbpsi_extended_text_delete(dvbpsi_extended_text_t *p_result)
 * \brief Release a result obtained from
 * dvbpsi_eit_event_get_extended_text().
 * \param p_result result structure, NULL is accepted and does nothing.
 *
 * Frees the structure, the item array and every copied byte string.
 */
void dvbpsi_extended_text_delete(dvbpsi_extended_text_t *p_result);

/*****************************************************************************
 * dvbpsi_eit_event_set_extended_text
 *****************************************************************************/
/*!
 * \fn bool dvbpsi_eit_event_set_extended_text(dvbpsi_eit_event_t *p_event,
 *                       const uint8_t i_iso_639_code[3],
 *                       const uint8_t *p_text, int i_text_length,
 *                       const dvbpsi_extended_text_item_t *p_items,
 *                       int i_item_count,
 *                       dvbpsi_eit_extended_error_t *p_error)
 * \brief Write the extended event descriptor series of one language.
 *
 * Encodes p_text (a single UTF-8 string) and the ordered p_items list into a
 * series of 0x4e descriptors carrying i_iso_639_code. The description may be
 * split over several descriptors, always on UTF-8 code point boundaries; a
 * name/value pair is never split. Descriptor numbers start at zero and are
 * contiguous, the last descriptor number equals the index of the last
 * descriptor and at most 16 descriptors are generated.
 *
 * If the event already contains a 0x4e series in the same language, that
 * series is replaced: the new series takes the position of the first
 * replaced descriptor. Descriptors in other languages and descriptors with
 * other tags keep their original position and raw bytes. When no previous
 * series exists the new one is appended.
 *
 * Ownership: the input strings are borrowed and copied; the event keeps
 * nothing that points at caller memory. On error the event is left
 * completely unchanged.
 *
 * \param p_event event to update
 * \param i_iso_639_code three letter ISO 639-2 language code
 * \param p_text UTF-8 description, may be NULL when i_text_length is zero
 * \param i_text_length description length in bytes
 * \param p_items ordered name/value items, may be NULL when i_item_count
 *                is zero
 * \param i_item_count number of items
 * \param p_error optional error code output, may be NULL
 * \return true on success, false on error (see p_error)
 */
bool dvbpsi_eit_event_set_extended_text(dvbpsi_eit_event_t *p_event,
                       const uint8_t i_iso_639_code[3],
                       const uint8_t *p_text, int i_text_length,
                       const dvbpsi_extended_text_item_t *p_items,
                       int i_item_count,
                       dvbpsi_eit_extended_error_t *p_error);

/*****************************************************************************
 * dvbpsi_eit_event_get_extended_text
 *****************************************************************************/
/*!
 * \fn dvbpsi_extended_text_t *dvbpsi_eit_event_get_extended_text(
 *                       dvbpsi_eit_event_t *p_event,
 *                       const uint8_t i_iso_639_code[3],
 *                       dvbpsi_eit_extended_error_t *p_error)
 * \brief Read the extended event text of one language.
 *
 * Gathers every 0x4e descriptor of i_iso_639_code regardless of their
 * physical position in the descriptor list and reassembles the description
 * fragments and the item list according to descriptor_number. Descriptors of
 * other languages and other descriptor tags are ignored.
 *
 * The series is rejected (NULL result and an error code) when descriptor
 * numbers are missing, duplicated, not contiguous from zero, when the last
 * descriptor numbers disagree, when an item pair is truncated, when a text
 * field uses a character table other than UTF-8 (0x15), or when the
 * reassembled byte strings are not valid UTF-8. No partially decoded text is
 * returned.
 *
 * Ownership: on success the returned dvbpsi_extended_text_t and all its
 * strings are freshly allocated, owned by the caller and independent of the
 * event and of decoder caches; release them with
 * dvbpsi_extended_text_delete(). On failure NULL is returned and nothing is
 * allocated.
 *
 * \param p_event event to inspect
 * \param i_iso_639_code three letter ISO 639-2 language code
 * \param p_error optional error code output, may be NULL
 * \return a newly allocated result or NULL
 */
dvbpsi_extended_text_t *dvbpsi_eit_event_get_extended_text(
                       dvbpsi_eit_event_t *p_event,
                       const uint8_t i_iso_639_code[3],
                       dvbpsi_eit_extended_error_t *p_error);

#ifdef __cplusplus
};
#endif

/*!
 * \example
 * The following program writes a bilingual event, reads one language back
 * and releases the result. It compiles against the public headers:
 *
 * \code
 * #include <assert.h>
 * #include <stdint.h>
 * #include <stdbool.h>
 * #include <string.h>
 * #include <dvbpsi/dvbpsi.h>
 * #include <dvbpsi/descriptor.h>
 * #include <dvbpsi/tables/eit.h>
 * #include <dvbpsi/tables/eit_extended.h>
 *
 * int main(void)
 * {
 *     static const uint8_t chi[] = {0xe4,0xbb,0x8a,0xe4,0xbb,0xac}; // "介介"
 *     const uint8_t lang[3] = { 'c', 'h', 'i' };
 *     dvbpsi_extended_text_item_t items[1] = {
 *         { (const uint8_t *)"Director", 8,
 *           (const uint8_t *)"A. Filmmaker", 12 }
 *     };
 *     dvbpsi_eit_extended_error_t err;
 *
 *     dvbpsi_eit_t *eit = dvbpsi_eit_new(0x4e, 7, 1, true, 8, 9, 0, 0x4e);
 *     assert(eit != NULL);
 *     dvbpsi_eit_event_t *event =
 *         dvbpsi_eit_event_add(eit, 42, 0xea60000000ULL, 0x010203, 4,
 *                              false, 0);
 *     assert(event != NULL);
 *
 *     if (!dvbpsi_eit_event_set_extended_text(event, lang, chi,
 *                                             (int)sizeof(chi), items, 1,
 *                                             &err))
 *         return 1;
 *
 *     dvbpsi_extended_text_t *result =
 *         dvbpsi_eit_event_get_extended_text(event, lang, &err);
 *     assert(result != NULL);
 *     assert(result->i_text_length == (int)sizeof(chi));
 *     assert(memcmp(result->p_text, chi, sizeof(chi)) == 0);
 *     assert(result->i_item_count == 1);
 *     assert(result->p_items[0].i_name_length == 8);
 *
 *     // The result is independent: the event (and the whole EIT) can go.
 *     dvbpsi_eit_delete(eit);
 *     assert(memcmp(result->p_text, chi, sizeof(chi)) == 0);
 *     dvbpsi_extended_text_delete(result);
 *     return 0;
 * }
 * \endcode
 */

#else
#error "Multiple inclusions of eit_extended.h"
#endif
