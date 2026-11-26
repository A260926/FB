/*
 * Mach-O indirect symbol reference regressions.
 *
 * Three driver groups:
 *   identity     - which symbol / nlist / library each relation is
 *   coordinates  - physical section coordinates and byte/address units
 *   ownership    - result and string lifetime across files and objects
 *
 * All fixtures are inert synthetic byte layouts produced by
 * task_support/fixture.h; no dependent library is opened and no image is
 * loaded or executed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "task_support/check.h"
#include "src/headerParserLib.h"
#include "task_support/fixture.h"

static int ref_streq(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return a == b;
    return strcmp(a, b) == 0;
}

static const char *ref_tmp(char *buf, size_t n, const char *name)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0')
        dir = ".task-build";
    snprintf(buf, n, "%s/%s", dir, name);
    return buf;
}

/* Build cfg, write it and optionally keep the built image descriptors. */
static int ref_write(const char *path, size_t prefix, const mfix_config *cfg,
                     mfix_image *keep)
{
    mfix_image im;
    if (mfix_build(cfg, &im) != 0)
        return 1;
    if (mfix_write(path, prefix, &im) != 0) {
        mfix_free(&im);
        return 2;
    }
    if (keep != NULL)
        *keep = im;
    else
        mfix_free(&im);
    return 0;
}

/* =================================================================== */
/* identity                                                            */
/* =================================================================== */

