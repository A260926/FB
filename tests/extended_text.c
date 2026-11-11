/*****************************************************************************
 * extended_text.c: regression tests for the high level extended event text
 * interface (src/tables/eit_extended.c).
 *
 * Run with: bash regress/run.sh tests/extended_text.c
 *****************************************************************************/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>

#include "dvbpsi.h"
#include "psi.h"
#include "demux.h"
#include "descriptor.h"
#include "tables/eit.h"
#include "tables/eit_extended.h"
#include "descriptors/dr_4e.h"

static int g_failures = 0;

#define CHECK(cond) do {                                                      \
    if (!(cond)) {                                                            \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        g_failures++;                                                         \
    }                                                                         \
} while (0)

static const uint8_t LANG_ENG[3] = { 'e', 'n', 'g' };
static const uint8_t LANG_DEU[3] = { 'd', 'e', 'u' };
static const uint8_t LANG_CHI[3] = { 'c', 'h', 'i' };

/* "节目介绍" (4 Chinese characters, 12 UTF-8 bytes). */
static const uint8_t CHI_TEXT[] = {
    0xe8, 0x8a, 0x82, 0xe7, 0x9b, 0xae,
    0xe4, 0xbb, 0x8b, 0xe7, 0xbb, 0x8d
};

/* One 3-byte Chinese character, used for boundary checks. */
#define CHI_CHAR { 0xe4, 0xbd, 0xa0 }

static dvbpsi_eit_event_t *new_event(dvbpsi_eit_t **pp_eit)
{
    *pp_eit = dvbpsi_eit_new(0x4e, 7, 1, true, 8, 9, 0, 0x4e);
    CHECK(*pp_eit != NULL);
    dvbpsi_eit_event_t *ev = dvbpsi_eit_event_add(*pp_eit, 42,
            0xea60000000ULL, 0x010203, 4, false, 0);
    CHECK(ev != NULL);
    return ev;
}

static int count_descriptors(dvbpsi_eit_event_t *ev, uint8_t tag,
                             const uint8_t lang[3])
{
    int n = 0;
    for (dvbpsi_descriptor_t *d = ev->p_first_descriptor; d; d = d->p_next)
        if (d->i_tag == tag &&
            (lang == NULL ||
             (d->i_length >= 4 && memcmp(&d->p_data[1], lang, 3) == 0)))
            n++;
    return n;
}

static dvbpsi_descriptor_t *find_descriptor(dvbpsi_eit_event_t *ev, int idx,
                                            uint8_t tag,
                                            const uint8_t lang[3])
{
    int n = 0;
    for (dvbpsi_descriptor_t *d = ev->p_first_descriptor; d; d = d->p_next)
    {
        if (d->i_tag == tag &&
            (lang == NULL ||
             (d->i_length >= 4 && memcmp(&d->p_data[1], lang, 3) == 0)))
        {
            if (n == idx)
                return d;
            n++;
        }
    }
    return NULL;
}

/*****************************************************************************
 * Low level dr_4e checks: empty items loop, payload limits, duplicate mode
 *****************************************************************************/
static void test_low_level_dr4e(void)
{
    /* regress/observe.c: a three byte text with no items must encode an
     * items_length of 0, not 3. */
    uint8_t text[] = { 0x15, 'h', 'i' };
    dvbpsi_extended_event_dr_t in;
    memset(&in, 0, sizeof(in));
    memcpy(in.i_iso_639_code, LANG_ENG, 3);
    in.i_text_length = sizeof(text);
    in.i_text = text;

    dvbpsi_descriptor_t *d = dvbpsi_GenExtendedEventDr(&in, false);
    CHECK(d != NULL);
    CHECK(d->i_tag == 0x4e);
    CHECK(d->i_length <= 255);
    CHECK(d->p_data[4] == 0);                /* items_length */
    CHECK(d->p_data[5] == 3);                /* text_length */

    /* Round trip through the decoder. */
    dvbpsi_extended_event_dr_t *dec = dvbpsi_DecodeExtendedEventDr(d);
    CHECK(dec != NULL);
    CHECK(dec->i_entry_count == 0);
    CHECK(dec->i_text_length == 3);
    CHECK(memcmp(dec->i_text, text, 3) == 0);
    /* Decoded text lives in the internal buffer, not in p_data. */
    CHECK(dec->i_text >= dec->i_buffer &&
          dec->i_text < dec->i_buffer + sizeof(dec->i_buffer));
    dvbpsi_DeleteDescriptors(d);

    /* b_duplicate: the duplicated decoded structure must own its pointers. */
    memset(&in, 0, sizeof(in));
    memcpy(in.i_iso_639_code, LANG_ENG, 3);
    in.i_entry_count = 1;
    uint8_t name[] = { 0x15, 'N' };
    uint8_t value[] = { 0x15, 'V' };
    in.i_item_description_length[0] = sizeof(name);
    in.i_item_description[0] = name;
    in.i_item_length[0] = sizeof(value);
    in.i_item[0] = value;

    d = dvbpsi_GenExtendedEventDr(&in, true);
    CHECK(d != NULL && d->p_decoded != NULL);
    dvbpsi_extended_event_dr_t *dup = d->p_decoded;
    CHECK(dup != &in);
    CHECK(dup->i_item_description[0] >= dup->i_buffer &&
          dup->i_item_description[0] < dup->i_buffer + sizeof(dup->i_buffer));
    CHECK(dup->i_item[0] >= dup->i_buffer &&
          dup->i_item[0] < dup->i_buffer + sizeof(dup->i_buffer));
    CHECK(dup->i_item_description[0] != in.i_item_description[0]);
    CHECK(dup->i_item_description[0][0] == 0x15);
    CHECK(dup->i_item[0][1] == 'V');
    dvbpsi_DeleteDescriptors(d);   /* must free the duplicate cleanly */

    /* A payload that would exceed 255 bytes is rejected, not truncated. */
    memset(&in, 0, sizeof(in));
    memcpy(in.i_iso_639_code, LANG_ENG, 3);
    uint8_t big[255];
    memset(big, 'a', sizeof(big));
    in.i_text_length = sizeof(big);
    in.i_text = big;
    CHECK(dvbpsi_GenExtendedEventDr(&in, false) == NULL);

    /* A truncated items loop is rejected by the decoder instead of reading
     * past the payload. */
    uint8_t raw_bad[] = {
        0x00, 'e', 'n', 'g',
        5,                /* items_length claims 5 bytes */
        3, 1, 2, 3,       /* only 4 bytes follow before text_length */
        0
    };
    dvbpsi_descriptor_t *bad =
            dvbpsi_NewDescriptor(0x4e, sizeof(raw_bad), raw_bad);
    CHECK(bad != NULL);
    CHECK(dvbpsi_DecodeExtendedEventDr(bad) == NULL);
    dvbpsi_DeleteDescriptors(bad);
}

