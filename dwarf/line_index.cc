// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "line_index.hh"

#include <algorithm>
#include <map>
#include <utility>

using namespace std;

DWARFPP_BEGIN_NAMESPACE

// Operation coordinates are (address, op_index) pairs, ordered
// lexicographically.
static bool
coordinate_less(taddr address1, unsigned op_index1,
                taddr address2, unsigned op_index2)
{
        return address1 < address2 ||
                (address1 == address2 && op_index1 < op_index2);
}

struct line_index::impl
{
        // A materialized source interval.  This mirrors
        // line_index::interval, but refers to files by index.
        struct record
        {
                taddr begin_address;
                unsigned begin_op_index;
                taddr end_address;
                unsigned end_op_index;
                unsigned file_index;
                unsigned line;
                unsigned column;
                unsigned discriminator;
                bool is_stmt;
                unsigned sequence;
                unsigned row;
        };

        // All non-empty intervals, in program order.  The
        // intervals of sequence i are records[sequences[i].first,
        // sequences[i].second), ordered by non-decreasing begin
        // coordinate; intervals that begin at the same coordinate
        // are adjacent and share their end coordinate.
        vector<record> records;
        vector<pair<size_t, size_t> > sequences;

        // File paths copied from the line table, by file index.
        vector<string> files;

        // For each (file, line), the records that are recommended
        // breakpoint locations, ordered by begin coordinate, then
        // by sequence and row.
        map<pair<string, unsigned>, vector<unsigned> > lines;

        interval to_interval(const record &rec) const
        {
                interval iv;
                iv.begin_address = rec.begin_address;
                iv.begin_op_index = rec.begin_op_index;
                iv.end_address = rec.end_address;
                iv.end_op_index = rec.end_op_index;
                iv.file = files[rec.file_index];
                iv.line = rec.line;
                iv.column = rec.column;
                iv.discriminator = rec.discriminator;
                iv.is_stmt = rec.is_stmt;
                iv.sequence = rec.sequence;
                iv.row = rec.row;
                return iv;
        }
};