static int case_baseline_fixture_identity(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_baseline.bin");

    TASK_CHECK(fixture_write(path, 0) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->headertype == HEADER_TYPE_MACH_O);
    TASK_CHECK(hd->macho_references_error == MACHO_REFERENCES_OK);
    TASK_CHECK(hd->macho_references_size == 2);

    MachOReferenceData *r0 = &hd->macho_references[0];
    TASK_CHECK(r0->status == MACHO_REFERENCE_RESOLVED);
    TASK_CHECK(r0->indirect_index == 0);
    TASK_CHECK(r0->indirect_value == 0);
    TASK_CHECK(r0->nlist_index == 0);
    TASK_CHECK(ref_streq(r0->symbol_name, "_alpha"));
    TASK_CHECK(r0->n_type == 0x01);
    TASK_CHECK(r0->n_sect == 0);
    TASK_CHECK(r0->n_desc == 0x0100);
    TASK_CHECK(r0->n_value == 0);
    TASK_CHECK(r0->library_ordinal == 1);
    TASK_CHECK(r0->library_kind == MACHO_LIBRARY_ORDINAL_NORMAL);
    TASK_CHECK(ref_streq(r0->library_path, "libAlpha.dylib"));

    MachOReferenceData *r1 = &hd->macho_references[1];
    TASK_CHECK(r1->status == MACHO_REFERENCE_LOCAL);
    TASK_CHECK(r1->indirect_index == 1);
    TASK_CHECK(r1->indirect_value == 0x80000000u);
    TASK_CHECK(r1->symbol_name == NULL);
    TASK_CHECK(r1->library_ordinal == -1);
    TASK_CHECK(r1->library_path == NULL);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_duplicate_names_identity(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_dup.bin");

    /* slot 0 -> nlist 0 "_dup", slot 1 -> nlist 1 "_dup" (different
     * nlist identity), slot 2 -> nlist 0 again. */
    uint32_t ind[] = { 0, 1, 0 };
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .size = 24, .ind = ind, .ind_n = 3 }
    };
    mfix_nlist nls[] = {
        { .name = "_dup", .n_type = 1, .n_desc = 1u << 8, .n_value = 0x1111 },
        { .name = "_dup", .n_type = 1, .n_desc = 2u << 8, .n_value = 0x2222 }
    };
    mfix_dylib dyls[] = {
        { MFIX_LC_LOAD_DYLIB, "one.dylib" },
        { MFIX_LC_LOAD_DYLIB, "two.dylib" }
    };
    mfix_config cfg = {
        .bits = 64, .secs = secs, .nsec = 1,
        .nls = nls, .nnl = 2, .dylibs = dyls, .ndylib = 2
    };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->macho_references_size == 3);

    MachOReferenceData *a = &hd->macho_references[0];
    MachOReferenceData *b = &hd->macho_references[1];
    MachOReferenceData *c = &hd->macho_references[2];

    TASK_CHECK(a->slot_index == 0 && b->slot_index == 1 && c->slot_index == 2);
    TASK_CHECK(a->nlist_index == 0);
    TASK_CHECK(b->nlist_index == 1);
    TASK_CHECK(c->nlist_index == 0);
    TASK_CHECK(ref_streq(a->symbol_name, "_dup"));
    TASK_CHECK(ref_streq(b->symbol_name, "_dup"));
    TASK_CHECK(ref_streq(c->symbol_name, "_dup"));
    /* same name, distinct nlist identity keeps distinct values/libraries */
    TASK_CHECK(a->n_value == 0x1111);
    TASK_CHECK(b->n_value == 0x2222);
    TASK_CHECK(a->library_ordinal == 1);
    TASK_CHECK(b->library_ordinal == 2);
    /* each row owns its own string copy */
    TASK_CHECK(a->symbol_name != b->symbol_name);
    TASK_CHECK(a->symbol_name != c->symbol_name);
    /* repeated nlist use still yields an independent row */
    TASK_CHECK(a != c);
    TASK_CHECK(a->image_offset != c->image_offset);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_special_indirect_states(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_special.bin");

    uint32_t ind[] = {
        0x80000000u,                 /* LOCAL */
        0x40000000u,                 /* ABS */
        0xC0000000u,                 /* LOCAL | ABS */
        0x80000123u                  /* LOCAL with low bits: state kept,
                                        raw number kept, never an index */
    };
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .size = 32, .ind = ind, .ind_n = 4 }
    };
    mfix_config cfg = { .bits = 64, .secs = secs, .nsec = 1 };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->macho_references_error == MACHO_REFERENCES_OK);
    TASK_CHECK(hd->macho_references_size == 4);

    TASK_CHECK(hd->macho_references[0].status == MACHO_REFERENCE_LOCAL);
    TASK_CHECK(hd->macho_references[1].status == MACHO_REFERENCE_ABS);
    TASK_CHECK(hd->macho_references[2].status == MACHO_REFERENCE_LOCAL_ABS);
    TASK_CHECK(hd->macho_references[3].status == MACHO_REFERENCE_LOCAL);

    TASK_CHECK(hd->macho_references[0].indirect_value == 0x80000000u);
    TASK_CHECK(hd->macho_references[1].indirect_value == 0x40000000u);
    TASK_CHECK(hd->macho_references[2].indirect_value == 0xC0000000u);
    TASK_CHECK(hd->macho_references[3].indirect_value == 0x80000123u);

    for (size_t i = 0; i < 4; ++i) {
        TASK_CHECK(hd->macho_references[i].symbol_name == NULL);
        TASK_CHECK(hd->macho_references[i].library_ordinal == -1);
        TASK_CHECK(hd->macho_references[i].library_kind == MACHO_LIBRARY_ORDINAL_NONE);
    }

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_library_ordinals(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_ord.bin");

    /* five symbols: self, dynamic lookup, executable, ordinal 1..4,
     * and an out-of-range ordinal 40. */
    uint32_t ind[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .size = 64, .ind = ind, .ind_n = 8 }
    };
    mfix_nlist nls[] = {
        { .name = "_self_", .n_type = 1, .n_desc = 0x0000 },
        { .name = "_dyld_", .n_type = 1, .n_desc = 0xFE00 },
        { .name = "_exe_",  .n_type = 1, .n_desc = 0xFF00 },
        { .name = "_one",   .n_type = 1, .n_desc = 0x0100 },
        { .name = "_two",   .n_type = 1, .n_desc = 0x0200 },
        { .name = "_three", .n_type = 1, .n_desc = 0x0300 },
        { .name = "_four",  .n_type = 1, .n_desc = 0x0400 },
        { .name = "_bad",   .n_type = 1, .n_desc = 40u << 8 }
    };
    /* ID dylib sits between load commands and must not take an ordinal. */
    mfix_dylib dyls[] = {
        { MFIX_LC_LOAD_DYLIB,     "one.dylib" },
        { MFIX_LC_ID_DYLIB,       "ignored-id.dylib" },
        { MFIX_LC_LOAD_WEAK,      "two.dylib" },
        { MFIX_LC_REEXPORT,       "three.dylib" },
        { MFIX_LC_UPWARD,         "four.dylib" }
    };
    mfix_config cfg = {
        .bits = 64, .secs = secs, .nsec = 1,
        .nls = nls, .nnl = 8, .dylibs = dyls, .ndylib = 5
    };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->macho_references_size == 8);

    MachOReferenceData *r = hd->macho_references;
    TASK_CHECK(r[0].library_ordinal == 0);
    TASK_CHECK(r[0].library_kind == MACHO_LIBRARY_ORDINAL_SELF);
    TASK_CHECK(r[0].status == MACHO_REFERENCE_RESOLVED);

    TASK_CHECK(r[1].library_ordinal == 0xFE);
    TASK_CHECK(r[1].library_kind == MACHO_LIBRARY_ORDINAL_DYNAMIC_LOOKUP);
    TASK_CHECK(r[1].status == MACHO_REFERENCE_RESOLVED);

    TASK_CHECK(r[2].library_ordinal == 0xFF);
    TASK_CHECK(r[2].library_kind == MACHO_LIBRARY_ORDINAL_EXECUTABLE);
    TASK_CHECK(r[2].status == MACHO_REFERENCE_RESOLVED);

    TASK_CHECK(r[3].library_ordinal == 1);
    TASK_CHECK(ref_streq(r[3].library_path, "one.dylib"));
    TASK_CHECK(r[4].library_ordinal == 2);
    TASK_CHECK(ref_streq(r[4].library_path, "two.dylib"));
    TASK_CHECK(r[5].library_ordinal == 3);
    TASK_CHECK(ref_streq(r[5].library_path, "three.dylib"));
    TASK_CHECK(r[6].library_ordinal == 4);
    TASK_CHECK(ref_streq(r[6].library_path, "four.dylib"));
    for (int i = 3; i <= 6; ++i)
        TASK_CHECK(r[i].status == MACHO_REFERENCE_RESOLVED);

    TASK_CHECK(r[7].library_ordinal == 40);
    TASK_CHECK(r[7].library_kind == MACHO_LIBRARY_ORDINAL_INVALID);
    TASK_CHECK(r[7].library_path == NULL);
    TASK_CHECK(r[7].status == MACHO_REFERENCE_UNRESOLVED);
    /* the symbol name itself is still real */
    TASK_CHECK(ref_streq(r[7].symbol_name, "_bad"));

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_unresolved_keeps_slots(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_unres.bin");

    /* slot 0: nlist index 7 past nsyms
     * slot 1: nlist ok but string offset bogus
     * slot 2: name ok but library ordinal dangling */
    uint32_t ind[] = { 7, 0, 1 };
    mfix_section secs[] = {
        { .sect = "__la_symbol_ptr", .seg = "__DATA", .type = 7,
          .size = 24, .ind = ind, .ind_n = 3 }
    };
    mfix_nlist nls[] = {
        { .name = NULL, .strx_set = 1, .strx = 0x0000F000u,
          .n_type = 0x12, .n_sect = 9, .n_desc = 0x0304, .n_value = 0x9999 },
        { .name = "_known", .n_type = 1, .n_desc = 40u << 8 }
    };
    mfix_dylib dyls[] = { { MFIX_LC_LOAD_DYLIB, "only.dylib" } };
    mfix_config cfg = {
        .bits = 64, .secs = secs, .nsec = 1,
        .nls = nls, .nnl = 2, .dylibs = dyls, .ndylib = 1
    };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->macho_references_error == MACHO_REFERENCES_OK);
    TASK_CHECK(hd->macho_references_size == 3);

    MachOReferenceData *r0 = &hd->macho_references[0];
    TASK_CHECK(r0->status == MACHO_REFERENCE_UNRESOLVED);
    TASK_CHECK(r0->nlist_index == 7);
    TASK_CHECK(r0->indirect_value == 7);
    TASK_CHECK(r0->symbol_name == NULL);

    MachOReferenceData *r1 = &hd->macho_references[1];
    TASK_CHECK(r1->status == MACHO_REFERENCE_UNRESOLVED);
    TASK_CHECK(r1->nlist_index == 0);
    TASK_CHECK(r1->symbol_name == NULL); /* no fabricated name */
    TASK_CHECK(r1->n_type == 0x12);      /* raw nlist fields retained */
    TASK_CHECK(r1->n_sect == 9);
    TASK_CHECK(r1->n_desc == 0x0304);
    TASK_CHECK(r1->n_value == 0x9999);

    MachOReferenceData *r2 = &hd->macho_references[2];
    TASK_CHECK(r2->status == MACHO_REFERENCE_UNRESOLVED);
    TASK_CHECK(ref_streq(r2->symbol_name, "_known"));
    TASK_CHECK(r2->library_ordinal == 40);
    TASK_CHECK(r2->library_kind == MACHO_LIBRARY_ORDINAL_INVALID);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