/*****************************************************************************
 * Basic write/read: Chinese text, English items, empty fields
 *****************************************************************************/
static void test_basic_roundtrip(void)
{
    dvbpsi_eit_t *eit;
    dvbpsi_eit_event_t *ev = new_event(&eit);
    dvbpsi_eit_extended_error_t err;

    dvbpsi_extended_text_item_t items[3] = {
        { (const uint8_t *)"Director", 8,
          (const uint8_t *)"A. Filmmaker", 12 },
        { NULL, 0, (const uint8_t *)"no name given", 13 },
        { (const uint8_t *)"Sound", 5, NULL, 0 }
    };

    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_CHI, CHI_TEXT,
            sizeof(CHI_TEXT), items, 3, &err));

    /* One descriptor is enough for this small input. */
    CHECK(count_descriptors(ev, 0x4e, LANG_CHI) == 1);
    dvbpsi_descriptor_t *d = find_descriptor(ev, 0, 0x4e, LANG_CHI);
    CHECK(d != NULL);
    CHECK(d->i_length <= 255);
    CHECK((d->p_data[0] >> 4) == 0);      /* descriptor number 0 */
    CHECK((d->p_data[0] & 0x0f) == 0);    /* last descriptor number 0 */
    /* The description follows the items loop; every non-empty field starts
     * with the UTF-8 selection byte. */
    int text_len_off = 5 + d->p_data[4];
    CHECK(d->p_data[text_len_off] == 1 + (int)sizeof(CHI_TEXT));
    CHECK(d->p_data[text_len_off + 1] == 0x15);

    dvbpsi_extended_text_t *r =
            dvbpsi_eit_event_get_extended_text(ev, LANG_CHI, &err);
    CHECK(r != NULL);
    if (r)
    {
        CHECK(r->i_text_length == (int)sizeof(CHI_TEXT));
        CHECK(r->p_text != NULL);
        CHECK(memcmp(r->p_text, CHI_TEXT, sizeof(CHI_TEXT)) == 0);
        CHECK(r->i_item_count == 3);
        CHECK(r->p_items[0].i_name_length == 8);
        CHECK(memcmp(r->p_items[0].p_name, "Director", 8) == 0);
        CHECK(r->p_items[0].i_value_length == 12);
        CHECK(r->p_items[1].p_name == NULL &&
              r->p_items[1].i_name_length == 0);
        CHECK(r->p_items[1].i_value_length == 13);
        CHECK(r->p_items[2].p_value == NULL &&
              r->p_items[2].i_value_length == 0);
        CHECK(r->p_items[2].i_name_length == 5);
        dvbpsi_extended_text_delete(r);
    }
    dvbpsi_eit_delete(eit);
}

/*****************************************************************************
 * The read result survives the event and the whole EIT being deleted.
 *****************************************************************************/
static void test_independent_ownership(void)
{
    dvbpsi_eit_t *eit;
    dvbpsi_eit_event_t *ev = new_event(&eit);
    dvbpsi_eit_extended_error_t err;

    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_CHI, CHI_TEXT,
            sizeof(CHI_TEXT), NULL, 0, &err));

    dvbpsi_extended_text_t *r =
            dvbpsi_eit_event_get_extended_text(ev, LANG_CHI, &err);
    CHECK(r != NULL);

    /* Free the input string's owner, the event and the whole table. */
    dvbpsi_eit_delete(eit);

    /* The result must still be fully readable. */
    CHECK(r != NULL && r->i_text_length == (int)sizeof(CHI_TEXT));
    CHECK(memcmp(r->p_text, CHI_TEXT, sizeof(CHI_TEXT)) == 0);
    dvbpsi_extended_text_delete(r);
    dvbpsi_extended_text_delete(NULL);   /* must be a no-op */
}