line_index::line_index(const line_table &table)
        : m(make_shared<impl>())
{
        if (!table.valid())
                return;

        // Ordinary rows that share the current greatest coordinate
        // of their sequence.  Their intervals stay open until the
        // next strictly greater coordinate or the end of the
        // sequence gives them a right boundary.
        struct open_row
        {
                unsigned file_index;
                unsigned line;
                unsigned column;
                unsigned discriminator;
                bool is_stmt;
                unsigned row;
        };
        vector<open_row> open;
        bool have_open = false;
        taddr open_address = 0;
        unsigned open_op_index = 0;
        unsigned sequence = 0, row = 0;
        size_t sequence_begin = 0;

        // Close the pending group of rows with the given right
        // boundary, materializing their intervals.
        auto close = [&](taddr end_address, unsigned end_op_index) {
                if (!have_open)
                        return;
                // Empty intervals are not materialized.
                if (open_address != end_address ||
                    open_op_index != end_op_index) {
                        for (const open_row &r : open) {
                                impl::record rec;
                                rec.begin_address = open_address;
                                rec.begin_op_index = open_op_index;
                                rec.end_address = end_address;
                                rec.end_op_index = end_op_index;
                                rec.file_index = r.file_index;
                                rec.line = r.line;
                                rec.column = r.column;
                                rec.discriminator = r.discriminator;
                                rec.is_stmt = r.is_stmt;
                                rec.sequence = sequence;
                                rec.row = r.row;
                                m->records.push_back(rec);
                        }
                }
                open.clear();
                have_open = false;
        };

        for (const auto &ent : table) {
                if (ent.end_sequence) {
                        // The terminating row bounds the last group
                        // of its sequence but describes no source
                        // itself.
                        if (have_open &&
                            coordinate_less(ent.address, ent.op_index,
                                            open_address, open_op_index))
                                throw format_error(
                                        "line table sequence moved backwards");
                        close(ent.address, ent.op_index);
                        m->sequences.emplace_back(sequence_begin,
                                                  m->records.size());
                        sequence_begin = m->records.size();
                        sequence++;
                        row = 0;
                        continue;
                }

                if (have_open &&
                    coordinate_less(ent.address, ent.op_index,
                                    open_address, open_op_index))
                        throw format_error(
                                "line table sequence moved backwards");
                if (have_open &&
                    coordinate_less(open_address, open_op_index,
                                    ent.address, ent.op_index))
                        close(ent.address, ent.op_index);
                if (!have_open) {
                        open_address = ent.address;
                        open_op_index = ent.op_index;
                        have_open = true;
                }

                // Copy file information on demand.  define_file may
                // grow the table's file list in the middle of the
                // line number program.
                while (m->files.size() <= ent.file_index)
                        m->files.push_back(
                                table.get_file(m->files.size())->path);

                open_row r;
                r.file_index = ent.file_index;
                r.line = ent.line;
                r.column = ent.column;
                r.discriminator = ent.discriminator;
                r.is_stmt = ent.is_stmt;
                r.row = row++;
                open.push_back(r);
        }

        if (have_open)
                throw format_error(
                        "line table sequence missing end_sequence");

        // Index breakpoint candidates by source line.
        for (size_t i = 0; i < m->records.size(); i++) {
                const impl::record &rec = m->records[i];
                if (!rec.is_stmt || rec.line == 0)
                        continue;
                m->lines[make_pair(m->files[rec.file_index], rec.line)]
                        .push_back((unsigned)i);
        }
        for (auto &kv : m->lines) {
                stable_sort(kv.second.begin(), kv.second.end(),
                            [&](unsigned a, unsigned b) {
                        const impl::record &ra = m->records[a];
                        const impl::record &rb = m->records[b];
                        if (ra.begin_address != rb.begin_address)
                                return ra.begin_address < rb.begin_address;
                        if (ra.begin_op_index != rb.begin_op_index)
                                return ra.begin_op_index < rb.begin_op_index;
                        if (ra.sequence != rb.sequence)
                                return ra.sequence < rb.sequence;
                        return ra.row < rb.row;
                });
        }
}

std::vector<line_index::interval>
line_index::find(taddr address, unsigned op_index) const
{
        vector<interval> result;
        if (!m)
                return result;

        for (const auto &seq : m->sequences) {
                // Locate the group of rows beginning at the
                // greatest begin coordinate not past the query
                // coordinate.
                size_t lo = seq.first, hi = seq.second;
                while (lo < hi) {
                        size_t mid = lo + (hi - lo) / 2;
                        const impl::record &rec = m->records[mid];
                        if (coordinate_less(address, op_index,
                                            rec.begin_address,
                                            rec.begin_op_index))
                                hi = mid;
                        else
                                lo = mid + 1;
                }
                if (lo == seq.first)
                        continue;
                const impl::record &group = m->records[lo - 1];
                // The group shares its right boundary; if the query
                // coordinate is not before it, this sequence does
                // not cover the coordinate.
                if (!coordinate_less(address, op_index,
                                     group.end_address, group.end_op_index))
                        continue;
                // Collect every row sharing the group's begin
                // coordinate, in row order.
                size_t first = lo - 1;
                while (first > seq.first &&
                       m->records[first - 1].begin_address ==
                               group.begin_address &&
                       m->records[first - 1].begin_op_index ==
                               group.begin_op_index)
                        first--;
                for (size_t i = first; i < lo; i++)
                        result.push_back(m->to_interval(m->records[i]));
        }
        return result;
}

std::vector<line_index::interval>
line_index::find_line(const std::string &path, unsigned line) const
{
        vector<interval> result;
        if (!m || line == 0)
                return result;
        auto it = m->lines.find(make_pair(path, line));
        if (it == m->lines.end())
                return result;
        for (unsigned i : it->second)
                result.push_back(m->to_interval(m->records[i]));
        return result;
}

DWARFPP_END_NAMESPACE
