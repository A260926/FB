/*
 * libxlsxwriter
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright 2014-2026, John McNamara, jmcnamara@cpan.org.
 *
 * shared_strings - A libxlsxwriter library for creating Excel XLSX
 *                  sst files.
 *
 */
#ifndef __LXW_SST_H__
#define __LXW_SST_H__

#include <string.h>
#include <stdint.h>

#include "common.h"
#include "disk_sst.h"

/* Define a tree.h RB structure for storing shared strings. */
RB_HEAD(sst_rb_tree, sst_element);

/* Define a queue.h structure for storing shared strings in insertion order. */
STAILQ_HEAD(sst_order_list, sst_element);

/* Wrapper around RB_GENERATE_STATIC from tree.h to avoid unused function
 * warnings and to avoid portability issues with the _unused attribute. */
#define LXW_RB_GENERATE_ELEMENT(name, type, field, cmp) \
    RB_GENERATE_INSERT_COLOR(name, type, field, static) \
    RB_GENERATE_INSERT(name, type, field, cmp, static)  \
    /* Add unused struct to allow adding a semicolon */ \
    struct lxw_rb_generate_element{int unused;}

/*
 * Elements of the SST table. They contain pointers to allow them to
 * be stored in a RB tree and also pointers to track the insertion order
 * in a separate list.
 */
struct sst_element {
    uint32_t index;
    char *string;
    uint8_t is_rich_string;

    STAILQ_ENTRY (sst_element) sst_order_pointers;
    RB_ENTRY (sst_element) sst_tree_pointers;
};

/*
 * Struct to represent a sst.
 */
typedef struct lxw_sst {
    FILE *file;

    uint32_t string_count;
    uint32_t unique_count;

    struct sst_order_list *order_list;
    struct sst_rb_tree *rb_tree;

    /* Non-NULL when the table is backed by temporary files on disk. */
    lxw_disk_sst *disk;
    uint8_t disk_mode;

} lxw_sst;

/* *INDENT-OFF* */
#ifdef __cplusplus
extern "C" {
#endif
/* *INDENT-ON* */

lxw_sst *lxw_sst_new(void);
void lxw_sst_free(lxw_sst *sst);
struct sst_element *lxw_get_sst_index(lxw_sst *sst, const char *string,
                                      uint8_t is_rich_string);
void lxw_sst_assemble_xml_file(lxw_sst *self);

/*
 * Switch the table to its disk backed implementation. Only valid before
 * any strings have been registered. Returns LXW_ERROR_CREATING_TMPFILE if
 * the temporary files cannot be created and
 * LXW_ERROR_PARAMETER_VALIDATION for an invalid cache budget.
 */
lxw_error lxw_sst_enable_disk_mode(lxw_sst *sst, const char *tmpdir,
                                   size_t cache_size);

/*
 * Add or find a string and return its stable index in either SST mode.
 * Returns an error code instead of a NULL element so disk write failures
 * are propagated to the worksheet write call.
 */
lxw_error lxw_sst_lookup_index(lxw_sst *sst, const char *string,
                               uint8_t is_rich_string, uint32_t *index);

/* Return the total number of string references stored in the table. */
uint32_t lxw_sst_get_string_count(lxw_sst *sst);

/* Return the number of unique strings stored in the table. */
uint32_t lxw_sst_get_unique_count(lxw_sst *sst);

/* Return the first backend error recorded, or LXW_NO_ERROR. */
lxw_error lxw_sst_get_error(lxw_sst *sst);

/* Declarations required for unit testing. */
#ifdef TESTING

STATIC void _sst_xml_declaration(lxw_sst *self);

#endif /* TESTING */

/* *INDENT-OFF* */
#ifdef __cplusplus
}
#endif
/* *INDENT-ON* */

#endif /* __LXW_SST_H__ */