/* =================================================================== */
/* coordinates                                                         */
/* =================================================================== */

static int case_baseline_coordinates(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_coords_base.bin");

    TASK_CHECK(fixture_write(path, 0) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->macho_references_size == 2);

    MachOReferenceData *r0 = &hd->macho_references[0];
    TASK_CHECK(r0->segment_index == 1);
    TASK_CHECK(ref_streq(r0->segment_name, "__DATA"));
    TASK_CHECK(r0->section_index == 2); /* __text is #1 */
    TASK_CHECK(r0->section_index_in_segment == 2);
    TASK_CHECK(ref_streq(r0->section_name, "__nl_symbol_ptr"));
    TASK_CHECK(r0->section_type == 6);
    TASK_CHECK(r0->slot_index == 0);
    TASK_CHECK(r0->address == 0x100000420ULL);
    TASK_CHECK(r0->image_offset == 1056);
    TASK_CHECK(r0->file_offset == 1056);

    MachOReferenceData *r1 = &hd->macho_references[1];
    TASK_CHECK(r1->slot_index == 1);
    TASK_CHECK(r1->address == 0x100000428ULL);
    TASK_CHECK(r1->image_offset == 1064);
    TASK_CHECK(r1->file_offset == 1064);
    TASK_CHECK(r1->indirect_index == 1);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_coordinates_32le(void)
{
    char path[1024];
    mfix_image im;
    ref_tmp(path, sizeof path, "mref_32le.bin");

    uint32_t ind[] = { 0, 0 };
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__IMPORT", .type = 6,
          .addr = 0x00402000u, .size = 8, .ind = ind, .ind_n = 2 }
    };
    mfix_nlist nls[] = { { .name = "_x", .n_type = 1, .n_desc = 0 } };
    mfix_config cfg = {
        .bits = 32, .big_endian = 0, .secs = secs, .nsec = 1,
        .nls = nls, .nnl = 1
    };

    TASK_CHECK(ref_write(path, 0, &cfg, &im) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->h_bitness == 32);
    TASK_CHECK(hd->endian == ENDIAN_LITTLE);
    TASK_CHECK(hd->macho_references_size == 2);

    for (uint32_t j = 0; j < 2; ++j) {
        MachOReferenceData *r = &hd->macho_references[j];
        TASK_CHECK(r->address == 0x00402000u + 4ull * j);
        TASK_CHECK(r->image_offset == (uint64_t)im.sec_offset[0] + 4ull * j);
        TASK_CHECK(r->file_offset == r->image_offset);
        TASK_CHECK(r->indirect_index == im.sec_reserved1[0] + j);
    }
    TASK_CHECK(hd->macho_references[1].image_offset ==
               (uint64_t)im.sec_offset[0] + 4);

    mfix_free(&im);
    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_coordinates_32be(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_32be.bin");

    uint32_t ind[] = { 0 };
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .addr = 0xAABB0000u, .size = 4, .ind = ind, .ind_n = 1 }
    };
    mfix_nlist nls[] = {
        { .name = "_be32", .n_type = 0x11, .n_sect = 7,
          .n_desc = 0x1234, .n_value = 0xDEADBEEFu }
    };
    mfix_config cfg = {
        .bits = 32, .big_endian = 1, .secs = secs, .nsec = 1,
        .nls = nls, .nnl = 1
    };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->endian == ENDIAN_BIG);
    TASK_CHECK(hd->macho_references_size == 1);
    MachOReferenceData *r = &hd->macho_references[0];
    TASK_CHECK(r->address == 0xAABB0000u);
    TASK_CHECK(ref_streq(r->symbol_name, "_be32"));
    TASK_CHECK(r->n_type == 0x11);
    TASK_CHECK(r->n_sect == 7);
    TASK_CHECK(r->n_desc == 0x1234);
    TASK_CHECK(r->n_value == 0xDEADBEEFu);
    TASK_CHECK(r->library_ordinal == 0x12); /* 0x1234 >> 8 */

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_coordinates_64be_lazy(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_64be.bin");

    uint32_t ind[] = { 0, 0 };
    mfix_section secs[] = {
        { .sect = "__la_symbol_ptr", .seg = "__DATA", .type = 7,
          .addr = 0x123456780000ULL, .size = 16, .ind = ind, .ind_n = 2 }
    };
    mfix_nlist nls[] = {
        { .name = "_be64", .n_type = 1, .n_desc = 0x0100,
          .n_value = 0x1122334455667788ULL }
    };
    mfix_dylib dyls[] = { { MFIX_LC_LOAD_DYLIB, "be.dylib" } };
    mfix_config cfg = {
        .bits = 64, .big_endian = 1, .secs = secs, .nsec = 1,
        .nls = nls, .nnl = 1, .dylibs = dyls, .ndylib = 1
    };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->endian == ENDIAN_BIG);
    TASK_CHECK(hd->macho_references_size == 2);
    TASK_CHECK(hd->macho_references[0].section_type == 7);
    TASK_CHECK(hd->macho_references[0].address == 0x123456780000ULL);
    TASK_CHECK(hd->macho_references[1].address == 0x123456780008ULL);
    TASK_CHECK(hd->macho_references[1].image_offset ==
               hd->macho_references[0].image_offset + 8);
    TASK_CHECK(hd->macho_references[0].n_value == 0x1122334455667788ULL);
    TASK_CHECK(ref_streq(hd->macho_references[0].library_path, "be.dylib"));

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_32be_stubs_and_lazy(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_32be_stub.bin");

    /* two 5 byte i386 style stubs, then two 4 byte lazy pointer slots;
     * automatic reserved1 packing gives indirect starts 0 and 2. */
    uint32_t ind_stub[] = { 0, 1 };
    uint32_t ind_lazy[] = { 0, 1 };
    mfix_section secs[] = {
        { .sect = "__stubs", .seg = "__TEXT", .type = 8,
          .addr = 0x30000000u, .size = 10, .stub_width = 5,
          .ind = ind_stub, .ind_n = 2 },
        { .sect = "__la_symbol_ptr", .seg = "__DATA", .type = 7,
          .addr = 0x40001000u, .size = 8,
          .ind = ind_lazy, .ind_n = 2 }
    };
    mfix_nlist nls[] = {
        { .name = "_s0", .n_type = 1, .n_desc = 0x0100 },
        { .name = "_s1", .n_type = 1, .n_desc = 0x0100 }
    };
    mfix_dylib dyls[] = { { MFIX_LC_LOAD_DYLIB, "mix.dylib" } };
    mfix_config cfg = {
        .bits = 32, .big_endian = 1, .secs = secs, .nsec = 2,
        .nls = nls, .nnl = 2, .dylibs = dyls, .ndylib = 1
    };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->h_bitness == 32);
    TASK_CHECK(hd->endian == ENDIAN_BIG);
    TASK_CHECK(hd->macho_references_size == 4);

    MachOReferenceData *r = hd->macho_references;
    TASK_CHECK(r[0].section_type == 8 && r[1].section_type == 8);
    TASK_CHECK(r[0].section_index == 1 && r[0].segment_index == 1);
    TASK_CHECK(r[0].address == 0x30000000u);
    TASK_CHECK(r[1].address == 0x30000005u);
    TASK_CHECK(r[0].indirect_index == 0);
    TASK_CHECK(r[1].indirect_index == 1);

    TASK_CHECK(r[2].section_type == 7 && r[3].section_type == 7);
    TASK_CHECK(r[2].section_index == 2 && r[2].segment_index == 2);
    TASK_CHECK(r[2].address == 0x40001000u);
    TASK_CHECK(r[3].address == 0x40001004u);
    TASK_CHECK(r[2].indirect_index == 2);
    TASK_CHECK(r[3].indirect_index == 3);

    for (int i = 0; i < 4; ++i) {
        TASK_CHECK(r[i].status == MACHO_REFERENCE_RESOLVED);
        TASK_CHECK(ref_streq(r[i].library_path, "mix.dylib"));
    }
    TASK_CHECK(ref_streq(r[2].symbol_name, "_s0"));
    TASK_CHECK(ref_streq(r[3].symbol_name, "_s1"));

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_stubs_width_coordinates(void)
{
    char path[1024];
    mfix_image im;
    ref_tmp(path, sizeof path, "mref_stubs.bin");

    /* two 12 byte stubs = 24 byte section */
    uint32_t ind[] = { 0, 1 };
    mfix_section secs[] = {
        { .sect = "__stubs", .seg = "__TEXT", .type = 8,
          .addr = 0x700000, .size = 24, .stub_width = 12,
          .ind = ind, .ind_n = 2 }
    };
    mfix_nlist nls[] = {
        { .name = "_a", .n_type = 1, .n_desc = 0x0100 },
        { .name = "_b", .n_type = 1, .n_desc = 0x0100 }
    };
    mfix_dylib dyls[] = { { MFIX_LC_LOAD_DYLIB, "s.dylib" } };
    mfix_config cfg = {
        .bits = 64, .secs = secs, .nsec = 1,
        .nls = nls, .nnl = 2, .dylibs = dyls, .ndylib = 1
    };

    TASK_CHECK(ref_write(path, 0, &cfg, &im) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->macho_references_size == 2);

    MachOReferenceData *r0 = &hd->macho_references[0];
    MachOReferenceData *r1 = &hd->macho_references[1];
    TASK_CHECK(r0->section_type == 8);
    TASK_CHECK(r0->address == 0x700000);
    TASK_CHECK(r1->address == 0x70000C);
    TASK_CHECK(r0->image_offset == (uint64_t)im.sec_offset[0]);
    TASK_CHECK(r1->image_offset == (uint64_t)im.sec_offset[0] + 12);
    TASK_CHECK(r0->file_offset == r0->image_offset);
    TASK_CHECK(r1->indirect_index == im.sec_reserved1[0] + 1);
    TASK_CHECK(ref_streq(r1->symbol_name, "_b"));

    mfix_free(&im);
    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_physical_section_order(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_order.bin");

    /* __TEXT segment: regular #1 then __stubs #2
     * __DATA segment: __nl_symbol_ptr #3 and __la_symbol_ptr #4 */
    uint32_t ind_stub[] = { 0 };
    uint32_t ind_nl[] = { 0 };
    uint32_t ind_la[] = { 0 };
    mfix_section secs[] = {
        { .sect = "__text", .seg = "__TEXT", .type = 0, .size = 16,
          .raw_flags = 0x80000400u },
        { .sect = "__stubs", .seg = "__TEXT", .type = 8, .size = 6,
          .stub_width = 6, .ind = ind_stub, .ind_n = 1 },
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .size = 8, .ind = ind_nl, .ind_n = 1 },
        { .sect = "__la_symbol_ptr", .seg = "__DATA", .type = 7,
          .size = 8, .ind = ind_la, .ind_n = 1 }
    };
    mfix_nlist nls[] = { { .name = "_o", .n_type = 1, .n_desc = 0 } };
    mfix_config cfg = {
        .bits = 64, .secs = secs, .nsec = 4, .nls = nls, .nnl = 1
    };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->macho_references_size == 3);

    MachOReferenceData *r = hd->macho_references;
    TASK_CHECK(r[0].section_index == 2);
    TASK_CHECK(r[0].section_index_in_segment == 2);
    TASK_CHECK(r[0].segment_index == 1);
    TASK_CHECK(ref_streq(r[0].segment_name, "__TEXT"));
    TASK_CHECK(ref_streq(r[0].section_name, "__stubs"));

    TASK_CHECK(r[1].section_index == 3);
    TASK_CHECK(r[1].section_index_in_segment == 1);
    TASK_CHECK(r[1].segment_index == 2);
    TASK_CHECK(ref_streq(r[1].segment_name, "__DATA"));
    TASK_CHECK(ref_streq(r[1].section_name, "__nl_symbol_ptr"));

    TASK_CHECK(r[2].section_index == 4);
    TASK_CHECK(r[2].section_index_in_segment == 2);
    TASK_CHECK(r[2].segment_index == 2);
    TASK_CHECK(ref_streq(r[2].section_name, "__la_symbol_ptr"));

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_start_prefix_coordinates(void)
{
    char path[1024];
    mfix_image im;
    ref_tmp(path, sizeof path, "mref_prefix.bin");

    uint32_t ind[] = { 0 };
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .addr = 0x100009000ULL, .size = 8, .ind = ind, .ind_n = 1 }
    };
    mfix_nlist nls[] = { { .name = "_p", .n_type = 1, .n_desc = 0x0100 } };
    mfix_dylib dyls[] = { { MFIX_LC_LOAD_DYLIB, "p.dylib" } };
    mfix_config cfg = {
        .bits = 64, .secs = secs, .nsec = 1,
        .nls = nls, .nnl = 1, .dylibs = dyls, .ndylib = 1
    };

    const size_t prefix = 137;
    TASK_CHECK(ref_write(path, prefix, &cfg, &im) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, prefix, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->headertype == HEADER_TYPE_MACH_O);
    TASK_CHECK(hd->macho_references_size == 1);
    MachOReferenceData *r = &hd->macho_references[0];

    /* image relative offsets ignore the prefix ... */
    TASK_CHECK(r->image_offset == (uint64_t)im.sec_offset[0]);
    /* ... whole file positions include it */
    TASK_CHECK(r->file_offset == prefix + (uint64_t)im.sec_offset[0]);
    /* and all linkedit tables still resolve through start */
    TASK_CHECK(ref_streq(r->symbol_name, "_p"));
    TASK_CHECK(ref_streq(r->library_path, "p.dylib"));

    mfix_free(&im);
    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_far_tables_and_reorder(void)
{
    char pa[1024], pb[1024];
    ref_tmp(pa, sizeof pa, "mref_far_a.bin");
    ref_tmp(pb, sizeof pb, "mref_far_b.bin");

    uint32_t ind[] = { 0, 1 };
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .size = 16, .ind = ind, .ind_n = 2 }
    };
    mfix_nlist nls[] = {
        { .name = "_far0", .n_type = 1, .n_desc = 0x0100 },
        { .name = "_far1", .n_type = 1, .n_desc = 0x0200 }
    };
    mfix_dylib dyls[] = {
        { MFIX_LC_LOAD_DYLIB, "fa.dylib" },
        { MFIX_LC_LOAD_DYLIB, "fb.dylib" }
    };
    mfix_config normal = {
        .bits = 64, .far_tables = 1, .reversed_cmds = 0,
        .secs = secs, .nsec = 1, .nls = nls, .nnl = 2,
        .dylibs = dyls, .ndylib = 2
    };
    mfix_config reordered = normal;
    reordered.reversed_cmds = 1;

    mfix_image im;
    TASK_CHECK(ref_write(pa, 0, &normal, &im) == 0);
    /* the indirect table really is far past the command area */
    TASK_CHECK(im.indirectoff > 1024);
    mfix_free(&im);
    TASK_CHECK(ref_write(pb, 0, &reordered, NULL) == 0);

    HeaderData *ha = getBasicHeaderParserInfo(pa, 0, 0);
    HeaderData *hb = getBasicHeaderParserInfo(pb, 0, 0);
    TASK_CHECK(ha != NULL && hb != NULL);
    TASK_CHECK(ha->macho_references_size == 2);
    TASK_CHECK(hb->macho_references_size == ha->macho_references_size);

    for (size_t i = 0; i < ha->macho_references_size; ++i) {
        MachOReferenceData *a = &ha->macho_references[i];
        MachOReferenceData *b = &hb->macho_references[i];
        TASK_CHECK(a->segment_index == b->segment_index);
        TASK_CHECK(a->section_index == b->section_index);
        TASK_CHECK(a->slot_index == b->slot_index);
        TASK_CHECK(a->address == b->address);
        TASK_CHECK(a->image_offset == b->image_offset);
        TASK_CHECK(a->indirect_index == b->indirect_index);
        TASK_CHECK(a->indirect_value == b->indirect_value);
        TASK_CHECK(a->status == b->status);
        TASK_CHECK(a->nlist_index == b->nlist_index);
        TASK_CHECK(ref_streq(a->symbol_name, b->symbol_name));
        TASK_CHECK(a->n_desc == b->n_desc);
        TASK_CHECK(a->library_ordinal == b->library_ordinal);
        TASK_CHECK(ref_streq(a->library_path, b->library_path));
    }
    TASK_CHECK(ref_streq(ha->macho_references[0].symbol_name, "_far0"));
    TASK_CHECK(ref_streq(ha->macho_references[1].library_path, "fb.dylib"));

    freeHeaderData(ha);
    freeHeaderData(hb);
    remove(pa);
    remove(pb);
    return 0;
}