/*****************************************************************************
 * Long description without items: text is split on UTF-8 code point
 * boundaries and reassembled exactly.
 *****************************************************************************/
static void test_long_text_no_items(void)
{
    /* 300 Chinese characters = 900 bytes. */
    int nchar = 300;
    uint8_t *text = malloc((size_t)nchar * 3);
    CHECK(text != NULL);
    for (int i = 0; i < nchar; i++)
    {
        static const uint8_t c[3] = CHI_CHAR;
        memcpy(text + 3 * i, c, 3);
    }

    dvbpsi_eit_t *eit;
    dvbpsi_eit_event_t *ev = new_event(&eit);
    dvbpsi_eit_extended_error_t err;

    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_CHI, text,
            nchar * 3, NULL, 0, &err));

    int nd = count_descriptors(ev, 0x4e, LANG_CHI);
    CHECK(nd >= 2 && nd <= 16);
    for (int i = 0; i < nd; i++)
    {
        dvbpsi_descriptor_t *d = find_descriptor(ev, i, 0x4e, LANG_CHI);
        CHECK(d != NULL);
        CHECK(d->i_length <= 255);
        CHECK((d->p_data[0] >> 4) == i);
        CHECK((d->p_data[0] & 0x0f) == nd - 1);
        CHECK(d->p_data[4] == 0);            /* no items */
        /* The text fragment must begin with 0x15 and never end halfway
         * through a three byte character: the next descriptor's fragment
         * starts on a lead byte, and the payload itself is whole. */
        int tl = d->p_data[5];
        CHECK(tl > 0 && d->p_data[6] == 0x15);
        CHECK((tl - 1) % 3 == 0);
    }

    dvbpsi_extended_text_t *r =
            dvbpsi_eit_event_get_extended_text(ev, LANG_CHI, &err);
    CHECK(r != NULL);
    if (r)
    {
        CHECK(r->i_text_length == nchar * 3);
        CHECK(r->i_item_count == 0);
        CHECK(r->p_items == NULL);
        CHECK(memcmp(r->p_text, text, (size_t)nchar * 3) == 0);
        dvbpsi_extended_text_delete(r);
    }

    dvbpsi_eit_delete(eit);
    free(text);
}

/*****************************************************************************
 * Only items, empty description; and items spread over several descriptors.
 *****************************************************************************/
static void test_items_only_and_spread(void)
{
    dvbpsi_eit_t *eit;
    dvbpsi_eit_event_t *ev = new_event(&eit);
    dvbpsi_eit_extended_error_t err;

    const int nitems = 40;
    dvbpsi_extended_text_item_t *items =
            calloc((size_t)nitems, sizeof(*items));
    CHECK(items != NULL);
    for (int i = 0; i < nitems; i++)
    {
        items[i].p_name = (const uint8_t *)"Name";
        items[i].i_name_length = 4;
        items[i].p_value = (const uint8_t *)"Value";
        items[i].i_value_length = 5;
    }

    /* Empty description (NULL and zero length). */
    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_ENG, NULL, 0,
            items, nitems, &err));
    CHECK(count_descriptors(ev, 0x4e, LANG_ENG) >= 2);

    dvbpsi_extended_text_t *r =
            dvbpsi_eit_event_get_extended_text(ev, LANG_ENG, &err);
    CHECK(r != NULL);
    if (r)
    {
        CHECK(r->i_text_length == 0 && r->p_text == NULL);
        CHECK(r->i_item_count == nitems);
        for (int i = 0; i < nitems; i++)
        {
            CHECK(r->p_items[i].i_name_length == 4 &&
                  memcmp(r->p_items[i].p_name, "Name", 4) == 0);
            CHECK(r->p_items[i].i_value_length == 5 &&
                  memcmp(r->p_items[i].p_value, "Value", 5) == 0);
        }
        dvbpsi_extended_text_delete(r);
    }

    /* Every name/value pair stays inside one descriptor: decode each
     * generated descriptor and check its item boundaries. */
    int nd = count_descriptors(ev, 0x4e, LANG_ENG);
    for (int i = 0; i < nd; i++)
    {
        dvbpsi_descriptor_t *d = find_descriptor(ev, i, 0x4e, LANG_ENG);
        dvbpsi_extended_event_dr_t *dec = dvbpsi_DecodeExtendedEventDr(d);
        CHECK(dec != NULL && dec->i_entry_count > 0);
        CHECK(dec->i_text_length == 0);
    }

    dvbpsi_eit_delete(eit);
    free(items);
}

/*****************************************************************************
 * Two languages mixed in one event and replacement semantics.
 *****************************************************************************/
