// Contract tests for dwarf::line_index.  All line number programs
// are synthesized in C++; no images are executed.

#include "dwarf/line_index.hh"
#include "dwarf/internal.hh"
#include "task_support/check.h"
#include "task_support/line_fixture.hh"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace std;

namespace {

using bytes = vector<unsigned char>;

void
u16(bytes &b, uint16_t x)
{
        b.push_back(x & 0xff);
        b.push_back((x >> 8) & 0xff);
}

void
u32(bytes &b, uint32_t x)
{
        for (int i = 0; i < 4; i++)
                b.push_back((x >> (8 * i)) & 0xff);
}

void
u64(bytes &b, uint64_t x)
{
        for (int i = 0; i < 8; i++)
                b.push_back((x >> (8 * i)) & 0xff);
}

void
uleb(bytes &b, uint64_t x)
{
        do {
                unsigned char c = x & 0x7f;
                x >>= 7;
                if (x)
                        c |= 0x80;
                b.push_back(c);
        } while (x);
}

void
sleb(bytes &b, int64_t x)
{
        while (true) {
                unsigned char c = x & 0x7f;
                x >>= 7;
                if ((x == 0 && !(c & 0x40)) || (x == -1 && (c & 0x40))) {
                        b.push_back(c);
                        return;
                }
                b.push_back(c | 0x80);
        }
}

// A DWARF2-4 line number program under construction.  Opcode
// encodings follow DWARF4 section 6.2.5.
struct line_program
{
        unsigned version = 4;
        unsigned char min_inst = 1;
        unsigned char max_ops = 1;
        unsigned char default_is_stmt = 1;
        signed char line_base = -5;
        unsigned char line_range = 14;
        vector<string> files;   // header file entries, directory 0
        bytes prog;

        void extended(unsigned char subop, const bytes &payload)
        {
                prog.push_back(0);
                uleb(prog, 1 + payload.size());
                prog.push_back(subop);
                prog.insert(prog.end(), payload.begin(), payload.end());
        }

        // Extended opcodes
        void end_sequence()
        {
                extended(1, bytes());
        }
        void set_address(uint64_t addr)
        {
                bytes p;
                u64(p, addr);
                extended(2, p);
        }
        void define_file(const string &name)
        {
                bytes p;
                p.insert(p.end(), name.begin(), name.end());
                p.push_back(0);
                uleb(p, 0);     // directory index
                uleb(p, 0);     // modification time
                uleb(p, 0);     // length
                extended(3, p);
        }
        void set_discriminator(uint64_t d)
        {
                bytes p;
                uleb(p, d);
                extended(4, p);
        }

        // Standard opcodes
        void copy() { prog.push_back(1); }
        void advance_pc(uint64_t n) { prog.push_back(2); uleb(prog, n); }
        void advance_line(int64_t n) { prog.push_back(3); sleb(prog, n); }
        void set_file(uint64_t n) { prog.push_back(4); uleb(prog, n); }
        void set_column(uint64_t n) { prog.push_back(5); uleb(prog, n); }
        void negate_stmt() { prog.push_back(6); }
        // Special opcode (DWARF4 section 6.2.5.1)
        void special(unsigned op_advance, int line_inc)
        {
                prog.push_back(13 + op_advance * line_range +
                               (line_inc - line_base));
        }