static int case_no_reference_sections_empty(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_empty.bin");

    mfix_section secs[] = {
        { .sect = "__text", .seg = "__TEXT", .type = 0, .size = 16,
          .raw_flags = 0x80000400u }
    };
    mfix_config cfg = { .bits = 64, .secs = secs, .nsec = 1 };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->headertype == HEADER_TYPE_MACH_O);
    TASK_CHECK(hd->macho_references_error == MACHO_REFERENCES_OK);
    TASK_CHECK(hd->macho_references_size == 0);
    TASK_CHECK(hd->macho_references == NULL);
    /* existing code region behaviour is unchanged */
    TASK_CHECK(hd->code_regions_size == 1);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_error_zero_stub_width(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_err_width.bin");

    mfix_section secs[] = {
        { .sect = "__stubs", .seg = "__TEXT", .type = 8,
          .size = 16, .stub_width = 0 }
    };
    mfix_config cfg = { .bits = 64, .secs = secs, .nsec = 1 };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->headertype == HEADER_TYPE_MACH_O); /* basic header ok */
    TASK_CHECK(hd->h_bitness == 64);
    TASK_CHECK(hd->macho_references_error == MACHO_REFERENCES_ERR_STUB_WIDTH);
    TASK_CHECK(hd->macho_references_size == 0);
    TASK_CHECK(hd->macho_references == NULL);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_error_nonintegral_slot(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_err_align.bin");

    /* 10 bytes is not a whole number of 8 byte 64 bit pointer slots */
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6, .size = 10 }
    };
    mfix_config cfg = { .bits = 64, .secs = secs, .nsec = 1 };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->macho_references_error == MACHO_REFERENCES_ERR_SLOT_ALIGNMENT);
    TASK_CHECK(hd->macho_references_size == 0);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_error_indirect_count_short(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_err_count.bin");

    uint32_t ind[] = { 0, 0 };
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .size = 16, .ind = ind, .ind_n = 2 }
    };
    mfix_config cfg = {
        .bits = 64, .secs = secs, .nsec = 1,
        .ovmask = MFIX_OV_NIND, .ov_nind = 1 /* only one indirect word */
    };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->macho_references_error == MACHO_REFERENCES_ERR_INDIRECT_TABLE);
    TASK_CHECK(hd->macho_references_size == 0);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_error_indirect_bytes_truncated(void)
{
    char path[1024];
    mfix_image im;
    ref_tmp(path, sizeof path, "mref_err_trunc.bin");

    uint32_t ind[] = { 0, 0 };
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .size = 16, .ind = ind, .ind_n = 2 }
    };
    mfix_config cfg = { .bits = 64, .secs = secs, .nsec = 1 };

    /* build once to learn the table offset, then cut inside the table */
    TASK_CHECK(mfix_build(&cfg, &im) == 0);
    uint32_t table_off = im.indirectoff;
    mfix_free(&im);
    cfg.truncate_to = table_off + 5; /* second 4 byte word is incomplete */

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->headertype == HEADER_TYPE_MACH_O);
    TASK_CHECK(hd->macho_references_error == MACHO_REFERENCES_ERR_INDIRECT_TABLE);
    TASK_CHECK(hd->macho_references_size == 0);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_error_bad_reserved1(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_err_r1.bin");

    uint32_t ind[] = { 0 };
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .size = 8, .reserved1_set = 1, .reserved1 = 0xFFFFFFF0u,
          .ind = ind, .ind_n = 1 }
    };
    mfix_config cfg = { .bits = 64, .secs = secs, .nsec = 1 };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->macho_references_error == MACHO_REFERENCES_ERR_INDIRECT_TABLE);
    TASK_CHECK(hd->macho_references_size == 0);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_error_missing_dysymtab(void)
{
    char path[1024];
    ref_tmp(path, sizeof path, "mref_err_dys.bin");

    uint32_t ind[] = { 0 };
    mfix_section secs[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .size = 8, .ind = ind, .ind_n = 1 }
    };
    mfix_config cfg = {
        .bits = 64, .secs = secs, .nsec = 1, .no_dysymtab = 1
    };

    TASK_CHECK(ref_write(path, 0, &cfg, NULL) == 0);
    HeaderData *hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != NULL);
    TASK_CHECK(hd->macho_references_error == MACHO_REFERENCES_ERR_MISSING_DYSYMTAB);
    TASK_CHECK(hd->macho_references_size == 0);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