static void test_two_languages_and_replace(void)
{
    dvbpsi_eit_t *eit;
    dvbpsi_eit_event_t *ev = new_event(&eit);
    dvbpsi_eit_extended_error_t err;

    /* A descriptor of another tag that must survive untouched. */
    uint8_t raw55[20];
    memset(raw55, 0x5a, sizeof(raw55));
    dvbpsi_descriptor_t *keep55 =
            dvbpsi_eit_event_descriptor_add(ev, 0x55, sizeof(raw55), raw55);
    CHECK(keep55 != NULL);

    dvbpsi_extended_text_item_t eng_items[1] = {
        { (const uint8_t *)"Title", 5, (const uint8_t *)"Old", 3 }
    };
    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_ENG,
            (const uint8_t *)"english description", 19, eng_items, 1, &err));

    dvbpsi_extended_text_item_t deu_items[1] = {
        { (const uint8_t *)"Titel", 5, (const uint8_t *)"Alt", 3 }
    };
    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_DEU,
            (const uint8_t *)"deutsche beschreibung", 21, deu_items, 1,
            &err));

    /* Order is now: 0x55, eng, deu. Snapshot the bytes that must survive. */
    uint8_t snap55[2 + 255];
    int snap55_len;
    uint8_t snap_deu[2 + 255];
    int snap_deu_len;
    {
        dvbpsi_descriptor_t *d55 = find_descriptor(ev, 0, 0x55, NULL);
        snap55_len = 2 + d55->i_length;
        snap55[0] = d55->i_tag; snap55[1] = d55->i_length;
        memcpy(snap55 + 2, d55->p_data, d55->i_length);

        dvbpsi_descriptor_t *ddeu = find_descriptor(ev, 0, 0x4e, LANG_DEU);
        snap_deu_len = 2 + ddeu->i_length;
        snap_deu[0] = ddeu->i_tag; snap_deu[1] = ddeu->i_length;
        memcpy(snap_deu + 2, ddeu->p_data, ddeu->i_length);
    }

    /* Replace English with a longer, two-descriptor series. */
    uint8_t *longeng = malloc(400);
    CHECK(longeng != NULL);
    memset(longeng, 'e', 400);
    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_ENG, longeng, 400,
            NULL, 0, &err));
    free(longeng);

    CHECK(count_descriptors(ev, 0x4e, LANG_ENG) == 2);
    CHECK(count_descriptors(ev, 0x4e, LANG_DEU) == 1);
    CHECK(count_descriptors(ev, 0x55, NULL) == 1);

    /* The new series occupies the position of the first replaced one:
     * 0x55, eng#0, eng#1, deu. */
    dvbpsi_descriptor_t *order[4];
    int n = 0;
    for (dvbpsi_descriptor_t *d = ev->p_first_descriptor; d && n < 4;
         d = d->p_next)
        order[n++] = d;
    CHECK(n == 4);
    CHECK(order[0]->i_tag == 0x55);
    CHECK(order[1]->i_tag == 0x4e &&
          memcmp(&order[1]->p_data[1], LANG_ENG, 3) == 0 &&
          (order[1]->p_data[0] >> 4) == 0);
    CHECK(order[2]->i_tag == 0x4e &&
          memcmp(&order[2]->p_data[1], LANG_ENG, 3) == 0 &&
          (order[2]->p_data[0] >> 4) == 1);
    CHECK(order[3]->i_tag == 0x4e &&
          memcmp(&order[3]->p_data[1], LANG_DEU, 3) == 0);

    /* The untouched descriptors keep their exact raw bytes. */
    CHECK(snap55_len == 2 + order[0]->i_length &&
          memcmp(snap55 + 2, order[0]->p_data, order[0]->i_length) == 0);
    CHECK(snap_deu_len == 2 + order[3]->i_length &&
          memcmp(snap_deu + 2, order[3]->p_data, order[3]->i_length) == 0);

    dvbpsi_extended_text_t *rdeu =
            dvbpsi_eit_event_get_extended_text(ev, LANG_DEU, &err);
    CHECK(rdeu != NULL && rdeu->i_text_length == 21 &&
          memcmp(rdeu->p_text, "deutsche beschreibung", 21) == 0);
    if (rdeu)
        dvbpsi_extended_text_delete(rdeu);

    dvbpsi_extended_text_t *reng =
            dvbpsi_eit_event_get_extended_text(ev, LANG_ENG, &err);
    CHECK(reng != NULL && reng->i_text_length == 400);
    if (reng)
    {
        for (int i = 0; i < 400; i++)
            CHECK(reng->p_text[i] == 'e');
        dvbpsi_extended_text_delete(reng);
    }

    /* An unknown language is reported as not found. */
    static const uint8_t lang_fra[3] = { 'f', 'r', 'a' };
    CHECK(dvbpsi_eit_event_get_extended_text(ev, lang_fra, &err) == NULL);
    CHECK(err == DVBPSI_EIT_EXTENDED_ERR_NOT_FOUND);

    dvbpsi_eit_delete(eit);
}

/*****************************************************************************
 * Descriptors physically shuffled in the list must still be read in number
 * order, and foreign tags/languages must not be joined into the series.
 *****************************************************************************/
