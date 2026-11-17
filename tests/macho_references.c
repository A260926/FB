#include <stdio.h>
#include <string.h>
#include "src/headerParserLib.h"
#include "task_support/check.h"
#include "task_support/fixture.h"

/*
 * Regression cases for the Mach-O indirect reference resolution
 * (HeaderData.macho_references). Groups: identity, coordinates, ownership.
 */

#define REF_DIR ".task-build/"

/* load class dylib command ids used by the fixtures */
#define LC_LOAD_DYLIB_X 0xcu
#define LC_ID_DYLIB_X 0xdu
#define LC_LOAD_WEAK_DYLIB_X 0x80000018u
#define LC_REEXPORT_DYLIB_X 0x8000001fu
#define LC_LOAD_UPWARD_DYLIB_X 0x80000023u

/* section types */
#define S_NL_PTR 0x6u
#define S_LAZY_PTR 0x7u
#define S_STUBS 0x8u

/* reference kinds and result status (mirror of HeaderData.h) */
#define KIND_UNRESOLVED 0
#define KIND_SYMBOL 1
#define KIND_LOCAL 2
#define KIND_ABS 3
#define KIND_LOCAL_ABS 4

#define STATUS_NONE 0
#define STATUS_OK 1
#define STATUS_ERROR 2

/* ------------------------------------------------------------------ */
/* fixture definitions                                                 */
/* ------------------------------------------------------------------ */

/* F1: library ordinals map to load class dylib commands in load order */
static const FixtureSectionDef f1_secs[] = {
    {"__DATA", "__nl_symbol_ptr", 0x1000, 56, 0x800, S_NL_PTR, 0, 0},
};
static const FixtureSymbolDef f1_syms[] = {
    {"_self", 0, 1, 0, 0x0000, 0},
    {"_w", 0, 1, 0, 0x0100, 0},
    {"_r", 0, 1, 0, 0x0200, 0},
    {"_u", 0, 1, 0, 0x0300, 0},
    {"_p", 0, 1, 0, 0x0400, 0},
    {"_dyn", 0, 1, 0, 0xfe00, 0},
    {"_exe", 0, 1, 0, 0xff00, 0},
};
static const uint32_t f1_ind[] = {0, 1, 2, 3, 4, 5, 6};
static const FixtureDylibDef f1_libs[] = {
    {LC_ID_DYLIB_X, "libSelf"},
    {LC_LOAD_WEAK_DYLIB_X, "libWeak"},
    {LC_REEXPORT_DYLIB_X, "libRe"},
    {LC_LOAD_UPWARD_DYLIB_X, "libUp"},
    {LC_LOAD_DYLIB_X, "libPlain"},
};
static const FixtureMachODef f1 = {
    64, 0, 6, 0x80,
    f1_secs, 1,
    f1_syms, 7,
    f1_ind, 7,
    f1_libs, 5,
    0, 0, 0, 0, 0
};

/* F1r: the same relations as F1 with reordered dylib commands and remapped ordinals */
static const FixtureSymbolDef f1r_syms[] = {
    {"_self", 0, 1, 0, 0x0000, 0},
    {"_w", 0, 1, 0, 0x0400, 0},
    {"_r", 0, 1, 0, 0x0300, 0},
    {"_u", 0, 1, 0, 0x0200, 0},
    {"_p", 0, 1, 0, 0x0100, 0},
    {"_dyn", 0, 1, 0, 0xfe00, 0},
    {"_exe", 0, 1, 0, 0xff00, 0},
};
static const FixtureDylibDef f1r_libs[] = {
    {LC_LOAD_DYLIB_X, "libPlain"},
    {LC_LOAD_UPWARD_DYLIB_X, "libUp"},
    {LC_REEXPORT_DYLIB_X, "libRe"},
    {LC_LOAD_WEAK_DYLIB_X, "libWeak"},
    {LC_ID_DYLIB_X, "libSelf"},
};
static const FixtureMachODef f1r = {
    64, 0, 6, 0x80,
    f1_secs, 1,
    f1r_syms, 7,
    f1_ind, 7,
    f1r_libs, 5,
    0, 0, 0, 0, 0
};

/* F2: INDIRECT_SYMBOL_ABS / LOCAL / LOCAL|ABS keep their state */
static const FixtureSectionDef f2_secs[] = {
    {"__DATA", "__nl_symbol_ptr", 0x1000, 24, 0x800, S_NL_PTR, 0, 0},
};
static const FixtureSymbolDef f2_syms[] = {
    {"_a", 0, 1, 0, 0, 0},
};
static const uint32_t f2_ind[] = {0x40000000u, 0xC0000000u, 0x80000000u};
static const FixtureMachODef f2 = {
    64, 0, 6, 0x80,
    f2_secs, 1,
    f2_syms, 1,
    f2_ind, 3,
    0, 0,
    0, 0, 0, 0, 0
};