/* =================================================================== */
/* ownership                                                           */
/* =================================================================== */

static int case_results_survive_delete_and_reparse(void)
{
    char pa[1024], pb[1024], pc[1024];
    ref_tmp(pa, sizeof pa, "mref_life_a.bin");
    ref_tmp(pb, sizeof pb, "mref_life_b.bin");
    ref_tmp(pc, sizeof pc, "mref_life_c.bin");

    uint32_t ind_a[] = { 0, 1, 0x80000000u };
    mfix_section secs_a[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA_AAA", .type = 6,
          .size = 24, .ind = ind_a, .ind_n = 3 }
    };
    mfix_nlist nls_a[] = {
        { .name = "_life_alpha", .n_type = 1, .n_desc = 0x0100 },
        { .name = "_life_beta",  .n_type = 1, .n_desc = 0x0100 }
    };
    mfix_dylib dyls_a[] = { { MFIX_LC_LOAD_DYLIB, "/usr/lib/lifeA.dylib" } };
    mfix_config cfg_a = {
        .bits = 64, .secs = secs_a, .nsec = 1,
        .nls = nls_a, .nnl = 2, .dylibs = dyls_a, .ndylib = 1
    };
    TASK_CHECK(ref_write(pa, 0, &cfg_a, NULL) == 0);

    HeaderData *ha = getBasicHeaderParserInfo(pa, 0, 0);
    TASK_CHECK(ha != NULL);
    TASK_CHECK(ha->macho_references_size == 3);

    /* snapshot identity of the owned strings */
    const char *a_sec_name = ha->macho_references[0].section_name;
    const char *a_seg_name = ha->macho_references[0].segment_name;
    const char *a_sym0 = ha->macho_references[0].symbol_name;
    const char *a_sym1 = ha->macho_references[1].symbol_name;
    const char *a_lib = ha->macho_references[0].library_path;
    MachOReferenceData *a_rows = ha->macho_references;
    TASK_CHECK(ref_streq(a_sym0, "_life_alpha"));
    TASK_CHECK(ref_streq(a_sym1, "_life_beta"));
    TASK_CHECK(ref_streq(a_lib, "/usr/lib/lifeA.dylib"));
    TASK_CHECK(ref_streq(a_sec_name, "__nl_symbol_ptr"));
    TASK_CHECK(ref_streq(a_seg_name, "__DATA_AAA"));

    /* delete the file: owned result must not depend on it */
    TASK_CHECK(remove(pa) == 0);

    /* parse a different image (32 bit big endian) into another object */
    uint32_t ind_b[] = { 0 };
    mfix_section secs_b[] = {
        { .sect = "__la_symbol_ptr", .seg = "__DATA_BBB", .type = 7,
          .size = 4, .ind = ind_b, .ind_n = 1 }
    };
    mfix_nlist nls_b[] = { { .name = "_other_image_name", .n_type = 1,
                             .n_desc = 0x0100 } };
    mfix_dylib dyls_b[] = { { MFIX_LC_LOAD_DYLIB, "otherB.dylib" } };
    mfix_config cfg_b = {
        .bits = 32, .big_endian = 1, .secs = secs_b, .nsec = 1,
        .nls = nls_b, .nnl = 1, .dylibs = dyls_b, .ndylib = 1
    };
    TASK_CHECK(ref_write(pb, 0, &cfg_b, NULL) == 0);
    HeaderData *hb = getBasicHeaderParserInfo(pb, 0, 0);
    TASK_CHECK(hb != NULL);
    TASK_CHECK(hb->macho_references_size == 1);
    TASK_CHECK(ref_streq(hb->macho_references[0].symbol_name,
                         "_other_image_name"));

    /* A is completely unchanged after delete + parse of B */
    TASK_CHECK(ha->macho_references == a_rows);
    TASK_CHECK(ha->macho_references_size == 3);
    TASK_CHECK(ha->macho_references_error == MACHO_REFERENCES_OK);
    TASK_CHECK(ha->macho_references[0].symbol_name == a_sym0);
    TASK_CHECK(ha->macho_references[1].symbol_name == a_sym1);
    TASK_CHECK(ha->macho_references[0].library_path == a_lib);
    TASK_CHECK(ha->macho_references[0].section_name == a_sec_name);
    TASK_CHECK(ha->macho_references[0].segment_name == a_seg_name);
    TASK_CHECK(ref_streq(ha->macho_references[0].symbol_name, "_life_alpha"));
    TASK_CHECK(ref_streq(ha->macho_references[1].symbol_name, "_life_beta"));
    TASK_CHECK(ref_streq(ha->macho_references[0].library_path,
                         "/usr/lib/lifeA.dylib"));
    TASK_CHECK(ha->macho_references[2].status == MACHO_REFERENCE_LOCAL);

    /* overwrite the old path with a third image and parse it */
    uint32_t ind_c[] = { 0, 0 };
    mfix_section secs_c[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA_CCC", .type = 6,
          .size = 16, .ind = ind_c, .ind_n = 2 }
    };
    mfix_nlist nls_c[] = { { .name = "_third", .n_type = 1 } };
    mfix_config cfg_c = {
        .bits = 64, .secs = secs_c, .nsec = 1, .nls = nls_c, .nnl = 1
    };
    TASK_CHECK(ref_write(pc, 0, &cfg_c, NULL) == 0);
    HeaderData *hc = getBasicHeaderParserInfo(pc, 0, 0);
    TASK_CHECK(hc != NULL);
    TASK_CHECK(hc->macho_references_size == 2);
    TASK_CHECK(ref_streq(hc->macho_references[0].section_name,
                         "__nl_symbol_ptr"));

    /* free the middle object first; A and C must remain intact */
    freeHeaderData(hb);
    TASK_CHECK(ref_streq(ha->macho_references[0].symbol_name, "_life_alpha"));
    TASK_CHECK(ha->macho_references[0].symbol_name == a_sym0);
    TASK_CHECK(ref_streq(hc->macho_references[0].symbol_name, "_third"));

    freeHeaderData(hc);
    /* C freed; A still intact */
    TASK_CHECK(ha->macho_references_size == 3);
    TASK_CHECK(ref_streq(ha->macho_references[1].symbol_name, "_life_beta"));

    freeHeaderData(ha);
    remove(pb);
    remove(pc);
    return 0;
}