static void test_out_of_order_and_foreign_descriptors(void)
{
    dvbpsi_eit_t *eit;
    dvbpsi_eit_event_t *ev = new_event(&eit);
    dvbpsi_eit_extended_error_t err;

    uint8_t text[500];
    memset(text, 'z', sizeof(text));
    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_ENG, text,
            sizeof(text), NULL, 0, &err));
    CHECK(count_descriptors(ev, 0x4e, LANG_ENG) == 3);

    /* Add noise: another language and a non 0x4e descriptor in the middle. */
    uint8_t noise[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
    CHECK(dvbpsi_eit_event_descriptor_add(ev, 0x66, sizeof(noise), noise));
    dvbpsi_extended_text_item_t deu[1] = {
        { (const uint8_t *)"x", 1, (const uint8_t *)"y", 1 }
    };
    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_DEU,
            (const uint8_t *)"deu", 3, deu, 1, &err));

    /* Reverse the whole descriptor list physically. */
    dvbpsi_descriptor_t *rev = NULL;
    while (ev->p_first_descriptor)
    {
        dvbpsi_descriptor_t *d = ev->p_first_descriptor;
        ev->p_first_descriptor = d->p_next;
        d->p_next = rev;
        rev = d;
    }
    ev->p_first_descriptor = rev;

    dvbpsi_extended_text_t *r =
            dvbpsi_eit_event_get_extended_text(ev, LANG_ENG, &err);
    CHECK(r != NULL);
    if (r)
    {
        CHECK(r->i_text_length == (int)sizeof(text));
        for (int i = 0; i < (int)sizeof(text); i++)
            CHECK(r->p_text[i] == 'z');
        dvbpsi_extended_text_delete(r);
    }

    dvbpsi_eit_delete(eit);
}

/*****************************************************************************
 * Deterministic output: identical input yields identical wire bytes.
 *****************************************************************************/
static void test_stable_output(void)
{
    dvbpsi_eit_extended_error_t err;
    dvbpsi_eit_t *eit1, *eit2;
    dvbpsi_eit_event_t *ev1 = new_event(&eit1);
    dvbpsi_eit_event_t *ev2 = new_event(&eit2);

    uint8_t text[700];
    memset(text, 's', sizeof(text));
    dvbpsi_extended_text_item_t items[2] = {
        { (const uint8_t *)"a", 1, (const uint8_t *)"bb", 2 },
        { NULL, 0, (const uint8_t *)"ccc", 3 }
    };

    CHECK(dvbpsi_eit_event_set_extended_text(ev1, LANG_ENG, text,
            sizeof(text), items, 2, &err));
    CHECK(dvbpsi_eit_event_set_extended_text(ev2, LANG_ENG, text,
            sizeof(text), items, 2, &err));

    dvbpsi_descriptor_t *d1 = ev1->p_first_descriptor;
    dvbpsi_descriptor_t *d2 = ev2->p_first_descriptor;
    while (d1 && d2)
    {
        CHECK(d1->i_tag == d2->i_tag && d1->i_length == d2->i_length);
        CHECK(memcmp(d1->p_data, d2->p_data, d1->i_length) == 0);
        d1 = d1->p_next;
        d2 = d2->p_next;
    }
    CHECK(d1 == NULL && d2 == NULL);

    dvbpsi_eit_delete(eit1);
    dvbpsi_eit_delete(eit2);
}

/*****************************************************************************
 * Writer error cases: the event must remain unchanged on every failure.
 *****************************************************************************/
