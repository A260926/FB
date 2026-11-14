// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#ifndef _DWARFPP_LINE_INDEX_HH_
#define _DWARFPP_LINE_INDEX_HH_

#include "dwarf++.hh"

#include <memory>
#include <string>
#include <vector>

DWARFPP_BEGIN_NAMESPACE

/**
 * An owning index over the source intervals described by a DWARF
 * line number table.
 *
 * A line number table maps operation coordinates---(address,
 * op_index) pairs, ordered lexicographically---to source locations.
 * This index materializes each sequence of a line_table into
 * half-open source intervals once, copies the file information it
 * needs, and then answers queries in both directions without
 * re-running the line number program: line_index::find returns
 * every interval covering an operation coordinate and
 * line_index::find_line returns the intervals where breakpoints
 * can be set for a source line.
 *
 * Intervals are constructed as follows.  Each sequence of the
 * table forms its own half-open intervals: an ordinary row covers
 * all coordinates from its own coordinate up to the next strictly
 * greater coordinate of its sequence.  Ordinary rows that share a
 * coordinate are kept individually and share the same right
 * boundary.  The last rows of a sequence are bounded by the
 * sequence's end_sequence coordinate; the terminating row itself
 * describes no source.  Empty intervals are omitted and intervals
 * never extend into the next sequence.  It is a format_error if
 * coordinates move backwards within a sequence or if a sequence is
 * not terminated by an end_sequence row.
 *
 * The index copies everything it needs from the line table,
 * including files defined in the middle of the line number
 * program; the table, its section data, and its loader may all be
 * released once the index is constructed.
 */
class line_index
{
public:
        /**
         * A half-open source interval over operation coordinates.
         */
        class interval
        {
        public:
                /**
                 * The first operation coordinate covered by this
                 * interval.
                 */
                taddr begin_address;
                unsigned begin_op_index;

                /**
                 * The operation coordinate bounding this interval
                 * on the right.  The interval covers all
                 * coordinates lexicographically smaller than this
                 * one.
                 */
                taddr end_address;
                unsigned end_op_index;

                /**
                 * The absolute path of the source file, as decoded
                 * by the line table.
                 */
                std::string file;

                /**
                 * The source line number of this interval.  This
                 * may be 0 if the operations cannot be attributed
                 * to any source line.
                 */
                unsigned line;

                /**
                 * The column number within the source line or 0.
                 */
                unsigned column;

                /**
                 * The discriminator of the block containing these
                 * operations.
                 */
                unsigned discriminator;

                /**
                 * True if the beginning of this interval is a
                 * recommended breakpoint location.
                 */
                bool is_stmt;

                /**
                 * The index of the line table sequence containing
                 * this interval, counting from 0.
                 */
                unsigned sequence;

                /**
                 * The index of this interval's row within its
                 * sequence, counting ordinary rows from 0.
                 */
                unsigned row;
        };

        /**
         * Construct an index over the given line number table.
         * Throws format_error if a sequence's coordinates move
         * backwards or a sequence is not terminated by an
         * end_sequence row.
         */
        line_index(const line_table &table);

        /**
         * Construct an invalid, empty index.
         */
        line_index() = default;

        /** Copy constructor */
        line_index(const line_index &o) = default;
        /** Move constructor */
        line_index(line_index &&o) = default;

        line_index &operator=(const line_index &o) = default;
        line_index &operator=(line_index &&o) = default;

        /**
         * Return true if this object represents an initialized
         * index.  Default constructed indexes are not valid.
         */
        bool valid() const
        {
                return !!m;
        }

        /**
         * Return every source interval covering the operation
         * coordinate (address, op_index), in sequence and row
         * order.  Intervals from overlapping sequences, intervals
         * that are not recommended breakpoint locations, and
         * intervals with line number 0 are all included.
         */
        std::vector<interval>
        find(taddr address, unsigned op_index = 0) const;

        /**
         * Return the breakpoint locations for source line `line`
         * of the file at absolute path `path`.  Every non-empty
         * interval of that line that is a recommended breakpoint
         * location contributes its interval; the interval's begin
         * coordinate is where a breakpoint can be set.  Intervals
         * that differ only in column, discriminator, or sequence
         * are returned separately.  The results are ordered by
         * begin coordinate, then by sequence and row.  The path is
         * compared exactly, as decoded by the line table; no files
         * are consulted and no other line is searched.
         */
        std::vector<interval>
        find_line(const std::string &path, unsigned line) const;

private:
        struct impl;
        std::shared_ptr<impl> m;
};

DWARFPP_END_NAMESPACE

#endif