/* F3: unresolvable symbol index, string offset and library ordinal */
static const FixtureSectionDef f3_secs[] = {
    {"__DATA", "__nl_symbol_ptr", 0x1000, 32, 0x800, S_NL_PTR, 0, 0},
};
static const FixtureSymbolDef f3_syms[] = {
    {"_a", 0, 1, 0, 0, 0},
    {0, 500, 1, 0, 0, 0},        /* strx beyond the string table */
    {"_b", 0, 1, 0, 0x0900, 0},  /* ordinal 9, but only one dylib */
};
static const uint32_t f3_ind[] = {7, 1, 2, 0};
static const FixtureDylibDef f3_libs[] = {
    {LC_LOAD_DYLIB_X, "libOnly"},
};
static const FixtureMachODef f3 = {
    64, 0, 6, 0x80,
    f3_secs, 1,
    f3_syms, 3,
    f3_ind, 4,
    f3_libs, 1,
    0, 0, 0, 0, 0
};

/* F4: same name symbols keep their identity and slot order */
static const FixtureSectionDef f4_secs[] = {
    {"__DATA", "__nl_symbol_ptr", 0x1000, 24, 0x800, S_NL_PTR, 0, 0},
};
static const FixtureSymbolDef f4_syms[] = {
    {"_dup", 0, 1, 0, 0, 0},
    {"_dup", 0, 1, 0, 0, 0},
};
static const uint32_t f4_ind[] = {1, 0, 1};
static const FixtureMachODef f4 = {
    64, 0, 6, 0x80,
    f4_secs, 1,
    f4_syms, 2,
    f4_ind, 3,
    0, 0,
    0, 0, 0, 0, 0
};

/* F5: 32 bit little endian, symbol stubs (width from reserved2) + pointers */
static const FixtureSectionDef f5_secs[] = {
    {"__TEXT", "__stubs", 0x2000, 18, 0x400, S_STUBS, 0, 6},
    {"__DATA", "__nl_symbol_ptr", 0x3000, 8, 0x500, S_NL_PTR, 3, 0},
};
static const FixtureSymbolDef f5_syms[] = {
    {"_x", 0, 1, 0, 0, 0},
    {"_y", 0, 1, 0, 0, 0},
};
static const uint32_t f5_ind[] = {0, 1, 0, 1, 0};
static const FixtureMachODef f5 = {
    32, 0, 6, 0x80,
    f5_secs, 2,
    f5_syms, 2,
    f5_ind, 5,
    0, 0,
    0, 0, 0, 0, 0
};

/* F6: 64 bit big endian */
static const FixtureSectionDef f6_secs[] = {
    {"__DATA", "__nl_symbol_ptr", 0x100000000ULL, 16, 0x800, S_NL_PTR, 0, 0},
};
static const FixtureSymbolDef f6_syms[] = {
    {"_be1", 0, 1, 0, 0x0100, 0},
    {"_be2", 0, 1, 0, 0x0100, 0},
};
static const uint32_t f6_ind[] = {0, 1};
static const FixtureDylibDef f6_libs[] = {
    {LC_LOAD_DYLIB_X, "libBE"},
};
static const FixtureMachODef f6 = {
    64, 1, 6, 0x80,
    f6_secs, 1,
    f6_syms, 2,
    f6_ind, 2,
    f6_libs, 1,
    0, 0, 0, 0, 0
};

/* F7: output follows the physical section order, not names */
static const FixtureSectionDef f7_secs[] = {
    {"__B", "__zz", 0x1000, 8, 0x600, S_NL_PTR, 0, 0},
    {"__A", "__aa", 0x2000, 16, 0x700, S_LAZY_PTR, 1, 0},
};
static const FixtureSymbolDef f7_syms[] = {
    {"_s", 0, 1, 0, 0, 0},
};
static const uint32_t f7_ind[] = {0, 0, 0};
static const FixtureMachODef f7 = {
    64, 0, 6, 0x80,
    f7_secs, 2,
    f7_syms, 1,
    f7_ind, 3,
    0, 0,
    0, 0, 0, 0, 0
};

/* F8: tables far away from the load command area */
static const FixtureSectionDef f8_secs[] = {
    {"__DATA", "__nl_symbol_ptr", 0x1000, 8, 0x900, S_NL_PTR, 0, 0},
};
static const FixtureSymbolDef f8_syms[] = {
    {"_far", 0, 1, 0, 0, 0},
};
static const uint32_t f8_ind[] = {0};
static const FixtureMachODef f8 = {
    64, 0, 6, 0x80,
    f8_secs, 1,
    f8_syms, 1,
    f8_ind, 1,
    0, 0,
    4000, 4048, 0, 4096, 0
};