static void test_write_errors(void)
{
    dvbpsi_eit_extended_error_t err;

    /* Invalid UTF-8 input. */
    {
        dvbpsi_eit_t *eit;
        dvbpsi_eit_event_t *ev = new_event(&eit);
        uint8_t bad[] = { 'o', 'k', 0xff, 0xfe };
        CHECK(!dvbpsi_eit_event_set_extended_text(ev, LANG_ENG, bad,
                sizeof(bad), NULL, 0, &err));
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_UTF8);
        CHECK(ev->p_first_descriptor == NULL);
        dvbpsi_eit_delete(eit);
    }

    /* A single item pair that cannot fit in one descriptor. */
    {
        dvbpsi_eit_t *eit;
        dvbpsi_eit_event_t *ev = new_event(&eit);
        uint8_t big[254];
        memset(big, 'n', sizeof(big));
        dvbpsi_extended_text_item_t item[1] = {
            { big, (int)sizeof(big), big, (int)sizeof(big) }
        };
        CHECK(!dvbpsi_eit_event_set_extended_text(ev, LANG_ENG, NULL, 0,
                item, 1, &err));
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_ITEM_TOO_LARGE);
        CHECK(ev->p_first_descriptor == NULL);
        dvbpsi_eit_delete(eit);
    }

    /* More than 16 descriptors needed. */
    {
        dvbpsi_eit_t *eit;
        dvbpsi_eit_event_t *ev = new_event(&eit);
        int len = 16 * 248 + 1;          /* needs descriptor number 16 */
        uint8_t *big = malloc((size_t)len);
        CHECK(big != NULL);
        memset(big, 'a', (size_t)len);
        CHECK(!dvbpsi_eit_event_set_extended_text(ev, LANG_ENG, big, len,
                NULL, 0, &err));
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_TOO_MANY_DESCRIPTORS);
        CHECK(ev->p_first_descriptor == NULL);
        free(big);
        dvbpsi_eit_delete(eit);
    }

    /* Event would exceed 4076 bytes after replacement: prefill with another
     * tag, then ask for a large series. */
    {
        dvbpsi_eit_t *eit;
        dvbpsi_eit_event_t *ev = new_event(&eit);
        uint8_t filler[255];
        memset(filler, 0x77, sizeof(filler));
        CHECK(dvbpsi_eit_event_descriptor_add(ev, 0x55, sizeof(filler),
                                              filler));
        int len = 3800;
        uint8_t *big = malloc((size_t)len);
        CHECK(big != NULL);
        memset(big, 'a', (size_t)len);
        CHECK(!dvbpsi_eit_event_set_extended_text(ev, LANG_ENG, big, len,
                NULL, 0, &err));
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_EVENT_TOO_LARGE);
        CHECK(count_descriptors(ev, 0x4e, LANG_ENG) == 0);
        CHECK(count_descriptors(ev, 0x55, NULL) == 1);
        free(big);
        dvbpsi_eit_delete(eit);
    }

    /* Bad arguments. */
    {
        dvbpsi_eit_t *eit;
        dvbpsi_eit_event_t *ev = new_event(&eit);
        CHECK(!dvbpsi_eit_event_set_extended_text(NULL, LANG_ENG, NULL, 0,
                NULL, 0, &err));
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_ARGUMENT);
        dvbpsi_extended_text_item_t item[1] = {
            { (const uint8_t *)"x", 1, NULL, 0 }
        };
        CHECK(!dvbpsi_eit_event_set_extended_text(ev, LANG_ENG, NULL, 0,
                item, -1, &err));
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_ARGUMENT);
        CHECK(ev->p_first_descriptor == NULL);
        dvbpsi_eit_delete(eit);
    }
}

/*****************************************************************************
 * Reader error cases: malformed series must never return partial text.
 *****************************************************************************/
static dvbpsi_descriptor_t *add_raw_4e(dvbpsi_eit_event_t *ev,
                                       const uint8_t *payload, int len)
{
    return dvbpsi_eit_event_descriptor_add(ev, 0x4e, (uint8_t)len,
                                           (uint8_t *)payload);
}

static void test_read_errors(void)
{
    dvbpsi_eit_extended_error_t err;

    /* Gap: numbers 0 and 2, both claiming last == 1. */
    {
        dvbpsi_eit_t *eit;
        dvbpsi_eit_event_t *ev = new_event(&eit);
        uint8_t p0[] = { 0x01, 'e','n','g', 0, 0 };
        uint8_t p2[] = { 0x21, 'e','n','g', 0, 0 };
        CHECK(add_raw_4e(ev, p0, sizeof(p0)));
        CHECK(add_raw_4e(ev, p2, sizeof(p2)));
        CHECK(dvbpsi_eit_event_get_extended_text(ev, LANG_ENG, &err) == NULL);
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_MALFORMED);
        dvbpsi_eit_delete(eit);
    }

    /* Duplicate descriptor number. */
    {
        dvbpsi_eit_t *eit;
        dvbpsi_eit_event_t *ev = new_event(&eit);
        uint8_t pa[] = { 0x01, 'e','n','g', 0, 0 };
        uint8_t pb[] = { 0x01, 'e','n','g', 0, 0 };
        CHECK(add_raw_4e(ev, pa, sizeof(pa)));
        CHECK(add_raw_4e(ev, pb, sizeof(pb)));
        CHECK(dvbpsi_eit_event_get_extended_text(ev, LANG_ENG, &err) == NULL);
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_MALFORMED);
        dvbpsi_eit_delete(eit);
    }

    /* Contradictory last_descriptor_number. */
    {
        dvbpsi_eit_t *eit;
        dvbpsi_eit_event_t *ev = new_event(&eit);
        uint8_t pa[] = { 0x01, 'e','n','g', 0, 0 };
        uint8_t pb[] = { 0x10, 'e','n','g', 0, 0 };
        CHECK(add_raw_4e(ev, pa, sizeof(pa)));
        CHECK(add_raw_4e(ev, pb, sizeof(pb)));
        CHECK(dvbpsi_eit_event_get_extended_text(ev, LANG_ENG, &err) == NULL);
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_MALFORMED);
        dvbpsi_eit_delete(eit);
    }

    /* Truncated item: items_length overruns the payload. */
    {
        dvbpsi_eit_t *eit;
        dvbpsi_eit_event_t *ev = new_event(&eit);
        uint8_t p[] = { 0x00, 'e','n','g', 9, 2, 'a','b', 0 };
        CHECK(add_raw_4e(ev, p, sizeof(p)));
        CHECK(dvbpsi_eit_event_get_extended_text(ev, LANG_ENG, &err) == NULL);
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_MALFORMED);
        dvbpsi_eit_delete(eit);
    }

    /* Illegal UTF-8 behind the 0x15 selection byte. */
    {
        dvbpsi_eit_t *eit;
        dvbpsi_eit_event_t *ev = new_event(&eit);
        uint8_t p[] = { 0x00, 'e','n','g', 0, 2, 0x15, 0xff };
        CHECK(add_raw_4e(ev, p, sizeof(p)));
        CHECK(dvbpsi_eit_event_get_extended_text(ev, LANG_ENG, &err) == NULL);
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_MALFORMED);
        dvbpsi_eit_delete(eit);
    }

    /* Unsupported character table (selection byte 0x01): the high level
     * interface rejects it while the low level decoder still works. */
    {
        dvbpsi_eit_t *eit;
        dvbpsi_eit_event_t *ev = new_event(&eit);
        uint8_t p[] = { 0x00, 'e','n','g', 0, 3, 0x01, 'h', 'i' };
        dvbpsi_descriptor_t *d = add_raw_4e(ev, p, sizeof(p));
        CHECK(d != NULL);
        CHECK(dvbpsi_DecodeExtendedEventDr(d) != NULL);
        CHECK(dvbpsi_eit_event_get_extended_text(ev, LANG_ENG, &err) == NULL);
        CHECK(err == DVBPSI_EIT_EXTENDED_ERR_ENCODING);
        dvbpsi_eit_delete(eit);
    }
}

