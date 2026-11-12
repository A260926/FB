/*
 * Target tests for BIFF8 formula dependency queries.
 *
 * Each test builds a small, fully in-memory OLE/CFB container holding a
 * BIFF8 workbook, then drives it through the public import path:
 * xls_open_buffer -> xls_getWorkSheet -> xls_parseWorkSheet ->
 * xls_get_formula_dependencies. No spreadsheet application and no external
 * sample files are required.
 */
#include "cases.hpp"
#include "xls.h"

using namespace xls;

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>

namespace {

typedef std::vector<uint8_t> Bytes;

void put16(Bytes &b, uint16_t v) {
    b.push_back((uint8_t)(v & 0xFF));
    b.push_back((uint8_t)(v >> 8));
}
void put32(Bytes &b, uint32_t v) {
    put16(b, (uint16_t)(v & 0xFFFF));
    put16(b, (uint16_t)(v >> 16));
}
void put_double(Bytes &b, double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    for (int i = 0; i < 8; ++i)
        b.push_back((uint8_t)(bits >> (8 * i)));
}

Bytes record(uint16_t id, const Bytes &body) {
    Bytes r;
    put16(r, id);
    put16(r, (uint16_t)body.size());
    r.insert(r.end(), body.begin(), body.end());
    return r;
}

/* Append one fully-built record/vector without dangling temporaries. */
void append(Bytes &dst, const Bytes &src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

/* ---- BIFF8 record builders ---- */

Bytes bof(uint16_t type) {
    Bytes b;
    put16(b, 0x0600);  // BIFF8
    put16(b, type);    // 0x0005 globals, 0x0010 worksheet
    put16(b, 0x0DB0);  // build
    put16(b, 0x07CD);  // year
    put32(b, 0);       // flags
    put32(b, 0x00E2);  // lowest supported version
    return record(0x0809, b);
}

Bytes eof_rec() { return record(0x000A, Bytes()); }

Bytes boundsheet(uint32_t stream_pos) {
    Bytes b;
    put32(b, stream_pos);
    b.push_back(0);             // visible
    b.push_back(0);             // worksheet
    put16(b, 1);                // name length
    b.push_back(0);             // compressed Latin-1
    b.push_back((uint8_t)'S');
    return record(0x0085, b);
}

Bytes row_rec(uint16_t index, uint16_t last_col) {
    Bytes b;
    put16(b, index);
    put16(b, 0);
    put16(b, (uint16_t)(last_col + 1));
    put16(b, 0x00FF);           // height
    put16(b, 0);
    put16(b, 0);
    put32(b, 0);                // flags / xf
    return record(0x0208, b);
}

Bytes number_cell(uint16_t row, uint16_t col, double value) {
    Bytes b;
    put16(b, row);
    put16(b, col);
    put16(b, 0);                // xf
    put_double(b, value);
    return record(0x0203, b);
}

/* FORMULA with an IEEE double cached result and arbitrary token blob. */
Bytes formula_double(uint16_t row, uint16_t col, double cached,
        const Bytes &tokens) {
    Bytes b;
    put16(b, row);
    put16(b, col);
    put16(b, 0);                // xf
    put_double(b, cached);      // 8-byte result; res word stays 0x0000
    put16(b, 0);                // flags
    b.insert(b.end(), 4, 0);    // chn
    put16(b, (uint16_t)tokens.size());
    b.insert(b.end(), tokens.begin(), tokens.end());
    return record(0x0006, b);
}

/* FORMULA carrying a cached boolean and a token blob. */
Bytes formula_bool(uint16_t row, uint16_t col, bool cached,
        const Bytes &tokens) {
    Bytes b;
    put16(b, row);
    put16(b, col);
    put16(b, 0);
    b.push_back(1);             // resid = boolean
    b.push_back(0);
    b.push_back(cached ? 1 : 0);
    b.insert(b.end(), 3, 0);    // rest of resdata
    put16(b, 0xFFFF);           // marks a non-double result
    put16(b, 0);                // flags
    b.insert(b.end(), 4, 0);    // chn
    put16(b, (uint16_t)tokens.size());
    b.insert(b.end(), tokens.begin(), tokens.end());
    return record(0x0006, b);
}

/* FORMULA whose claimed token length overruns the record body. */
Bytes formula_truncated(uint16_t row, uint16_t col) {
    Bytes b;
    put16(b, row);
    put16(b, col);
    put16(b, 0);
    put_double(b, 9.0);
    put16(b, 0);
    b.insert(b.end(), 4, 0);
    put16(b, 9);                // claims nine token bytes ...
    Bytes half;
    half.push_back(0x25);       // PtgArea ... only five are present
    put16(half, 0);
    put16(half, 0);
    b.insert(b.end(), half.begin(), half.end());
    return record(0x0006, b);
}

Bytes ptg_exp(uint16_t anchor_row, uint16_t anchor_col) {
    Bytes t;
    t.push_back(0x01);          // PtgExp
    put16(t, anchor_row);
    put16(t, anchor_col);
    return t;
}

Bytes formula_usage(uint16_t row, uint16_t col, uint16_t anchor_row,
        uint16_t anchor_col) {
    return formula_double(row, col, 0.0, ptg_exp(anchor_row, anchor_col));
}

Bytes shared_formula(uint16_t r0, uint16_t r1, uint8_t c0, uint8_t c1,
        const Bytes &tokens) {
    Bytes b;
    put16(b, r0);
    put16(b, r1);
    b.push_back(c0);
    b.push_back(c1);
    put16(b, (uint16_t)tokens.size());
    b.insert(b.end(), tokens.begin(), tokens.end());
    return record(0x04BC, b);
}

/* ---- token builders ---- */

Bytes ptg_ref_abs(uint16_t row, uint16_t col) {
    Bytes t;
    t.push_back(0x24);          // PtgRef
    put16(t, row);
    put16(t, col);             // no flag bits: both axes absolute
    return t;
}

Bytes ptg_refv_abs(uint16_t row, uint16_t col) {
    Bytes t;
    t.push_back(0x44);          // PtgRefV (value-class variant)
    put16(t, row);
    put16(t, col);
    return t;
}

Bytes ptg_ref_mixed_absrow_relcol(uint16_t abs_row, int16_t rel_col) {
    Bytes t;
    t.push_back(0x24);
    put16(t, abs_row);
    put16(t, (uint16_t)(0x8000u | (uint16_t)rel_col));
    return t;
}

Bytes ptg_ref_relative(int16_t rel_row, int16_t rel_col) {
    Bytes t;
    t.push_back(0x24);
    put16(t, (uint16_t)rel_row);
    put16(t, (uint16_t)(0xC000u | (uint16_t)rel_col));
    return t;
}

Bytes ptg_area_abs(uint16_t r0, uint16_t r1, uint16_t c0, uint16_t c1) {
    Bytes t;
    t.push_back(0x25);          // PtgArea
    put16(t, r0);
    put16(t, r1);
    put16(t, c0);
    put16(t, c1);
    return t;
}

/* Shared PtgRefN: relative row/column, both deltas explicit and signed. */
Bytes ptg_refn(int16_t row_delta, int16_t col_delta) {
    Bytes t;
    t.push_back(0x2C);
    put16(t, 0);                // encoded base unused for N tokens
    put16(t, 0xC000);           // both axes relative
    put16(t, (uint16_t)row_delta);
    put16(t, (uint16_t)col_delta);
    return t;
}

/* Shared PtgAreaN: relative rows, absolute columns 0..1, zero deltas. */
Bytes ptg_arean_mixed() {
    Bytes t;
    t.push_back(0x2D);
    put16(t, 0);                // first row encoded base
    put16(t, 0);                // last row: zero span
    put16(t, 0x4000);           // rows relative, columns absolute -> col 0
    put16(t, 0x4001);           // col 1
    put16(t, 0);                // row delta
    put16(t, 0);                // col delta
    return t;
}

/* Shared PtgAreaN with relative rows spanning row_span+1 rows and absolute
 * columns c0..c1; the dedicated per-cell offsets are both zero. */
Bytes ptg_arean_relrows(uint16_t c0, uint16_t c1, int16_t row_span) {
    Bytes t;
    t.push_back(0x2D);
    put16(t, 0);
    put16(t, (uint16_t)row_span);   // encoded last-first span
    put16(t, (uint16_t)(0x4000 | c0));  // relative rows, absolute col c0
    put16(t, (uint16_t)(0x4000 | c1));
    put16(t, 0);                    // row offset applied at the usage cell
    put16(t, 0);                    // col offset
    return t;
}

Bytes ptg_name() {
    Bytes t;
    t.push_back(0x23);          // PtgName
    put16(t, 1);                // name index
    put16(t, 0);
    return t;
}

Bytes ptg_ref3d() {
    Bytes t;
    t.push_back(0x3A);          // PtgRef3d
    put16(t, 0);                // ixti
    put16(t, 0);                // row
    put16(t, 0);                // col
    return t;
}

Bytes ptg_func(uint16_t func) {
    Bytes t;
    t.push_back(0x21);          // PtgFunc
    put16(t, func);
    return t;
}

Bytes ptg_int(uint16_t v) {
    Bytes t;
    t.push_back(0x1E);          // PtgInt
    put16(t, v);
    return t;
}

Bytes ptg_add() { return Bytes(1, 0x03); }

Bytes ptg_unicode_string_then_ref() {
    Bytes t;
    t.push_back(0x17);          // PtgStr
    t.push_back(2);             // two characters
    t.push_back(0x01);          // Unicode
    t.push_back('h'); t.push_back(0);
    t.push_back('i'); t.push_back(0);
    Bytes ref = ptg_ref_abs(3, 3);
    t.insert(t.end(), ref.begin(), ref.end());
    return t;
}

Bytes ptg_attr_choose_then_ref() {
    Bytes t;
    t.push_back(0x19);          // PtgAttr
    t.push_back(0x08);          // AttrChoose
    put16(t, 2);                // two choices -> three jump offsets
    put16(t, 0);
    put16(t, 0);
    put16(t, 0);
    Bytes ref = ptg_ref_abs(4, 4);
    t.insert(t.end(), ref.begin(), ref.end());
    return t;
}

/* ---- minimal OLE/CFB container writer ---- */

const uint32_t ENDOFCHAIN = 0xFFFFFFFE;
const uint32_t FREESECT = 0xFFFFFFFF;
const uint32_t FATSECT = 0xFFFFFFFD;

void pad_to(Bytes &b, size_t n) {
    while (b.size() < n)
        b.push_back(0);
}

Bytes directory_entry(const std::string &name, uint8_t type, uint32_t child,
        uint32_t start, uint32_t size) {
    Bytes e(128, 0);
    for (size_t i = 0; i < name.size(); ++i) {
        e[2 * i] = (uint8_t)name[i];
        e[2 * i + 1] = 0;
    }
    size_t name_bytes = name.size() * 2 + 2;   // include UTF-16 NUL
    e[64] = (uint8_t)(name_bytes & 0xFF);
    e[65] = (uint8_t)(name_bytes >> 8);
    e[66] = type;
    e[67] = 1;                  // black
    e[68] = 0xFF; e[69] = 0xFF;   // left sibling = FREESECT
    e[70] = 0xFF; e[71] = 0xFF;
    e[72] = 0xFF; e[73] = 0xFF;   // right sibling = FREESECT
    e[74] = 0xFF; e[75] = 0xFF;
    e[76] = child & 0xFF; e[77] = (child >> 8) & 0xFF;   // child
    e[78] = (child >> 16) & 0xFF; e[79] = child >> 24;
    // CLSID, state, timestamps (80..115) stay zero.
    e[116] = start & 0xFF; e[117] = (start >> 8) & 0xFF;
    e[118] = (start >> 16) & 0xFF; e[119] = start >> 24;
    e[120] = size & 0xFF; e[121] = (size >> 8) & 0xFF;
    e[122] = (size >> 16) & 0xFF; e[123] = size >> 24;
    return e;
}

Bytes wrap_cfb(const Bytes &workbook) {
    const size_t SECT = 512, MINI = 64;
    size_t mini_count = (workbook.size() + MINI - 1) / MINI;
    size_t mini_stream_len = mini_count * MINI;
    size_t root_sectors = (mini_stream_len + SECT - 1) / SECT;
    uint32_t dir_sector = 1;
    uint32_t root_start = 2;
    uint32_t minifat_sector = (uint32_t)(root_start + root_sectors);
    uint32_t total_sectors = minifat_sector + 1;

    Bytes out;
    const uint8_t magic[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    out.insert(out.end(), magic, magic + 8);
    pad_to(out, 24);
    put16(out, 0x003E);         // minor version
    put16(out, 0x0003);         // dll version
    put16(out, 0xFFFE);         // byte order
    put16(out, 9);              // 512-byte sectors
    put16(out, 6);              // 64-byte mini sectors
    put16(out, 0);
    put32(out, 0);
    put32(out, 0);
    put32(out, 1);              // cfat
    put32(out, dir_sector);     // directory start
    put32(out, 0);
    put32(out, 0x1000);         // mini stream cutoff
    put32(out, minifat_sector); // mini FAT start
    put32(out, 1);              // mini FAT sector count
    put32(out, ENDOFCHAIN);     // no DIFAT
    put32(out, 0);              // DIFAT count
    put32(out, 0);              // MSAT[0] -> sector 0
    pad_to(out, 512);
    // Fill the unused MSAT slots in place (the header is exactly 512 bytes).
    for (int i = 1; i < 109; ++i) {
        out[0x4C + 4 * i] = 0xFF;
        out[0x4C + 4 * i + 1] = 0xFF;
        out[0x4C + 4 * i + 2] = 0xFF;
        out[0x4C + 4 * i + 3] = 0xFF;
    }

    // Sector 0: FAT.
    Bytes fat(512, 0);
    auto fat_set = [&](uint32_t sid, uint32_t next) {
        fat[4 * sid] = next & 0xFF;
        fat[4 * sid + 1] = (next >> 8) & 0xFF;
        fat[4 * sid + 2] = (next >> 16) & 0xFF;
        fat[4 * sid + 3] = next >> 24;
    };
    for (uint32_t s = 0; s < total_sectors; ++s)
        fat_set(s, FREESECT);
    fat_set(0, FATSECT);
    fat_set(dir_sector, ENDOFCHAIN);
    for (uint32_t k = 0; k < root_sectors; ++k) {
        uint32_t sid = root_start + k;
        fat_set(sid, k + 1 < root_sectors ? sid + 1 : ENDOFCHAIN);
    }
    fat_set(minifat_sector, ENDOFCHAIN);
    out.insert(out.end(), fat.begin(), fat.end());

    // Sector 1: directory (root, Workbook, two empty entries).
    Bytes dir;
    Bytes root = directory_entry("Root Entry", 5, 1, root_start,
            (uint32_t)mini_stream_len);
    Bytes wb = directory_entry("Workbook", 2, FREESECT, 0,
            (uint32_t)workbook.size());
    dir.insert(dir.end(), root.begin(), root.end());
    dir.insert(dir.end(), wb.begin(), wb.end());
    dir.insert(dir.end(), 128, 0);
    dir.insert(dir.end(), 128, 0);
    out.insert(out.end(), dir.begin(), dir.end());

    // Root mini stream occupies whole regular sectors in its FAT chain.
    Bytes mini = workbook;
    mini.resize(mini_stream_len, 0);
    out.insert(out.end(), mini.begin(), mini.end());
    pad_to(out, 512 + SECT * minifat_sector);

    // Mini FAT: chain the mini sectors holding the workbook.
    Bytes minifat(512, 0);
    auto mini_set = [&](uint32_t sid, uint32_t next) {
        minifat[4 * sid] = next & 0xFF;
        minifat[4 * sid + 1] = (next >> 8) & 0xFF;
        minifat[4 * sid + 2] = (next >> 16) & 0xFF;
        minifat[4 * sid + 3] = next >> 24;
    };
    for (uint32_t s = 0; s < 128; ++s)
        mini_set(s, FREESECT);
    for (size_t i = 0; i < mini_count; ++i)
        mini_set((uint32_t)i, i + 1 < mini_count ? (uint32_t)(i + 1)
                                                 : ENDOFCHAIN);
    out.insert(out.end(), minifat.begin(), minifat.end());

    return out;
}

Bytes build_workbook() {
    Bytes globals;
    append(globals, bof(0x0005));
    Bytes codepage;
    put16(codepage, 1200);
    append(globals, record(0x0042, codepage));
    Bytes window1(18, 0);
    append(globals, record(0x003D, window1));

    Bytes sheet;
    append(sheet, bof(0x0010));
    append(sheet, row_rec(0, 255));
    for (uint16_t r = 1; r <= 15; ++r)
        append(sheet, row_rec(r, 5));

    auto add = [&](const Bytes &r) {
        sheet.insert(sheet.end(), r.begin(), r.end());
    };

    // Ordinary formulas in row 0.
    add(formula_double(0, 0, 1.0, ptg_ref_abs(0, 0)));
    add(formula_double(0, 1, 2.0, ptg_ref_mixed_absrow_relcol(2, 1)));
    add(formula_double(0, 2, 3.0, ptg_area_abs(0, 1, 0, 1)));
    {
        Bytes dup = ptg_area_abs(0, 1, 0, 1);
        Bytes again = ptg_area_abs(0, 1, 0, 1);
        dup.insert(dup.end(), again.begin(), again.end());
        dup.push_back(0x03);
        add(formula_double(0, 3, 4.0, dup));
    }
    {
        Bytes constants = ptg_int(1);
        Bytes two = ptg_int(2);
        constants.insert(constants.end(), two.begin(), two.end());
        constants.push_back(0x03);
        add(formula_double(0, 4, 5.0, constants));
    }
    add(formula_double(0, 5, 6.0, ptg_name()));
    add(number_cell(0, 6, 42.0));
    add(formula_double(0, 7, 7.0, ptg_unicode_string_then_ref()));
    add(formula_double(0, 8, 8.0, ptg_attr_choose_then_ref()));
    add(formula_double(0, 9, 0.0, ptg_func(0x94)));   // INDIRECT
    add(formula_double(0, 10, 0.0, ptg_func(0x4E)));  // OFFSET
    add(formula_double(0, 255, 0.0,
            ptg_ref_mixed_absrow_relcol(0, 1)));      // column 256: OOB
    add(formula_double(0, 11, 0.0,
            ptg_ref_relative(-1, 0)));               // row -1: OOB, no wrap
    add(formula_double(0, 12, 0.0, ptg_refv_abs(6, 6)));
    {
        // Two distinct refs emitted out of order; result must be sorted.
        Bytes ordered = ptg_ref_abs(5, 5);
        Bytes earlier = ptg_ref_abs(2, 2);
        ordered.insert(ordered.end(), earlier.begin(), earlier.end());
        add(formula_double(0, 13, 0.0, ordered));
    }
    {
        // Two overlapping but distinct areas must both be retained.
        Bytes overlap = ptg_area_abs(0, 2, 0, 1);
        Bytes other = ptg_area_abs(1, 3, 0, 1);
        overlap.insert(overlap.end(), other.begin(), other.end());
        add(formula_double(0, 14, 0.0, overlap));
    }
    {
        // A valid reference followed by an unsupported token: the partial
        // reference must not be returned.
        Bytes mixed = ptg_ref_abs(1, 1);
        Bytes name = ptg_name();
        mixed.insert(mixed.end(), name.begin(), name.end());
        add(formula_double(0, 15, 0.0, mixed));
    }
    {
        // A valid reference followed by an out-of-bounds one: no partial list.
        Bytes mixed = ptg_ref_abs(9, 9);
        Bytes oob = ptg_ref_mixed_absrow_relcol(0, 240);  // col 16+240=256
        mixed.insert(mixed.end(), oob.begin(), oob.end());
        add(formula_double(0, 16, 0.0, mixed));
    }
    add(formula_double(1, 0, 1.0, ptg_ref_relative(-1, 0)));

    // Corrupt formula next to a perfectly legal neighbour.
    add(formula_truncated(2, 0));
    add(formula_double(2, 1, 3.0, ptg_ref_abs(7, 7)));

    // Shared block #1: anchor (4,2), rectangle rows 4..6 / cols 2..3.
    {
        Bytes shared;
        Bytes rn = ptg_refn(-1, -2);
        Bytes an = ptg_arean_mixed();
        Bytes absref = ptg_ref_abs(1, 1);
        shared.insert(shared.end(), rn.begin(), rn.end());
        shared.insert(shared.end(), an.begin(), an.end());
        shared.insert(shared.end(), absref.begin(), absref.end());
        shared.push_back(0x03);
        shared.push_back(0x03);
        add(formula_double(4, 2, 1.0, shared));       // anchor FORMULA first
        add(shared_formula(4, 6, 2, 3, shared));      // SHRFMLA right after
        add(number_cell(5, 0, 1.0));                  // non-contiguous gap
        add(formula_usage(5, 3, 4, 2));
        add(number_cell(6, 0, 2.0));
        add(formula_usage(6, 3, 4, 2));
        add(formula_usage(6, 2, 4, 2));
    }

    // Shared block #2: definition arrives after the anchor, usage later.
    {
        Bytes shared = ptg_refn(-1, 0);
        add(formula_double(8, 0, 1.0, shared));
        add(shared_formula(8, 9, 0, 0, shared));
        add(number_cell(8, 1, 8.0));
        add(formula_usage(9, 0, 8, 0));
    }

    // Shared block #3: usage outside the declared rectangle.
    {
        Bytes shared = ptg_refn(0, 0);
        add(formula_double(10, 0, 1.0, shared));
        add(shared_formula(10, 10, 0, 0, shared));
        add(formula_usage(11, 0, 10, 0));
    }

    // Shared block #4: duplicate SHRFMLA definitions for one anchor.
    {
        Bytes shared = ptg_ref_abs(0, 0);
        add(formula_double(12, 0, 1.0, shared));
        add(shared_formula(12, 12, 0, 0, shared));
        add(shared_formula(12, 12, 0, 0, shared));
    }

    // Shared block #5: multi-row relative AreaN keeps its span at every usage.
    {
        Bytes shared = ptg_arean_relrows(2, 3, 2);  // rows base..base+2
        add(formula_double(13, 0, 1.0, shared));
        add(shared_formula(13, 14, 0, 0, shared));
        add(formula_usage(14, 0, 13, 0));
    }

    // ptgExp whose shared definition is missing entirely.
    add(formula_usage(14, 5, 20, 20));

    // Unsupported formula whose cached boolean must remain readable.
    add(formula_bool(15, 0, true, ptg_name()));

    append(sheet, eof_rec());

    // The sheet stream starts right after globals + BOUNDSHEET (4-byte header
    // and a 10-byte body) + the globals EOF.
    Bytes bs = boundsheet(0);
    Bytes eof = eof_rec();
    uint32_t sheet_pos = (uint32_t)(globals.size() + bs.size() + eof.size());
    bs = boundsheet(sheet_pos);
    globals.insert(globals.end(), bs.begin(), bs.end());
    globals.insert(globals.end(), eof.begin(), eof.end());

    Bytes workbook = globals;
    workbook.insert(workbook.end(), sheet.begin(), sheet.end());
    return wrap_cfb(workbook);
}

struct Opened {
    xlsWorkBook *wb;
    xlsWorkSheet *ws;
};

Opened open_parse(const Bytes &file) {
    Opened o{nullptr, nullptr};
    xls_error_t err = LIBXLS_OK;
    o.wb = xls_open_buffer(file.data(), file.size(), "UTF-8", &err);
    if (!o.wb)
        throw std::runtime_error("xls_open_buffer failed");
    o.ws = xls_getWorkSheet(o.wb, 0);
    if (!o.ws)
        throw std::runtime_error("xls_getWorkSheet failed");
    if (xls_parseWorkSheet(o.ws) != LIBXLS_OK)
        throw std::runtime_error("xls_parseWorkSheet failed");
    return o;
}

void close(Opened &o) {
    if (o.ws)
        xls_close_WS(o.ws);
    if (o.wb)
        xls_close_WB(o.wb);
}

struct Rect {
    uint16_t r0, r1, c0, c1;
    bool operator==(const Rect &o) const {
        return r0 == o.r0 && r1 == o.r1 && c0 == o.c0 && c1 == o.c1;
    }
};

std::vector<Rect> rects_of(xls_formula_dependencies *d) {
    std::vector<Rect> out;
    for (DWORD i = 0; i < d->count; ++i) {
        const xls_formula_dependency &x = d->dependencies[i];
        out.push_back({x.row_first, x.row_last, x.col_first, x.col_last});
    }
    return out;
}

void expect_rects(const Bytes &file, uint16_t row, uint16_t col,
        const std::vector<Rect> &want) {
    Opened o = open_parse(file);
    xls_formula_dependencies *d = nullptr;
    xls_formula_query_status st = xls_get_formula_dependencies(o.ws, row, col, &d);
    CHECK(st == XLS_FORMULA_QUERY_OK);
    CHECK(d != nullptr);
    std::vector<Rect> got = rects_of(d);
    CHECK(got == want);
    xls_free_formula_dependencies(d);
    close(o);
}

}  // namespace

CASE(ordinary_refs) {
    Bytes file = build_workbook();

    expect_rects(file, 0, 0, {{0, 0, 0, 0}});
    // Absolute row 2, relative column +1 from column 1 -> column 2.
    expect_rects(file, 0, 1, {{2, 2, 2, 2}});
    expect_rects(file, 0, 2, {{0, 1, 0, 1}});

    // Exact duplicate rectangle is reported once; ordering is canonical.
    expect_rects(file, 0, 3, {{0, 1, 0, 1}});

    // Signed negative offset from row 1 points back at row 0.
    expect_rects(file, 1, 0, {{0, 0, 0, 0}});

    Opened o = open_parse(file);
    xls_formula_dependencies *d = nullptr;

    // Constants-only formula: legal formula, zero static references.
    CHECK(xls_get_formula_dependencies(o.ws, 0, 4, &d)
            == XLS_FORMULA_QUERY_NO_REFERENCES);
    CHECK(d->count == 0);
    xls_free_formula_dependencies(d);

    // A plain number cell is distinguishable from a reference-free formula.
    CHECK(xls_get_formula_dependencies(o.ws, 0, 6, &d)
            == XLS_FORMULA_QUERY_NOT_A_FORMULA);
    CHECK(d->count == 0);
    xls_free_formula_dependencies(d);

    // Unicode PtgStr and AttrChoose jump tables are skipped by real length.
    CHECK(xls_get_formula_dependencies(o.ws, 0, 7, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{{3, 3, 3, 3}}));
    xls_free_formula_dependencies(d);

    CHECK(xls_get_formula_dependencies(o.ws, 0, 8, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{{4, 4, 4, 4}}));
    xls_free_formula_dependencies(d);

    // Value-class (V) reference variant normalizes to the same rectangle.
    CHECK(xls_get_formula_dependencies(o.ws, 0, 12, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{{6, 6, 6, 6}}));
    xls_free_formula_dependencies(d);

    // Distinct rectangles are returned in canonical sorted order.
    CHECK(xls_get_formula_dependencies(o.ws, 0, 13, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{{2, 2, 2, 2}, {5, 5, 5, 5}}));
    xls_free_formula_dependencies(d);

    // Overlapping rectangles are not merged.
    CHECK(xls_get_formula_dependencies(o.ws, 0, 14, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{
            {0, 2, 0, 1}, {1, 3, 0, 1}}));
    xls_free_formula_dependencies(d);

    // Failures never return a partial list, even after a valid reference.
    CHECK(xls_get_formula_dependencies(o.ws, 0, 15, &d)
            == XLS_FORMULA_QUERY_UNSUPPORTED);
    CHECK(d->count == 0);
    xls_free_formula_dependencies(d);
    CHECK(xls_get_formula_dependencies(o.ws, 0, 16, &d)
            == XLS_FORMULA_QUERY_OUT_OF_BOUNDS);
    CHECK(d->count == 0);
    xls_free_formula_dependencies(d);

    // Strict non-wrapping policy: relative column 256 does not wrap.
    CHECK(xls_get_formula_dependencies(o.ws, 0, 255, &d)
            == XLS_FORMULA_QUERY_OUT_OF_BOUNDS);
    CHECK(d->count == 0);
    xls_free_formula_dependencies(d);

    // A negative row offset reaching -1 is rejected instead of wrapping.
    CHECK(xls_get_formula_dependencies(o.ws, 0, 11, &d)
            == XLS_FORMULA_QUERY_OUT_OF_BOUNDS);
    CHECK(d->count == 0);
    xls_free_formula_dependencies(d);

    close(o);
}

CASE(shared_offsets) {
    Bytes file = build_workbook();
    Opened o = open_parse(file);
    xls_formula_dependencies *d = nullptr;

    // Anchor: PtgRefN(-1,-2) from (4,2) -> (3,0); mixed AreaN -> rows 4..4,
    // absolute cols 0..1; absolute PtgRef stays at (1,1).
    CHECK(xls_get_formula_dependencies(o.ws, 4, 2, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{
            {1, 1, 1, 1}, {3, 3, 0, 0}, {4, 4, 0, 1}}));
    xls_free_formula_dependencies(d);

    // The worked example from the spec: usage (5,3), offsets (-1,-2) -> (4,1).
    CHECK(xls_get_formula_dependencies(o.ws, 5, 3, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{
            {1, 1, 1, 1}, {4, 4, 1, 1}, {5, 5, 0, 1}}));
    xls_free_formula_dependencies(d);

    // Another usage, different row: offsets are interpreted per usage cell.
    CHECK(xls_get_formula_dependencies(o.ws, 6, 3, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{
            {1, 1, 1, 1}, {5, 5, 1, 1}, {6, 6, 0, 1}}));
    xls_free_formula_dependencies(d);

    // Non-contiguous usage (records were separated by NUMBER cells).
    CHECK(xls_get_formula_dependencies(o.ws, 6, 2, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{
            {1, 1, 1, 1}, {5, 5, 0, 0}, {6, 6, 0, 1}}));
    xls_free_formula_dependencies(d);

    // Second shared block: per-cell relative row offset.
    CHECK(xls_get_formula_dependencies(o.ws, 8, 0, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{{7, 7, 0, 0}}));
    xls_free_formula_dependencies(d);
    CHECK(xls_get_formula_dependencies(o.ws, 9, 0, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{{8, 8, 0, 0}}));
    xls_free_formula_dependencies(d);

    // Multi-row relative AreaN: the span (3 rows) is preserved and the base
    // moves with each usage cell; absolute columns stay fixed.
    CHECK(xls_get_formula_dependencies(o.ws, 13, 0, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{{13, 15, 2, 3}}));
    xls_free_formula_dependencies(d);
    CHECK(xls_get_formula_dependencies(o.ws, 14, 0, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{{14, 16, 2, 3}}));
    xls_free_formula_dependencies(d);

    close(o);
}

CASE(record_order) {
    Bytes file = build_workbook();
    Opened o = open_parse(file);
    xls_formula_dependencies *d = nullptr;

    // Truncated token stream: corrupt status with a token offset, while the
    // cached numeric value survives through the ordinary cell API.
    CHECK(xls_get_formula_dependencies(o.ws, 2, 0, &d)
            == XLS_FORMULA_QUERY_CORRUPT);
    CHECK(d->record_id == 0x0006);
    CHECK(d->token_offset == 0);
    xls_free_formula_dependencies(d);

    xlsCell *cached = xls_cell(o.ws, 2, 0);
    CHECK(cached != nullptr);
    CHECK(std::fabs(cached->d - 9.0) < 1e-12);

    // The neighbouring legal formula is untouched by the corrupt cell.
    CHECK(xls_get_formula_dependencies(o.ws, 2, 1, &d)
            == XLS_FORMULA_QUERY_OK);
    CHECK(rects_of(d) == (std::vector<Rect>{{7, 7, 7, 7}}));
    xls_free_formula_dependencies(d);

    // ptgExp usage outside the SHRFMLA rectangle is corruption.
    CHECK(xls_get_formula_dependencies(o.ws, 11, 0, &d)
            == XLS_FORMULA_QUERY_CORRUPT);
    xls_free_formula_dependencies(d);

    // Duplicate SHRFMLA definition is reported against the SHRFMLA record.
    CHECK(xls_get_formula_dependencies(o.ws, 12, 0, &d)
            == XLS_FORMULA_QUERY_CORRUPT);
    CHECK(d->record_id == 0x04BC);
    xls_free_formula_dependencies(d);

    // ptgExp without any matching SHRFMLA definition.
    CHECK(xls_get_formula_dependencies(o.ws, 14, 5, &d)
            == XLS_FORMULA_QUERY_CORRUPT);
    xls_free_formula_dependencies(d);

    close(o);
}

CASE(unsupported_isolation) {
    Bytes file = build_workbook();
    Opened o = open_parse(file);
    xls_formula_dependencies *d = nullptr;

    // Defined names, 3D references and dynamic address functions refuse to
    // return partial dependencies.
    CHECK(xls_get_formula_dependencies(o.ws, 0, 5, &d)
            == XLS_FORMULA_QUERY_UNSUPPORTED);
    CHECK(d->count == 0);
    xls_free_formula_dependencies(d);

    CHECK(xls_get_formula_dependencies(o.ws, 0, 9, &d)
            == XLS_FORMULA_QUERY_UNSUPPORTED);
    xls_free_formula_dependencies(d);
    CHECK(xls_get_formula_dependencies(o.ws, 0, 10, &d)
            == XLS_FORMULA_QUERY_UNSUPPORTED);
    xls_free_formula_dependencies(d);

    // Cached boolean of an unsupported formula still reads via the old API.
    xlsCell *bool_cell = xls_cell(o.ws, 15, 0);
    CHECK(bool_cell != nullptr);
    CHECK(bool_cell->str != nullptr);
    CHECK(std::string(bool_cell->str) == "bool");
    CHECK(xls_get_formula_dependencies(o.ws, 15, 0, &d)
            == XLS_FORMULA_QUERY_UNSUPPORTED);
    xls_free_formula_dependencies(d);

    // A legal formula elsewhere on the same sheet is unaffected.
    CHECK(xls_get_formula_dependencies(o.ws, 0, 0, &d)
            == XLS_FORMULA_QUERY_OK);
    xls_free_formula_dependencies(d);

    close(o);

    // Result lifetime: independent copies, free before close, re-query, and
    // two worksheet objects opened from the same buffer never share state.
    Opened a = open_parse(file);
    Opened b = open_parse(file);
    xls_formula_dependencies *da = nullptr;
    xls_formula_dependencies *db = nullptr;
    CHECK(xls_get_formula_dependencies(a.ws, 4, 2, &da)
            == XLS_FORMULA_QUERY_OK);
    CHECK(xls_get_formula_dependencies(b.ws, 5, 3, &db)
            == XLS_FORMULA_QUERY_OK);
    CHECK(da != db);
    CHECK(rects_of(da) != rects_of(db));
    xls_free_formula_dependencies(da);
    da = nullptr;
    // Closing the first worksheet cannot invalidate the second result.
    xls_close_WS(a.ws);
    a.ws = nullptr;
    CHECK(rects_of(db) == (std::vector<Rect>{
            {1, 1, 1, 1}, {4, 4, 1, 1}, {5, 5, 0, 1}}));
    xls_free_formula_dependencies(db);
    xls_close_WS(b.ws);
    b.ws = nullptr;
    xls_close_WB(a.wb);
    xls_close_WB(b.wb);
}