static int case_two_objects_coexist(void)
{
    char pa[1024], pb[1024];
    ref_tmp(pa, sizeof pa, "mref_co_a.bin");
    ref_tmp(pb, sizeof pb, "mref_co_b.bin");

    uint32_t ind_a[] = { 0 };
    mfix_section secs_a[] = {
        { .sect = "__nl_symbol_ptr", .seg = "__DATA", .type = 6,
          .size = 8, .ind = ind_a, .ind_n = 1 }
    };
    mfix_nlist nls_a[] = { { .name = "_object_a", .n_type = 1,
                             .n_desc = 0x0100 } };
    mfix_dylib dyls_a[] = { { MFIX_LC_LOAD_DYLIB, "a.dylib" } };
    mfix_config cfg_a = {
        .bits = 64, .secs = secs_a, .nsec = 1, .nls = nls_a, .nnl = 1,
        .dylibs = dyls_a, .ndylib = 1
    };

    uint32_t ind_b[] = { 0, 1 };
    mfix_section secs_b[] = {
        { .sect = "__la_symbol_ptr", .seg = "__DATA", .type = 7,
          .size = 8, .ind = ind_b, .ind_n = 2 }
    };
    mfix_nlist nls_b[] = {
        { .name = "_object_b0", .n_type = 1, .n_desc = 0x0100 },
        { .name = "_object_b1", .n_type = 1, .n_desc = 0x0100 }
    };
    mfix_config cfg_b = {
        .bits = 32, .secs = secs_b, .nsec = 1, .nls = nls_b, .nnl = 2,
        .dylibs = dyls_a, .ndylib = 1
    };

    TASK_CHECK(ref_write(pa, 7, &cfg_a, NULL) == 0);
    TASK_CHECK(ref_write(pb, 0, &cfg_b, NULL) == 0);

    HeaderData *ha = getBasicHeaderParserInfo(pa, 7, 0);
    HeaderData *hb = getBasicHeaderParserInfo(pb, 0, 0);
    TASK_CHECK(ha != NULL && hb != NULL);

    /* both objects live at the same time and keep separate identities */
    TASK_CHECK(ha != hb);
    TASK_CHECK(ha->macho_references != hb->macho_references);
    TASK_CHECK(ha->macho_references_size == 1);
    TASK_CHECK(hb->macho_references_size == 2);
    TASK_CHECK(ref_streq(ha->macho_references[0].symbol_name, "_object_a"));
    TASK_CHECK(ref_streq(hb->macho_references[0].symbol_name, "_object_b0"));
    TASK_CHECK(ref_streq(hb->macho_references[1].symbol_name, "_object_b1"));
    TASK_CHECK(!ref_streq(ha->macho_references[0].symbol_name,
                          hb->macho_references[0].symbol_name));
    TASK_CHECK(ha->macho_references[0].file_offset ==
               7 + ha->macho_references[0].image_offset);

    /* delete both files while results are alive */
    TASK_CHECK(remove(pa) == 0);
    TASK_CHECK(remove(pb) == 0);

    /* free A first, B must be untouched */
    freeHeaderData(ha);
    TASK_CHECK(hb->macho_references_size == 2);
    TASK_CHECK(ref_streq(hb->macho_references[1].symbol_name, "_object_b1"));

    freeHeaderData(hb);
    return 0;
}