/*****************************************************************************
 * Full chain: sections_generate -> TS packet_push -> EIT callback.
 *****************************************************************************/
typedef struct
{
    bool called;
    const uint8_t *expected_text;
    int expected_text_length;
} roundtrip_ctx_t;

static void roundtrip_callback(void *data, dvbpsi_eit_t *new_eit)
{
    roundtrip_ctx_t *ctx = data;
    ctx->called = true;

    CHECK(new_eit->i_extension == 7);                 /* service id */
    CHECK(new_eit->p_first_event != NULL);
    dvbpsi_eit_event_t *ev = new_eit->p_first_event;
    CHECK(ev->i_event_id == 42);
    CHECK(ev->i_start_time == 0xea60000000ULL);
    CHECK(ev->i_duration == 0x010203);

    dvbpsi_eit_extended_error_t err;
    dvbpsi_extended_text_t *r =
            dvbpsi_eit_event_get_extended_text(ev, LANG_CHI, &err);
    CHECK(r != NULL);
    if (r)
    {
        CHECK(r->i_text_length == ctx->expected_text_length);
        CHECK(memcmp(r->p_text, ctx->expected_text,
                     (size_t)ctx->expected_text_length) == 0);
        CHECK(r->i_item_count == 2);
        if (r->i_item_count == 2)
        {
            CHECK(r->p_items[0].i_name_length == 6);
            CHECK(memcmp(r->p_items[0].p_name, "Actor1", 6) == 0);
            CHECK(r->p_items[1].i_value_length == 5);
            CHECK(memcmp(r->p_items[1].p_value, "role2", 5) == 0);
        }
        dvbpsi_extended_text_delete(r);
    }
}

static void new_subtable_cb(dvbpsi_t *h, uint8_t table_id,
                            uint16_t extension, void *data)
{
    if (table_id == 0x4e)
        CHECK(dvbpsi_eit_attach(h, table_id, extension,
                                roundtrip_callback, data));
}

static void push_sections(dvbpsi_t *h, dvbpsi_psi_section_t *sections,
                          uint16_t pid)
{
    uint8_t cc = 0;
    for (dvbpsi_psi_section_t *s = sections; s; s = s->p_next)
    {
        int total = 3 + s->i_length;
        const uint8_t *pos = s->p_data;
        bool first_packet = true;

        while (total > 0)
        {
            uint8_t pkt[188];
            memset(pkt, 0xff, sizeof(pkt));
            pkt[0] = 0x47;
            pkt[1] = (uint8_t)(((pid >> 8) & 0x1f) |
                               (first_packet ? 0x40 : 0x00));
            pkt[2] = (uint8_t)(pid & 0xff);
            pkt[3] = 0x10 | (cc & 0x0f);
            cc++;

            int off;
            int chunk;
            if (first_packet)
            {
                pkt[4] = 0x00;              /* pointer_field */
                off = 5;
                chunk = 183;
            }
            else
            {
                off = 4;
                chunk = 184;
            }
            if (chunk > total)
                chunk = total;
            memcpy(pkt + off, pos, (size_t)chunk);
            pos += chunk;
            total -= chunk;
            first_packet = false;

            CHECK(dvbpsi_packet_push(h, pkt));
        }
    }
}