/* F9: zero stub width is a structural error, basic info stays readable */
static const FixtureSectionDef f9_secs[] = {
    {"__TEXT", "__text", 0x1000, 16, 0x800, 0x80000400u, 0, 0},
    {"__TEXT", "__stubs", 0x2000, 12, 0x900, S_STUBS, 0, 0},
};
static const FixtureSymbolDef f9_syms[] = {
    {"_a", 0, 1, 0, 0, 0},
};
static const uint32_t f9_ind[] = {0, 0, 0};
static const FixtureMachODef f9 = {
    64, 0, 6, 0x80,
    f9_secs, 2,
    f9_syms, 1,
    f9_ind, 3,
    0, 0,
    0, 0, 0, 0, 0
};

/* F10: section size not a multiple of the slot width */
static const FixtureSectionDef f10_secs[] = {
    {"__DATA", "__nl_symbol_ptr", 0x1000, 10, 0x800, S_NL_PTR, 0, 0},
};
static const FixtureSymbolDef f10_syms[] = {
    {"_a", 0, 1, 0, 0, 0},
};
static const uint32_t f10_ind[] = {0, 0};
static const FixtureMachODef f10 = {
    64, 0, 6, 0x80,
    f10_secs, 1,
    f10_syms, 1,
    f10_ind, 2,
    0, 0,
    0, 0, 0, 0, 0
};

/* F11: indirect table has fewer entries than the sections need */
static const FixtureSectionDef f11_secs[] = {
    {"__DATA", "__nl_symbol_ptr", 0x1000, 16, 0x800, S_NL_PTR, 0, 0},
};
static const FixtureSymbolDef f11_syms[] = {
    {"_a", 0, 1, 0, 0, 0},
};
static const uint32_t f11_ind[] = {0};
static const FixtureMachODef f11 = {
    64, 0, 6, 0x80,
    f11_secs, 1,
    f11_syms, 1,
    f11_ind, 1,
    0, 0,
    0, 0, 0, 0, 0
};

/* F12: indirect table range exceeds the file */
static const FixtureSectionDef f12_secs[] = {
    {"__DATA", "__nl_symbol_ptr", 0x1000, 16, 0x800, S_NL_PTR, 0, 0},
};
static const FixtureSymbolDef f12_syms[] = {
    {"_a", 0, 1, 0, 0, 0},
};
static const uint32_t f12_ind[] = {0, 0};
static const FixtureMachODef f12 = {
    64, 0, 6, 0x80,
    f12_secs, 1,
    f12_syms, 1,
    f12_ind, 2,
    0, 0,
    0, 0, 0, 1500, 1504
};

/* F13: no indirect reference sections at all */
static const FixtureSectionDef f13_secs[] = {
    {"__TEXT", "__text", 0x1000, 16, 0x800, 0x80000400u, 0, 0},
};
static const FixtureMachODef f13 = {
    64, 0, 2, 0x80,
    f13_secs, 1,
    0, 0,
    0, 0,
    0, 0,
    0, 0, 0, 0, 0
};

/* F15: 32 bit big endian */
static const FixtureSectionDef f15_secs[] = {
    {"__DATA", "__nl_symbol_ptr", 0x4000, 8, 0x600, S_NL_PTR, 0, 0},
};
static const FixtureSymbolDef f15_syms[] = {
    {"_s1", 0, 1, 0, 0x0100, 0},
    {"_s2", 0, 1, 0, 0x0100, 0},
};
static const uint32_t f15_ind[] = {0, 1};
static const FixtureDylibDef f15_libs[] = {
    {LC_LOAD_DYLIB_X, "libBE32"},
};
static const FixtureMachODef f15 = {
    32, 1, 6, 0x80,
    f15_secs, 1,
    f15_syms, 2,
    f15_ind, 2,
    f15_libs, 1,
    0, 0, 0, 0, 0
};

/* F14: second image for the ownership cases */
static const FixtureSectionDef f14_secs[] = {
    {"__DATA", "__nl_symbol_ptr", 0x1000, 8, 0x800, S_NL_PTR, 0, 0},
};
static const FixtureSymbolDef f14_syms[] = {
    {"_beta", 0, 1, 0, 0x0100, 0},
};
static const uint32_t f14_ind[] = {0};
static const FixtureDylibDef f14_libs[] = {
    {LC_LOAD_DYLIB_X, "libZ"},
};
static const FixtureMachODef f14 = {
    64, 0, 6, 0x80,
    f14_secs, 1,
    f14_syms, 1,
    f14_ind, 1,
    f14_libs, 1,
    0, 0, 0, 0, 0
};