        bytes build() const
        {
                bytes h;
                h.push_back(min_inst);
                if (version >= 4)
                        h.push_back(max_ops);
                h.push_back(default_is_stmt);
                h.push_back((unsigned char)line_base);
                h.push_back(line_range);
                h.push_back(13);        // opcode_base
                static const unsigned char opcode_lengths[12] =
                        {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
                h.insert(h.end(), opcode_lengths, opcode_lengths + 12);
                h.push_back(0);         // no include directories
                for (const string &f : files) {
                        h.insert(h.end(), f.begin(), f.end());
                        h.push_back(0);
                        uleb(h, 0);     // directory index
                        uleb(h, 0);     // modification time
                        uleb(h, 0);     // length
                }
                h.push_back(0);         // end of file entries

                bytes body;
                u16(body, (uint16_t)version);
                u32(body, (uint32_t)h.size());
                body.insert(body.end(), h.begin(), h.end());
                body.insert(body.end(), prog.begin(), prog.end());

                bytes out;
                u32(out, (uint32_t)body.size());
                out.insert(out.end(), body.begin(), body.end());
                return out;
        }
};

dwarf::line_table
make_table(const bytes &b, const string &comp_dir = "/src",
           const string &cu_name = "u.c")
{
        auto sec = make_shared<dwarf::section>(
                dwarf::section_type::line, b.data(), b.size(),
                dwarf::byte_order::lsb);
        return dwarf::line_table(sec, 0, 8, comp_dir, cu_name);
}

bool
throws_format_error(dwarf::line_table &t)
{
        try {
                dwarf::line_index idx(t);
                (void)idx;
        } catch (const dwarf::format_error &) {
                return true;
        }
        return false;
}

// ----------------------------------------------------------------
// operation_ranges

int
case_op_slots_and_equal_coordinates()
{
        // A DWARF4 table with two operation slots per instruction.
        line_program p;
        p.max_ops = 2;
        p.files.push_back("u.c");
        p.set_address(0x100);
        p.copy();               // row 0: (0x100, 0) line 1
        p.advance_pc(1);        // op_index 0 -> 1, same address
        p.copy();               // row 1: (0x100, 1) line 1
        p.advance_pc(1);        // wraps to (0x101, 0)
        p.advance_line(1);
        p.copy();               // row 2: (0x101, 0) line 2
        p.negate_stmt();
        p.copy();               // row 3: (0x101, 0) line 2, not stmt
        p.advance_pc(2);        // (0x102, 0)
        p.end_sequence();       //        (0x102, 0)
        bytes b = p.build();
        dwarf::line_table t = make_table(b);
        dwarf::line_index idx(t);

        // Each operation slot is its own coordinate.
        auto r = idx.find(0x100, 0);
        TASK_CHECK(r.size() == 1);
        TASK_CHECK(r[0].begin_address == 0x100 && r[0].begin_op_index == 0);
        TASK_CHECK(r[0].end_address == 0x100 && r[0].end_op_index == 1);
        TASK_CHECK(r[0].line == 1 && r[0].is_stmt);
        TASK_CHECK(r[0].sequence == 0 && r[0].row == 0);
        TASK_CHECK(r[0].file == "/src/u.c");

        r = idx.find(0x100, 1);
        TASK_CHECK(r.size() == 1);
        TASK_CHECK(r[0].begin_address == 0x100 && r[0].begin_op_index == 1);
        TASK_CHECK(r[0].end_address == 0x101 && r[0].end_op_index == 0);
        TASK_CHECK(r[0].row == 1);

        // Coordinates compare lexicographically: (0x100, 2) falls
        // in [(0x100, 1), (0x101, 0)).
        r = idx.find(0x100, 2);
        TASK_CHECK(r.size() == 1 && r[0].row == 1);

        // The default operation slot is 0.
        r = idx.find(0x100);
        TASK_CHECK(r.size() == 1 && r[0].row == 0);

        // Both rows at the same coordinate are kept and share their
        // right boundary.
        r = idx.find(0x101, 0);
        TASK_CHECK(r.size() == 2);
        TASK_CHECK(r[0].row == 2 && r[0].is_stmt);
        TASK_CHECK(r[1].row == 3 && !r[1].is_stmt);
        TASK_CHECK(r[0].begin_address == 0x101 && r[0].begin_op_index == 0);
        TASK_CHECK(r[0].end_address == 0x102 && r[0].end_op_index == 0);
        TASK_CHECK(r[1].begin_address == 0x101 && r[1].begin_op_index == 0);
        TASK_CHECK(r[1].end_address == 0x102 && r[1].end_op_index == 0);

        // The terminating coordinate is half-open: it covers
        // nothing, and nothing extends past the sequence.
        TASK_CHECK(idx.find(0x102, 0).empty());
        TASK_CHECK(idx.find(0x103, 0).empty());
        TASK_CHECK(idx.find(0xff, 0).empty());
        return 0;
}

int
case_overlapping_shuffled_sequences()
{
        line_program p;
        p.version = 2;
        p.files.push_back("u.c");
        // Sequence 0: [0x100, 0x110)
        p.set_address(0x100);
        p.advance_line(9);
        p.copy();               // seq 0 row 0: 0x100 line 10
        p.advance_pc(8);
        p.advance_line(1);
        p.copy();               // seq 0 row 1: 0x108 line 11
        p.advance_pc(8);
        p.end_sequence();       //        0x110
        // Sequence 1: [0x108, 0x118), overlapping sequence 0
        p.set_address(0x108);
        p.advance_line(19);
        p.copy();               // seq 1 row 0: 0x108 line 20
        p.advance_pc(8);
        p.negate_stmt();
        p.advance_line(-20);
        p.copy();               // seq 1 row 1: 0x110 line 0, not stmt
        p.advance_pc(8);
        p.end_sequence();       //        0x118
        // Sequence 2: [0x80, 0x90), out of address order
        p.set_address(0x80);
        p.advance_line(29);
        p.copy();               // seq 2 row 0: 0x80 line 30
        p.advance_pc(16);
        p.end_sequence();       //        0x90
        bytes b = p.build();
        dwarf::line_table t = make_table(b);
        dwarf::line_index idx(t);

        // Overlapping sequences both cover 0x108.
        auto r = idx.find(0x108);
        TASK_CHECK(r.size() == 2);
        TASK_CHECK(r[0].sequence == 0 && r[0].row == 1);
        TASK_CHECK(r[0].line == 11 && r[0].begin_address == 0x108 &&
                   r[0].end_address == 0x110);
        TASK_CHECK(r[1].sequence == 1 && r[1].row == 0);
        TASK_CHECK(r[1].line == 20 && r[1].begin_address == 0x108 &&
                   r[1].end_address == 0x110);

        // Inside sequence 0 only.
        r = idx.find(0x104);
        TASK_CHECK(r.size() == 1 && r[0].line == 10 &&
                   r[0].sequence == 0 && r[0].row == 0);

        // Past sequence 0's end: sequence 0 does not borrow from
        // sequence 1, and the line-0, non-statement row of sequence
        // 1 still counts.
        r = idx.find(0x110);
        TASK_CHECK(r.size() == 1);
        TASK_CHECK(r[0].sequence == 1 && r[0].row == 1);
        TASK_CHECK(r[0].line == 0 && !r[0].is_stmt);
        TASK_CHECK(r[0].begin_address == 0x110 && r[0].end_address == 0x118);

        r = idx.find(0x117);
        TASK_CHECK(r.size() == 1 && r[0].sequence == 1 && r[0].row == 1);
        TASK_CHECK(idx.find(0x118).empty());

        // The out-of-order sequence is indexed all the same.
        r = idx.find(0x88);
        TASK_CHECK(r.size() == 1);
        TASK_CHECK(r[0].sequence == 2 && r[0].row == 0 && r[0].line == 30);
        TASK_CHECK(r[0].begin_address == 0x80 && r[0].end_address == 0x90);
        TASK_CHECK(idx.find(0x90).empty());
        TASK_CHECK(idx.find(0x7f).empty());
        return 0;
}

int
case_sequence_tail_termination()
{
        bytes b = fixture::ordinary();
        dwarf::line_table t = fixture::table(b);

        // The end_sequence row at the end of the section is
        // iterated exactly once, and repeated traversal agrees.
        for (int pass = 0; pass < 2; pass++) {
                unsigned rows = 0, terminal = 0;
                dwarf::taddr last = 0;
                for (const auto &ent : t) {
                        rows++;
                        if (ent.end_sequence) {
                                terminal++;
                                last = ent.address;
                        }
                }
                TASK_CHECK(rows == 3);
                TASK_CHECK(terminal == 1);
                TASK_CHECK(last == 0x108);
        }

        // The classic query API keeps its signature and now reaches
        // the tail of the sequence.
        TASK_CHECK(t.find_address(0x102) != t.end());
        TASK_CHECK(t.find_address(0x102)->line == 1);
        TASK_CHECK(t.find_address(0x106) != t.end());
        TASK_CHECK(t.find_address(0x106)->line == 2);
        TASK_CHECK(t.find_address(0x108) == t.end());

        // The last group is bounded by the end_sequence coordinate;
        // the terminating row itself produces no source.
        dwarf::line_index idx(t);
        auto r = idx.find(0x104);
        TASK_CHECK(r.size() == 1);
        TASK_CHECK(r[0].line == 2);
        TASK_CHECK(r[0].begin_address == 0x104 && r[0].end_address == 0x108);
        TASK_CHECK(idx.find(0x108).empty());

        // An empty interval (a row at the end_sequence coordinate)
        // is not materialized.
        line_program p;
        p.files.push_back("u.c");
        p.set_address(0x100);
        p.copy();
        p.end_sequence();       // same coordinate 0x100
        bytes b2 = p.build();
        dwarf::line_table t2 = make_table(b2);
        dwarf::line_index idx2(t2);
        TASK_CHECK(idx2.find(0x100).empty());
        TASK_CHECK(idx2.find_line("/src/u.c", 1).empty());

        // Empty tables iterate and index normally.
        dwarf::line_table empty;
        TASK_CHECK(empty.begin() == empty.end());
        dwarf::line_index idx3(empty);
        TASK_CHECK(idx3.find(0x100).empty());
        TASK_CHECK(idx3.find_line("/src/u.c", 1).empty());
        return 0;
}

int
case_malformed_programs()
{
        // A sequence without a terminating row.
        {
                line_program p;
                p.files.push_back("u.c");
                p.set_address(0x100);
                p.copy();
                bytes b = p.build();
                dwarf::line_table t = make_table(b);
                TASK_CHECK(throws_format_error(t));
        }
        // Trailing state machine opcodes without a terminating row.
        {
                line_program p;
                p.files.push_back("u.c");
                p.set_address(0x100);
                p.copy();
                p.advance_pc(4);
                bytes b = p.build();
                dwarf::line_table t = make_table(b);
                TASK_CHECK(throws_format_error(t));
        }
        // Coordinates moving backwards within a sequence.
        {
                line_program p;
                p.files.push_back("u.c");
                p.set_address(0x100);
                p.copy();
                p.set_address(0x80);
                p.copy();
                p.end_sequence();
                bytes b = p.build();
                dwarf::line_table t = make_table(b);
                TASK_CHECK(throws_format_error(t));
        }
        // An end_sequence coordinate before the last row.
        {
                line_program p;
                p.files.push_back("u.c");
                p.set_address(0x100);
                p.copy();
                p.advance_pc(8);
                p.copy();
                p.set_address(0x104);
                p.end_sequence();
                bytes b = p.build();
                dwarf::line_table t = make_table(b);
                TASK_CHECK(throws_format_error(t));
        }
        // A well-formed single-row sequence does not throw.
        {
                line_program p;
                p.files.push_back("u.c");
                p.set_address(0x100);
                p.copy();
                p.advance_pc(4);
                p.end_sequence();
                bytes b = p.build();
                dwarf::line_table t = make_table(b);
                TASK_CHECK(!throws_format_error(t));
        }
        return 0;
}

// ----------------------------------------------------------------
// source_sites

int
case_breakpoints_for_line()
{
        line_program p;
        p.files.push_back("u.c");
        // Sequence 0: three statements on line 42, then a
        // non-statement row on the same line.
        p.set_address(0x100);
        p.advance_line(41);
        p.set_column(8);
        p.copy();               // seq 0 row 0: 0x100 line 42 col 8
        p.advance_pc(8);
        p.set_discriminator(2);
        p.copy();               // seq 0 row 1: 0x108 line 42 col 8 disc 2
        p.advance_pc(8);
        p.set_column(12);
        p.copy();               // seq 0 row 2: 0x110 line 42 col 12
        p.advance_pc(8);
        p.negate_stmt();
        p.copy();               // seq 0 row 3: 0x118 line 42, not stmt
        p.advance_pc(8);
        p.end_sequence();       //        0x120
        // Sequence 1: another statement on line 42 at the same
        // address as sequence 0's first row, then line 43.
        p.set_address(0x100);
        p.advance_line(41);
        p.set_column(8);
        p.copy();               // seq 1 row 0: 0x100 line 42 col 8
        p.advance_pc(8);
        p.advance_line(1);
        p.copy();               // seq 1 row 1: 0x108 line 43
        p.advance_pc(8);
        p.end_sequence();       //        0x110
        bytes b = p.build();
        dwarf::line_table t = make_table(b);
        dwarf::line_index idx(t);

        // Every statement interval of line 42 is a breakpoint
        // location; equal addresses from different sequences,
        // columns, or discriminators are not merged.
        auto r = idx.find_line("/src/u.c", 42);
        TASK_CHECK(r.size() == 4);
        // Ordered by begin coordinate, then sequence and row.
        TASK_CHECK(r[0].begin_address == 0x100 && r[0].end_address == 0x108);
        TASK_CHECK(r[0].sequence == 0 && r[0].row == 0);
        TASK_CHECK(r[0].column == 8 && r[0].discriminator == 0 &&
                   r[0].is_stmt);
        TASK_CHECK(r[0].file == "/src/u.c" && r[0].line == 42);
        TASK_CHECK(r[1].begin_address == 0x100 && r[1].end_address == 0x108);
        TASK_CHECK(r[1].sequence == 1 && r[1].row == 0);
        TASK_CHECK(r[2].begin_address == 0x108 && r[2].end_address == 0x110);
        TASK_CHECK(r[2].sequence == 0 && r[2].row == 1);
        TASK_CHECK(r[2].discriminator == 2);
        TASK_CHECK(r[3].begin_address == 0x110 && r[3].end_address == 0x118);
        TASK_CHECK(r[3].sequence == 0 && r[3].row == 2);
        TASK_CHECK(r[3].column == 12);

        // The non-statement row on line 42 is not a candidate, but
        // still covers its coordinate.
        auto all = idx.find(0x118);
        TASK_CHECK(all.size() == 1 && !all[0].is_stmt && all[0].line == 42);

        // Asking for a line with no locations does not fall through
        // to the next line.
        r = idx.find_line("/src/u.c", 43);
        TASK_CHECK(r.size() == 1 && r[0].begin_address == 0x108 &&
                   r[0].sequence == 1 && r[0].row == 1);
        TASK_CHECK(idx.find_line("/src/u.c", 44).empty());
        TASK_CHECK(idx.find_line("/src/u.c", 1).empty());
        return 0;
}

int
case_exact_path_and_line()
{
        // Paths are compared exactly as decoded by the line table;
        // nothing is normalized or looked up on disk.
        line_program p;
        p.files.push_back("u.c");
        p.set_address(0x100);
        p.copy();               // 0x100 line 1
        p.special(4, -1);       // 0x104 line 0
        p.advance_pc(4);
        p.end_sequence();       // 0x108
        bytes b = p.build();
        dwarf::line_table t = make_table(b);
        dwarf::line_index idx(t);

        TASK_CHECK(idx.find_line("/src/u.c", 1).size() == 1);
        // Line 0 rows are never breakpoint candidates...
        TASK_CHECK(idx.find_line("/src/u.c", 0).empty());
        // ...but they still cover their coordinates.
        auto r = idx.find(0x104);
        TASK_CHECK(r.size() == 1 && r[0].line == 0);

        // No normalization, no disk access.
        TASK_CHECK(idx.find_line("u.c", 1).empty());
        TASK_CHECK(idx.find_line("/src/./u.c", 1).empty());
        TASK_CHECK(idx.find_line("/src/../src/u.c", 1).empty());
        TASK_CHECK(idx.find_line("/src/other.c", 1).empty());
        return 0;
}

// ----------------------------------------------------------------
// ownership

int
case_dynamic_files_and_lifetime()
{
        // define_file extends the file table in the middle of the
        // line number program.
        line_program p;
        p.files.push_back("u.c");
        p.set_address(0x200);
        p.advance_line(4);
        p.copy();               // 0x200 file 1 ("/src/u.c") line 5
        p.define_file("gen.c"); // file 2: "/src/gen.c"
        p.set_file(2);
        p.advance_pc(8);
        p.advance_line(2);
        p.copy();               // 0x208 file 2 ("/src/gen.c") line 7
        p.advance_pc(8);
        p.end_sequence();       // 0x210

        dwarf::line_index idx;
        {
                bytes b = p.build();
                dwarf::line_table t = make_table(b);
                // The index is built once, here, while the table,
                // the program bytes, and any loader are alive.
                idx = dwarf::line_index(t);
        }
        // The table, the section, and the program bytes are gone;
        // the index answers from its own copies.

        TASK_CHECK(idx.valid());
        auto r = idx.find(0x208);
        TASK_CHECK(r.size() == 1);
        TASK_CHECK(r[0].file == "/src/gen.c" && r[0].line == 7);
        TASK_CHECK(r[0].begin_address == 0x208 && r[0].end_address == 0x210);
        r = idx.find(0x200);
        TASK_CHECK(r.size() == 1 && r[0].file == "/src/u.c" && r[0].line == 5);

        auto bp = idx.find_line("/src/gen.c", 7);
        TASK_CHECK(bp.size() == 1 && bp[0].begin_address == 0x208);
        bp = idx.find_line("/src/u.c", 5);
        TASK_CHECK(bp.size() == 1 && bp[0].begin_address == 0x200);
        return 0;
}

int
case_index_copies()
{
        // A default-constructed index is invalid and answers empty.
        dwarf::line_index empty;
        TASK_CHECK(!empty.valid());
        TASK_CHECK(empty.find(0x100).empty());
        TASK_CHECK(empty.find_line("/src/u.c", 1).empty());

        bytes b = fixture::ordinary();
        dwarf::line_table t = fixture::table(b);
        dwarf::line_index idx(t);
        TASK_CHECK(idx.valid());

        // Copies share the indexed data and stay valid
        // independently of the original.
        dwarf::line_index copy(idx);
        dwarf::line_index assigned;
        assigned = idx;
        TASK_CHECK(copy.valid() && assigned.valid());
        TASK_CHECK(copy.find(0x102).size() == 1);
        TASK_CHECK(assigned.find_line("/src/u.c", 2).size() == 1);

        // Moving transfers the index.
        dwarf::line_index moved(std::move(copy));
        TASK_CHECK(moved.valid());
        TASK_CHECK(moved.find(0x102).size() == 1);
        return 0;
}

} // anonymous namespace

const task_case task_cases[] = {
        {"operation_ranges", "op_slots_and_equal_coordinates",
         case_op_slots_and_equal_coordinates},
        {"operation_ranges", "overlapping_shuffled_sequences",
         case_overlapping_shuffled_sequences},
        {"operation_ranges", "sequence_tail_termination",
         case_sequence_tail_termination},
        {"operation_ranges", "malformed_programs",
         case_malformed_programs},
        {"source_sites", "breakpoints_for_line",
         case_breakpoints_for_line},
        {"source_sites", "exact_path_and_line",
         case_exact_path_and_line},
        {"ownership", "dynamic_files_and_lifetime",
         case_dynamic_files_and_lifetime},
        {"ownership", "index_copies",
         case_index_copies},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