static void test_full_roundtrip(void)
{
    dvbpsi_t *gen = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    CHECK(gen != NULL);
    dvbpsi_eit_t *eit =
            dvbpsi_eit_new(0x4e, 7, 1, true, 8, 9, 0, 0x4e);
    CHECK(eit != NULL);
    dvbpsi_eit_event_t *ev = dvbpsi_eit_event_add(eit, 42,
            0xea60000000ULL, 0x010203, 4, false, 0);
    CHECK(ev != NULL);

    dvbpsi_extended_text_item_t items[2] = {
        { (const uint8_t *)"Actor1", 6,
          (const uint8_t *)"role1", 5 },
        { (const uint8_t *)"Actor2", 6,
          (const uint8_t *)"role2", 5 }
    };
    dvbpsi_eit_extended_error_t err;
    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_CHI, CHI_TEXT,
            sizeof(CHI_TEXT), items, 2, &err));

    dvbpsi_psi_section_t *sections =
            dvbpsi_eit_sections_generate(gen, eit, 0x4e);
    CHECK(sections != NULL);
    CHECK(dvbpsi_ValidPSISection(sections));

    roundtrip_ctx_t ctx = {
        .called = false,
        .expected_text = CHI_TEXT,
        .expected_text_length = sizeof(CHI_TEXT)
    };

    dvbpsi_t *dec = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    CHECK(dec != NULL);
    CHECK(dvbpsi_AttachDemux(dec, new_subtable_cb, &ctx));
    push_sections(dec, sections, 0x0012);
    CHECK(ctx.called);

    dvbpsi_DetachDemux(dec);
    dvbpsi_delete(dec);
    dvbpsi_DeletePSISections(sections);
    dvbpsi_eit_delete(eit);
    dvbpsi_delete(gen);
}

/*****************************************************************************
 * Near the 4076 byte event limit: a 15 full + 1 partial descriptor series
 * must still fit, generate a valid section and read back exactly.
 *****************************************************************************/
static void test_event_size_boundary(void)
{
    dvbpsi_t *handle = dvbpsi_new(NULL, DVBPSI_MSG_NONE);
    CHECK(handle != NULL);
    dvbpsi_eit_t *eit;
    dvbpsi_eit_event_t *ev = new_event(&eit);
    dvbpsi_eit_extended_error_t err;

    /* 15 descriptors hold 15*248 = 3720 bytes; stay just under the event
     * limit with a short final fragment. */
    int len = 3720 + 100;
    uint8_t *text = malloc((size_t)len);
    CHECK(text != NULL);
    for (int i = 0; i < len; i++)
        text[i] = (uint8_t)('a' + (i % 26));   /* stays valid ASCII UTF-8 */

    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_ENG, text, len,
            NULL, 0, &err));
    int nd = count_descriptors(ev, 0x4e, LANG_ENG);
    CHECK(nd == 16);

    /* i_descriptors_length must match the actual descriptor loop. */
    int loop = 0;
    for (dvbpsi_descriptor_t *d = ev->p_first_descriptor; d; d = d->p_next)
        loop += 2 + d->i_length;
    CHECK(ev->i_descriptors_length == loop);
    CHECK(12 + loop <= 4076);

    /* Section generation still succeeds and stays one section. */
    dvbpsi_psi_section_t *s =
            dvbpsi_eit_sections_generate(handle, eit, 0x4e);
    CHECK(s != NULL && s->p_next == NULL);
    CHECK(dvbpsi_ValidPSISection(s));
    dvbpsi_DeletePSISections(s);

    dvbpsi_extended_text_t *r =
            dvbpsi_eit_event_get_extended_text(ev, LANG_ENG, &err);
    CHECK(r != NULL && r->i_text_length == len);
    if (r)
    {
        CHECK(memcmp(r->p_text, text, (size_t)len) == 0);
        dvbpsi_extended_text_delete(r);
    }

    free(text);
    dvbpsi_eit_delete(eit);
    dvbpsi_delete(handle);
}

/*****************************************************************************
 * Empty input writes no descriptor; writing empty again removes the series.
 *****************************************************************************/
static void test_empty_input(void)
{
    dvbpsi_eit_t *eit;
    dvbpsi_eit_event_t *ev = new_event(&eit);
    dvbpsi_eit_extended_error_t err;

    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_ENG, NULL, 0,
            NULL, 0, &err));
    CHECK(ev->p_first_descriptor == NULL);
    CHECK(dvbpsi_eit_event_get_extended_text(ev, LANG_ENG, &err) == NULL);
    CHECK(err == DVBPSI_EIT_EXTENDED_ERR_NOT_FOUND);

    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_ENG,
            (const uint8_t *)"temp", 4, NULL, 0, &err));
    CHECK(count_descriptors(ev, 0x4e, LANG_ENG) == 1);

    /* Replacing with empty input removes the series, leaves nothing. */
    CHECK(dvbpsi_eit_event_set_extended_text(ev, LANG_ENG, NULL, 0,
            NULL, 0, &err));
    CHECK(ev->p_first_descriptor == NULL);

    dvbpsi_eit_delete(eit);
}

int main(void)
{
    test_low_level_dr4e();
    test_basic_roundtrip();
    test_independent_ownership();
    test_long_text_no_items();
    test_items_only_and_spread();
    test_two_languages_and_replace();
    test_out_of_order_and_foreign_descriptors();
    test_stable_output();
    test_write_errors();
    test_read_errors();
    test_full_roundtrip();
    test_event_size_boundary();
    test_empty_input();

    if (g_failures != 0)
    {
        fprintf(stderr, "extended event text: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("extended event text: ok");
    return 0;
}