/* ------------------------------------------------------------------ */
/* identity                                                            */
/* ------------------------------------------------------------------ */

static int case_base_layout(void)
{
    const char *path = REF_DIR "ref-base.bin";
    HeaderData *hd;
    const MachOReferenceData *r;

    TASK_CHECK(fixture_write(path, 0) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->macho_references_status == STATUS_OK);
    TASK_CHECK(hd->macho_references_size == 2);
    TASK_CHECK(hd->macho_references != 0);

    r = &hd->macho_references[0];
    TASK_CHECK(r->kind == KIND_SYMBOL);
    TASK_CHECK(r->symbol_index == 0);
    TASK_CHECK(r->symbol_name != 0 && strcmp(r->symbol_name, "_alpha") == 0);
    TASK_CHECK(r->symbol_type == 1);
    TASK_CHECK(r->symbol_sect == 0);
    TASK_CHECK(r->symbol_desc == 0x0100);
    TASK_CHECK(r->symbol_value == 0);
    TASK_CHECK(r->library_ordinal == 1);
    TASK_CHECK(r->library_path != 0 && strcmp(r->library_path, "libAlpha.dylib") == 0);
    TASK_CHECK(r->indirect_index == 0);
    TASK_CHECK(r->indirect_value == 0);

    r = &hd->macho_references[1];
    TASK_CHECK(r->kind == KIND_LOCAL);
    TASK_CHECK(r->indirect_index == 1);
    TASK_CHECK(r->indirect_value == 0x80000000u);
    TASK_CHECK(r->symbol_name == 0);
    TASK_CHECK(r->library_path == 0);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_ordinals_map_load_order(void)
{
    const char *path = REF_DIR "ref-ordinals.bin";
    HeaderData *hd;
    const char *want_paths[7] = {0, "libWeak", "libRe", "libUp", "libPlain", 0, 0};
    const char *want_names[7] = {"_self", "_w", "_r", "_u", "_p", "_dyn", "_exe"};
    unsigned want_ord[7] = {0, 1, 2, 3, 4, 0xfe, 0xff};
    size_t i;

    TASK_CHECK(fixture_write_macho(path, 0, &f1) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->macho_references_status == STATUS_OK);
    TASK_CHECK(hd->macho_references_size == 7);

    for (i = 0; i < 7; i++)
    {
        const MachOReferenceData *r = &hd->macho_references[i];
        TASK_CHECK(r->kind == KIND_SYMBOL);
        TASK_CHECK(r->symbol_index == i);
        TASK_CHECK(r->symbol_name != 0 && strcmp(r->symbol_name, want_names[i]) == 0);
        TASK_CHECK(r->library_ordinal == want_ord[i]);
        if (want_paths[i])
            TASK_CHECK(r->library_path != 0 && strcmp(r->library_path, want_paths[i]) == 0);
        else
            TASK_CHECK(r->library_path == 0);
    }

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_abs_local_kinds(void)
{
    const char *path = REF_DIR "ref-kinds.bin";
    HeaderData *hd;
    unsigned want_kind[3] = {KIND_ABS, KIND_LOCAL_ABS, KIND_LOCAL};
    uint32_t want_value[3] = {0x40000000u, 0xC0000000u, 0x80000000u};
    size_t i;

    TASK_CHECK(fixture_write_macho(path, 0, &f2) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->macho_references_status == STATUS_OK);
    TASK_CHECK(hd->macho_references_size == 3);

    for (i = 0; i < 3; i++)
    {
        const MachOReferenceData *r = &hd->macho_references[i];
        TASK_CHECK(r->kind == want_kind[i]);
        TASK_CHECK(r->indirect_value == want_value[i]);
        TASK_CHECK(r->indirect_index == i);
        TASK_CHECK(r->symbol_name == 0);
        TASK_CHECK(r->symbol_index == 0);
        TASK_CHECK(r->library_path == 0);
    }

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_unresolved_slots(void)
{
    const char *path = REF_DIR "ref-unresolved.bin";
    HeaderData *hd;
    const MachOReferenceData *r;

    TASK_CHECK(fixture_write_macho(path, 0, &f3) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->macho_references_status == STATUS_OK);
    TASK_CHECK(hd->macho_references_size == 4);

    /* symbol index beyond the symbol table */
    r = &hd->macho_references[0];
    TASK_CHECK(r->kind == KIND_UNRESOLVED);
    TASK_CHECK(r->symbol_index == 7);
    TASK_CHECK(r->indirect_value == 7);
    TASK_CHECK(r->symbol_name == 0);

    /* string offset beyond the string table: raw nlist fields are kept */
    r = &hd->macho_references[1];
    TASK_CHECK(r->kind == KIND_UNRESOLVED);
    TASK_CHECK(r->symbol_index == 1);
    TASK_CHECK(r->symbol_type == 1);
    TASK_CHECK(r->symbol_name == 0);

    /* library ordinal without a matching dylib command */
    r = &hd->macho_references[2];
    TASK_CHECK(r->kind == KIND_UNRESOLVED);
    TASK_CHECK(r->symbol_index == 2);
    TASK_CHECK(r->library_ordinal == 9);
    TASK_CHECK(r->symbol_name == 0);
    TASK_CHECK(r->library_path == 0);

    /* control slot: resolves normally */
    r = &hd->macho_references[3];
    TASK_CHECK(r->kind == KIND_SYMBOL);
    TASK_CHECK(r->symbol_name != 0 && strcmp(r->symbol_name, "_a") == 0);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_same_name_symbols(void)
{
    const char *path = REF_DIR "ref-dup.bin";
    HeaderData *hd;
    unsigned want_index[3] = {1, 0, 1};
    size_t i;

    TASK_CHECK(fixture_write_macho(path, 0, &f4) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->macho_references_status == STATUS_OK);
    TASK_CHECK(hd->macho_references_size == 3);

    for (i = 0; i < 3; i++)
    {
        const MachOReferenceData *r = &hd->macho_references[i];
        TASK_CHECK(r->kind == KIND_SYMBOL);
        TASK_CHECK(r->symbol_index == want_index[i]);
        TASK_CHECK(r->symbol_name != 0 && strcmp(r->symbol_name, "_dup") == 0);
        TASK_CHECK(r->slot_index == i);
    }

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_reordered_commands_same_relations(void)
{
    const char *path_a = REF_DIR "ref-order-a.bin";
    const char *path_b = REF_DIR "ref-order-b.bin";
    HeaderData *hd1;
    HeaderData *hd2;
    size_t i;

    TASK_CHECK(fixture_write_macho(path_a, 0, &f1) == 0);
    TASK_CHECK(fixture_write_macho(path_b, 0, &f1r) == 0);
    hd1 = getBasicHeaderParserInfo(path_a, 0, 0);
    hd2 = getBasicHeaderParserInfo(path_b, 0, 0);
    TASK_CHECK(hd1 != 0 && hd2 != 0);

    TASK_CHECK(hd1->macho_references_size == 7);
    TASK_CHECK(hd2->macho_references_size == 7);

    /* reordered commands describing the same relations resolve identically */
    for (i = 0; i < 7; i++)
    {
        const MachOReferenceData *a = &hd1->macho_references[i];
        const MachOReferenceData *b = &hd2->macho_references[i];
        TASK_CHECK(a->kind == b->kind);
        TASK_CHECK(strcmp(a->symbol_name, b->symbol_name) == 0);
        if (a->library_path == 0 || b->library_path == 0)
            TASK_CHECK(a->library_path == b->library_path);
        else
            TASK_CHECK(strcmp(a->library_path, b->library_path) == 0);
    }

    freeHeaderData(hd1);
    freeHeaderData(hd2);
    remove(path_a);
    remove(path_b);
    return 0;
}

/* ------------------------------------------------------------------ */
/* coordinates                                                         */
/* ------------------------------------------------------------------ */

static int case_slot_coordinates_64(void)
{
    const char *path = REF_DIR "ref-coords.bin";
    HeaderData *hd;
    const MachOReferenceData *r;

    TASK_CHECK(fixture_write(path, 0) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->macho_references_size == 2);

    r = &hd->macho_references[0];
    TASK_CHECK(r->segment_index == 0);
    TASK_CHECK(r->section_index == 1);
    TASK_CHECK(r->segment_name != 0 && strcmp(r->segment_name, "__DATA") == 0);
    TASK_CHECK(r->section_name != 0 && strcmp(r->section_name, "__nl_symbol_ptr") == 0);
    TASK_CHECK(r->section_type == S_NL_PTR);
    TASK_CHECK(r->slot_index == 0);
    TASK_CHECK(r->slot_address == 0x100000420ULL);
    TASK_CHECK(r->slot_image_offset == 1056);
    TASK_CHECK(r->slot_file_offset == 1056);

    r = &hd->macho_references[1];
    TASK_CHECK(r->slot_index == 1);
    TASK_CHECK(r->slot_address == 0x100000428ULL);
    TASK_CHECK(r->slot_image_offset == 1064);
    TASK_CHECK(r->slot_file_offset == 1064);
    TASK_CHECK(r->section_index == 1);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_prefix_start_offset(void)
{
    const char *path = REF_DIR "ref-prefix.bin";
    HeaderData *hd;
    const MachOReferenceData *r;

    TASK_CHECK(fixture_write(path, 100) == 0);
    hd = getBasicHeaderParserInfo(path, 100, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->macho_references_status == STATUS_OK);
    TASK_CHECK(hd->macho_references_size == 2);

    /* table offsets are relative to the image start, file offsets to the file */
    r = &hd->macho_references[0];
    TASK_CHECK(r->slot_image_offset == 1056);
    TASK_CHECK(r->slot_file_offset == 1156);
    TASK_CHECK(r->symbol_name != 0 && strcmp(r->symbol_name, "_alpha") == 0);

    r = &hd->macho_references[1];
    TASK_CHECK(r->slot_image_offset == 1064);
    TASK_CHECK(r->slot_file_offset == 1164);
    TASK_CHECK(r->kind == KIND_LOCAL);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_stubs32_widths(void)
{
    const char *path = REF_DIR "ref-stubs32.bin";
    HeaderData *hd;
    const uint64_t want_addr[5] = {0x2000, 0x2006, 0x200c, 0x3000, 0x3004};
    const uint64_t want_off[5] = {0x400, 0x406, 0x40c, 0x500, 0x504};
    const uint64_t want_ind[5] = {0, 1, 2, 3, 4};
    const char *want_name[5] = {"_x", "_y", "_x", "_y", "_x"};
    size_t i;

    TASK_CHECK(fixture_write_macho(path, 0, &f5) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->h_bitness == 32);
    TASK_CHECK(hd->macho_references_status == STATUS_OK);
    TASK_CHECK(hd->macho_references_size == 5);

    for (i = 0; i < 5; i++)
    {
        const MachOReferenceData *r = &hd->macho_references[i];
        TASK_CHECK(r->slot_address == want_addr[i]);
        TASK_CHECK(r->slot_image_offset == want_off[i]);
        TASK_CHECK(r->slot_file_offset == want_off[i]);
        TASK_CHECK(r->indirect_index == want_ind[i]);
        TASK_CHECK(r->kind == KIND_SYMBOL);
        TASK_CHECK(r->symbol_name != 0 && strcmp(r->symbol_name, want_name[i]) == 0);
    }

    /* stub slots come from the first section, pointer slots from the second */
    TASK_CHECK(hd->macho_references[0].section_type == S_STUBS);
    TASK_CHECK(hd->macho_references[0].section_index == 0);
    TASK_CHECK(hd->macho_references[3].section_type == S_NL_PTR);
    TASK_CHECK(hd->macho_references[3].section_index == 1);
    TASK_CHECK(hd->macho_references[3].segment_index == 1);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_big_endian_64(void)
{
    const char *path = REF_DIR "ref-be64.bin";
    HeaderData *hd;
    const MachOReferenceData *r;

    TASK_CHECK(fixture_write_macho(path, 0, &f6) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->endian == 2); /* ENDIAN_BIG */
    TASK_CHECK(hd->macho_references_status == STATUS_OK);
    TASK_CHECK(hd->macho_references_size == 2);

    r = &hd->macho_references[0];
    TASK_CHECK(r->kind == KIND_SYMBOL);
    TASK_CHECK(r->symbol_name != 0 && strcmp(r->symbol_name, "_be1") == 0);
    TASK_CHECK(r->slot_address == 0x100000000ULL);
    TASK_CHECK(r->library_ordinal == 1);
    TASK_CHECK(r->library_path != 0 && strcmp(r->library_path, "libBE") == 0);

    r = &hd->macho_references[1];
    TASK_CHECK(r->symbol_name != 0 && strcmp(r->symbol_name, "_be2") == 0);
    TASK_CHECK(r->slot_address == 0x100000008ULL);
    TASK_CHECK(r->slot_image_offset == 0x808);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_big_endian_32(void)
{
    const char *path = REF_DIR "ref-be32.bin";
    HeaderData *hd;
    const MachOReferenceData *r;

    TASK_CHECK(fixture_write_macho(path, 0, &f15) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->h_bitness == 32);
    TASK_CHECK(hd->endian == 2); /* ENDIAN_BIG */
    TASK_CHECK(hd->macho_references_status == STATUS_OK);
    TASK_CHECK(hd->macho_references_size == 2);

    r = &hd->macho_references[0];
    TASK_CHECK(r->kind == KIND_SYMBOL);
    TASK_CHECK(r->symbol_name != 0 && strcmp(r->symbol_name, "_s1") == 0);
    TASK_CHECK(r->slot_address == 0x4000);
    TASK_CHECK(r->slot_image_offset == 0x600);
    TASK_CHECK(r->library_path != 0 && strcmp(r->library_path, "libBE32") == 0);

    r = &hd->macho_references[1];
    TASK_CHECK(r->symbol_name != 0 && strcmp(r->symbol_name, "_s2") == 0);
    TASK_CHECK(r->slot_address == 0x4004);
    TASK_CHECK(r->slot_image_offset == 0x604);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_physical_section_order(void)
{
    const char *path = REF_DIR "ref-order.bin";
    HeaderData *hd;

    TASK_CHECK(fixture_write_macho(path, 0, &f7) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->macho_references_status == STATUS_OK);
    TASK_CHECK(hd->macho_references_size == 3);

    /* physical order wins over alphabetical order: __zz before __aa */
    TASK_CHECK(strcmp(hd->macho_references[0].section_name, "__zz") == 0);
    TASK_CHECK(hd->macho_references[0].section_index == 0);
    TASK_CHECK(hd->macho_references[0].segment_index == 0);
    TASK_CHECK(hd->macho_references[0].slot_index == 0);
    TASK_CHECK(hd->macho_references[0].section_type == S_NL_PTR);

    TASK_CHECK(strcmp(hd->macho_references[1].section_name, "__aa") == 0);
    TASK_CHECK(hd->macho_references[1].section_index == 1);
    TASK_CHECK(hd->macho_references[1].segment_index == 1);
    TASK_CHECK(hd->macho_references[1].slot_index == 0);
    TASK_CHECK(hd->macho_references[1].section_type == S_LAZY_PTR);

    TASK_CHECK(strcmp(hd->macho_references[2].section_name, "__aa") == 0);
    TASK_CHECK(hd->macho_references[2].slot_index == 1);
    TASK_CHECK(hd->macho_references[2].indirect_index == 2);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_tables_far_from_commands(void)
{
    const char *path = REF_DIR "ref-far.bin";
    HeaderData *hd;
    const MachOReferenceData *r;

    TASK_CHECK(fixture_write_macho(path, 0, &f8) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->macho_references_status == STATUS_OK);
    TASK_CHECK(hd->macho_references_size == 1);

    r = &hd->macho_references[0];
    TASK_CHECK(r->kind == KIND_SYMBOL);
    TASK_CHECK(r->symbol_name != 0 && strcmp(r->symbol_name, "_far") == 0);
    TASK_CHECK(r->slot_image_offset == 0x900);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

static int case_structural_errors(void)
{
    const char *path = REF_DIR "ref-err.bin";
    HeaderData *hd;
    const FixtureMachODef *defs[4] = {&f9, &f10, &f11, &f12};
    size_t i;

    for (i = 0; i < 4; i++)
    {
        TASK_CHECK(fixture_write_macho(path, 0, defs[i]) == 0);
        hd = getBasicHeaderParserInfo(path, 0, 0);
        TASK_CHECK(hd != 0);

        /* the incomplete table is dropped and marked as error */
        TASK_CHECK(hd->macho_references_status == STATUS_ERROR);
        TASK_CHECK(hd->macho_references_size == 0);
        TASK_CHECK(hd->macho_references == 0);

        /* basic header info is still readable */
        TASK_CHECK(hd->headertype == 11); /* HEADER_TYPE_MACH_O */
        TASK_CHECK(hd->h_bitness == 64);

        freeHeaderData(hd);
    }

    /* the error fixture with a code section keeps its code region */
    TASK_CHECK(fixture_write_macho(path, 0, &f9) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);
    TASK_CHECK(hd->code_regions_size == 1);
    TASK_CHECK(hd->code_regions[0].start == 0x800);
    TASK_CHECK(hd->code_regions[0].end == 0x810);
    freeHeaderData(hd);

    remove(path);
    return 0;
}

static int case_no_indirect_sections(void)
{
    const char *path = REF_DIR "ref-none.bin";
    HeaderData *hd;

    TASK_CHECK(fixture_write_macho(path, 0, &f13) == 0);
    hd = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd != 0);

    TASK_CHECK(hd->macho_references_status == STATUS_NONE);
    TASK_CHECK(hd->macho_references_size == 0);
    TASK_CHECK(hd->macho_references == 0);
    TASK_CHECK(hd->code_regions_size == 1);

    freeHeaderData(hd);
    remove(path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* ownership                                                           */
/* ------------------------------------------------------------------ */

static int case_results_survive_file_removal_and_reparse(void)
{
    const char *path = REF_DIR "ref-life.bin";
    HeaderData *hd1;
    HeaderData *hd2;

    TASK_CHECK(fixture_write(path, 0) == 0);
    hd1 = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd1 != 0);
    TASK_CHECK(hd1->macho_references_size == 2);

    /* the source file is deleted and replaced by another image */
    TASK_CHECK(remove(path) == 0);
    TASK_CHECK(fixture_write_macho(path, 0, &f14) == 0);
    hd2 = getBasicHeaderParserInfo(path, 0, 0);
    TASK_CHECK(hd2 != 0);

    /* the first result is unchanged */
    TASK_CHECK(hd1->macho_references_status == STATUS_OK);
    TASK_CHECK(hd1->macho_references_size == 2);
    TASK_CHECK(strcmp(hd1->macho_references[0].symbol_name, "_alpha") == 0);
    TASK_CHECK(hd1->macho_references[0].library_ordinal == 1);
    TASK_CHECK(strcmp(hd1->macho_references[0].library_path, "libAlpha.dylib") == 0);
    TASK_CHECK(hd1->macho_references[1].kind == KIND_LOCAL);
    TASK_CHECK(hd1->macho_references[1].indirect_value == 0x80000000u);
    TASK_CHECK(strcmp(hd1->macho_references[0].section_name, "__nl_symbol_ptr") == 0);

    /* the second result carries its own data */
    TASK_CHECK(hd2->macho_references_size == 1);
    TASK_CHECK(strcmp(hd2->macho_references[0].symbol_name, "_beta") == 0);
    TASK_CHECK(hd2->macho_references[0].library_path != 0);
    TASK_CHECK(strcmp(hd2->macho_references[0].library_path, "libZ") == 0);

    /* freeing the first object leaves the second one intact */
    freeHeaderData(hd1);
    TASK_CHECK(hd2->macho_references_size == 1);
    TASK_CHECK(strcmp(hd2->macho_references[0].symbol_name, "_beta") == 0);
    TASK_CHECK(strcmp(hd2->macho_references[0].library_path, "libZ") == 0);
    freeHeaderData(hd2);

    remove(path);
    return 0;
}

static int case_two_objects_independent(void)
{
    const char *path_a = REF_DIR "ref-life-a.bin";
    const char *path_b = REF_DIR "ref-life-b.bin";
    HeaderData *hd1;
    HeaderData *hd2;

    TASK_CHECK(fixture_write(path_a, 0) == 0);
    TASK_CHECK(fixture_write_macho(path_b, 0, &f14) == 0);

    hd1 = getBasicHeaderParserInfo(path_a, 0, 0);
    hd2 = getBasicHeaderParserInfo(path_b, 0, 0);
    TASK_CHECK(hd1 != 0 && hd2 != 0);

    /* both objects are alive and keep their own strings */
    TASK_CHECK(hd1->macho_references_size == 2);
    TASK_CHECK(hd2->macho_references_size == 1);
    TASK_CHECK(strcmp(hd1->macho_references[0].symbol_name, "_alpha") == 0);
    TASK_CHECK(strcmp(hd2->macho_references[0].symbol_name, "_beta") == 0);
    TASK_CHECK(hd1->macho_references[0].symbol_name != hd2->macho_references[0].symbol_name);
    TASK_CHECK(hd1->macho_references[0].section_name != hd2->macho_references[0].section_name);

    freeHeaderData(hd1);
    TASK_CHECK(strcmp(hd2->macho_references[0].symbol_name, "_beta") == 0);
    TASK_CHECK(strcmp(hd2->macho_references[0].library_path, "libZ") == 0);
    freeHeaderData(hd2);

    remove(path_a);
    remove(path_b);
    return 0;
}

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"identity", "base_layout", case_base_layout},
    {"identity", "ordinals_map_load_order", case_ordinals_map_load_order},
    {"identity", "abs_local_kinds", case_abs_local_kinds},
    {"identity", "unresolved_slots", case_unresolved_slots},
    {"identity", "same_name_symbols", case_same_name_symbols},
    {"identity", "reordered_commands_same_relations", case_reordered_commands_same_relations},
    {"coordinates", "slot_coordinates_64", case_slot_coordinates_64},
    {"coordinates", "prefix_start_offset", case_prefix_start_offset},
    {"coordinates", "stubs32_widths", case_stubs32_widths},
    {"coordinates", "big_endian_64", case_big_endian_64},
    {"coordinates", "big_endian_32", case_big_endian_32},
    {"coordinates", "physical_section_order", case_physical_section_order},
    {"coordinates", "tables_far_from_commands", case_tables_far_from_commands},
    {"coordinates", "structural_errors", case_structural_errors},
    {"coordinates", "no_indirect_sections", case_no_indirect_sections},
    {"ownership", "results_survive_file_removal_and_reparse", case_results_survive_file_removal_and_reparse},
    {"ownership", "two_objects_independent", case_two_objects_independent},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