const struct task_case task_cases[] = {
    { "identity", "baseline_fixture_identity", case_baseline_fixture_identity },
    { "identity", "duplicate_names_keep_identity", case_duplicate_names_identity },
    { "identity", "special_indirect_states", case_special_indirect_states },
    { "identity", "library_ordinals", case_library_ordinals },
    { "identity", "unresolved_keeps_slots", case_unresolved_keeps_slots },

    { "coordinates", "baseline_coordinates", case_baseline_coordinates },
    { "coordinates", "coordinates_32le", case_coordinates_32le },
    { "coordinates", "coordinates_32be", case_coordinates_32be },
    { "coordinates", "coordinates_64be_lazy", case_coordinates_64be_lazy },
    { "coordinates", "32be_stubs_and_lazy", case_32be_stubs_and_lazy },
    { "coordinates", "stubs_width_coordinates", case_stubs_width_coordinates },
    { "coordinates", "physical_section_order", case_physical_section_order },
    { "coordinates", "start_prefix_coordinates", case_start_prefix_coordinates },
    { "coordinates", "far_tables_and_reorder", case_far_tables_and_reorder },
    { "coordinates", "no_reference_sections_empty", case_no_reference_sections_empty },
    { "coordinates", "error_zero_stub_width", case_error_zero_stub_width },
    { "coordinates", "error_nonintegral_slot", case_error_nonintegral_slot },
    { "coordinates", "error_indirect_count_short", case_error_indirect_count_short },
    { "coordinates", "error_indirect_bytes_truncated", case_error_indirect_bytes_truncated },
    { "coordinates", "error_bad_reserved1", case_error_bad_reserved1 },
    { "coordinates", "error_missing_dysymtab", case_error_missing_dysymtab },

    { "ownership", "survive_delete_and_reparse", case_results_survive_delete_and_reparse },
    { "ownership", "two_objects_coexist", case_two_objects_coexist }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
