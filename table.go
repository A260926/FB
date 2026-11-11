package goquery

import (
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"

	"golang.org/x/net/html"
)

// errOverflow is returned by the integer helpers when an addition or
// multiplication would not fit in an int.
var errOverflow = errors.New("integer overflow")

// defaultTableGridSlots is the TableGrid cell budget used when maxSlots is 0.
const defaultTableGridSlots = 65536

// asciiWhitespace is the set of ASCII whitespace characters defined by HTML.
const asciiWhitespace = " \t\r\n\f"

// TableSlot is one position of the two-dimensional grid returned by
// Selection.TableGrid. It is a value type: a grid built from the DOM is an
// independent snapshot, so keeping or modifying it never affects the document
// it was extracted from.
type TableSlot struct {
	// Present reports whether the slot is covered by a source th or td cell,
	// including slots reached through that cell's colspan or rowspan. A false
	// value marks a padding slot that no source cell covers; every other field
	// then holds its zero value.
	Present bool

	// Header reports whether the source cell covering the slot is a th
	// element. Slots covered by a td element have it set to false.
	Header bool

	// Text holds the text of the source cell covering the slot. Text nodes are
	// merged in DOM order, br elements contribute a space, nested tables as
	// well as script and style subtrees are skipped, and runs of ASCII
	// whitespace are collapsed and trimmed at both ends.
	Text string

	// OriginRow is the zero-based row, counted across the whole table, of the
	// top-left corner of the source cell covering the slot. Every slot of a
	// colspan/rowspan rectangle shares the same origin coordinates.
	OriginRow int

	// OriginCol is the zero-based column of the top-left corner of the source
	// cell covering the slot.
	OriginCol int
}

// TableGrid flattens the single table element held by the selection into a
// rectangular, zero-based [][]TableSlot grid that records the source cell of
// every covered position.
//
// The receiver must contain exactly one node, and that node must be a table
// element; otherwise TableGrid returns an error and a nil grid. The document is
// only read, never modified, and the returned grid does not share any mutable
// state with the DOM.
//
// Rows are taken from the direct tr children of the table's thead, tbody and
// tfoot elements, in DOM order, plus any consecutive run of tr elements that
// are direct children of the table itself, which is treated as its own row
// group. Rows belonging to nested tables, as well as caption and colgroup
// elements, are never counted. Within a row only the direct th and td children
// are read; th cells are flagged through TableSlot.Header.
//
// Each row group is laid out independently, starting with the cursor on its
// first row. A source cell advances the row cursor to the first free slot and
// fills the rectangle described by its colspan and rowspan attributes; every
// slot of that rectangle records the coordinates of the cell's top-left
// corner. A rectangle that overlaps a slot already occupied by an earlier cell
// is an error instead of being silently shifted. Missing spans default to 1,
// span attributes must be trimmed decimal non-negative integers (colspan must
// be at least 1), a rowspan of 0 covers the remaining rows of the current row
// group, and a positive rowspan cannot extend past its group. Rows fully
// covered by earlier rowspans, and completely empty rows, are still emitted,
// and every row is padded to the width of the widest row with slots whose
// Present field is false. Rowspans never carry over into the next group.
//
// maxSlots bounds the total height-times-width of the returned grid. A value
// of 0 uses the default of 65536, a negative value is an error, and any table
// whose layout would exceed the budget (during span parsing, coordinate
// arithmetic or the final rectangle) returns an error and a nil grid without
// allocating the oversized rows. A table without any rows yields an empty,
// non-nil grid.
func (s *Selection) TableGrid(maxSlots int) ([][]TableSlot, error) {
	if len(s.Nodes) != 1 {
		return nil, fmt.Errorf("goquery: TableGrid: selection must contain exactly one table node, has %d", len(s.Nodes))
	}
	table := s.Nodes[0]
	if table.Type != html.ElementNode || table.Data != "table" {
		return nil, errors.New("goquery: TableGrid: selected node is not a table element")
	}
	if maxSlots < 0 {
		return nil, errors.New("goquery: TableGrid: maxSlots must not be negative")
	}
	if maxSlots == 0 {
		maxSlots = defaultTableGridSlots
	}

	groups := collectTableGroups(table)

	totalRows := 0
	for _, g := range groups {
		next, err := addInt(totalRows, len(g.rows))
		if err != nil {
			return nil, errors.New("goquery: TableGrid: table height overflows int")
		}
		totalRows = next
	}
	if totalRows == 0 {
		return [][]TableSlot{}, nil
	}
	if totalRows > maxSlots {
		return nil, fmt.Errorf("goquery: TableGrid: table has %d rows, exceeding maxSlots of %d", totalRows, maxSlots)
	}

	grid := make([][]TableSlot, totalRows)
	b := &tableGridBuilder{
		grid:      grid,
		maxSlots:  maxSlots,
		totalRows: totalRows,
	}

	for _, g := range groups {
		if err := b.layoutGroup(g); err != nil {
			return nil, err
		}
	}

	// Make the height-times-width guarantee explicit before padding.
	area, err := mulInt(totalRows, b.width)
	if err != nil || area > maxSlots {
		return nil, fmt.Errorf("goquery: TableGrid: grid of %d rows x %d columns exceeds maxSlots of %d", totalRows, b.width, maxSlots)
	}
	for r := range grid {
		if grid[r] == nil {
			// Keep rows that exist in the DOM but carry no column, instead of
			// leaving a nil slice indistinguishable from "no such row".
			grid[r] = []TableSlot{}
		}
		if len(grid[r]) < b.width {
			grid[r] = append(grid[r], make([]TableSlot, b.width-len(grid[r]))...)
		}
	}
	return grid, nil
}

// tableGridBuilder holds the incremental layout state. Row slices grow on
// demand so that an oversized span fails the budget checks before any large
// backing array is allocated.
type tableGridBuilder struct {
	grid      [][]TableSlot
	width     int // widest row reached so far
	slots     int // number of Present slots written so far
	maxSlots  int
	totalRows int
}

// layoutGroup places the cells of one row group. Row indices are absolute
// across the whole table.
func (b *tableGridBuilder) layoutGroup(g *tableGroup) error {
	groupHeight := len(g.rows)
	for ri, row := range g.rows {
		r := g.startRow + ri
		cursor := 0
		for ci, cell := range row.cells {
			if cell.parseErr != nil {
				return fmt.Errorf("goquery: TableGrid: row %d cell %d: %w", r, ci, cell.parseErr)
			}

			for b.isOccupied(r, cursor) {
				cursor++
			}

			endCol, err := addInt(cursor, cell.colSpan)
			if err != nil {
				return fmt.Errorf("goquery: TableGrid: row %d cell %d: column coordinate overflows int", r, ci)
			}

			rowSpan := cell.rowSpan
			if rowSpan == 0 {
				// rowspan=0 fills the remainder of this row group.
				rowSpan = groupHeight - ri
			} else {
				limit, err := addInt(ri, rowSpan)
				if err != nil || limit > groupHeight {
					return fmt.Errorf("goquery: TableGrid: row %d cell %d: rowspan %d extends past its row group", r, ci, cell.rowSpan)
				}
			}
			endRow, err := addInt(r, rowSpan)
			if err != nil {
				return fmt.Errorf("goquery: TableGrid: row %d cell %d: row coordinate overflows int", r, ci)
			}

			rectArea, err := mulInt(rowSpan, cell.colSpan)
			if err != nil {
				return fmt.Errorf("goquery: TableGrid: row %d cell %d: cell rectangle overflows int", r, ci)
			}
			filled, err := addInt(b.slots, rectArea)
			if err != nil || filled > b.maxSlots {
				return fmt.Errorf("goquery: TableGrid: row %d cell %d: table exceeds maxSlots of %d", r, ci, b.maxSlots)
			}
			// Bounding every row slice by endCol keeps the total backing
			// memory at or below totalRows*maxSlots even before final padding.
			boundedWidth, err := mulInt(b.totalRows, endCol)
			if err != nil || boundedWidth > b.maxSlots {
				return fmt.Errorf("goquery: TableGrid: row %d cell %d: grid of %d rows x %d columns exceeds maxSlots of %d", r, ci, b.totalRows, endCol, b.maxSlots)
			}

			// Reject overlaps before writing anything: the cell must never be
			// silently pushed to the right as a whole.
			for rr := r; rr < endRow; rr++ {
				for cc := cursor; cc < endCol; cc++ {
					if b.isOccupied(rr, cc) {
						return fmt.Errorf("goquery: TableGrid: row %d cell %d: rectangle overlaps an occupied slot at (%d,%d)", r, ci, rr, cc)
					}
				}
			}

			slot := TableSlot{
				Present:   true,
				Header:    cell.header,
				Text:      cell.text,
				OriginRow: r,
				OriginCol: cursor,
			}
			for rr := r; rr < endRow; rr++ {
				if len(b.grid[rr]) < endCol {
					b.grid[rr] = append(b.grid[rr], make([]TableSlot, endCol-len(b.grid[rr]))...)
				}
				for cc := cursor; cc < endCol; cc++ {
					b.grid[rr][cc] = slot
				}
			}
			b.slots = filled
			if endCol > b.width {
				b.width = endCol
			}
			cursor = endCol
		}
	}
	return nil
}

func (b *tableGridBuilder) isOccupied(r, c int) bool {
	row := b.grid[r]
	return c < len(row) && row[c].Present
}

// tableGroup is one layout scope: the rows of a thead/tbody/tfoot element, or a
// consecutive run of table-direct tr elements. Rowspans never cross groups.
type tableGroup struct {
	startRow int
	rows     []*tableRow
}

type tableRow struct {
	cells []*tableSourceCell
}

type tableSourceCell struct {
	header   bool
	text     string
	colSpan  int
	rowSpan  int
	parseErr error
}

// collectTableGroups walks the direct children of the table in DOM order and
// builds its row groups. Nested tables cannot appear here because only direct
// children are inspected, and caption/colgroup elements are ignored.
func collectTableGroups(table *html.Node) []*tableGroup {
	var groups []*tableGroup
	var loose *tableGroup

	for c := table.FirstChild; c != nil; c = c.NextSibling {
		if c.Type != html.ElementNode {
			continue
		}
		switch c.Data {
		case "thead", "tbody", "tfoot":
			loose = nil
			g := &tableGroup{}
			for tr := c.FirstChild; tr != nil; tr = tr.NextSibling {
				if tr.Type == html.ElementNode && tr.Data == "tr" {
					g.rows = append(g.rows, parseTableRow(tr))
				}
			}
			if len(g.rows) > 0 {
				groups = append(groups, g)
			}
		case "tr":
			if loose == nil {
				loose = &tableGroup{}
				groups = append(groups, loose)
			}
			loose.rows = append(loose.rows, parseTableRow(c))
		default:
			// caption, colgroup or anything else breaks a run of loose rows.
			loose = nil
		}
	}

	startRow := 0
	for _, g := range groups {
		g.startRow = startRow
		startRow += len(g.rows)
	}
	return groups
}

// parseTableRow reads only the direct th/td children of a tr.
func parseTableRow(tr *html.Node) *tableRow {
	row := &tableRow{}
	for c := tr.FirstChild; c != nil; c = c.NextSibling {
		if c.Type != html.ElementNode || (c.Data != "th" && c.Data != "td") {
			continue
		}
		cell := &tableSourceCell{
			header: c.Data == "th",
			text:   tableCellText(c),
		}
		colSpan, err := parseSpanAttr(c, "colspan", false)
		if err != nil {
			cell.parseErr = fmt.Errorf("colspan: %w", err)
			row.cells = append(row.cells, cell)
			continue
		}
		rowSpan, err := parseSpanAttr(c, "rowspan", true)
		if err != nil {
			cell.parseErr = fmt.Errorf("rowspan: %w", err)
			row.cells = append(row.cells, cell)
			continue
		}
		cell.colSpan = colSpan
		cell.rowSpan = rowSpan
		row.cells = append(row.cells, cell)
	}
	return row
}

// parseSpanAttr resolves a colspan/rowspan attribute. A missing attribute
// defaults to 1; a present attribute must be a decimal non-negative integer
// after trimming ASCII whitespace. allowZero is false for colspan, where a zero
// value is invalid; rowspan keeps zero for the "rest of the group" semantics.
func parseSpanAttr(n *html.Node, attr string, allowZero bool) (int, error) {
	attrNode := getAttributePtr(attr, n)
	if attrNode == nil {
		return 1, nil
	}
	raw := strings.Trim(attrNode.Val, asciiWhitespace)
	if raw == "" {
		return 0, fmt.Errorf("%s attribute is not a non-negative decimal integer", attr)
	}
	for _, r := range raw {
		if r < '0' || r > '9' {
			return 0, fmt.Errorf("%s attribute %q is not a non-negative decimal integer", attr, attrNode.Val)
		}
	}
	value, err := strconv.Atoi(raw)
	if err != nil {
		// strconv catches values that do not fit in int.
		return 0, fmt.Errorf("%s attribute %q overflows int", attr, attrNode.Val)
	}
	if !allowZero && value == 0 {
		return 0, fmt.Errorf("%s attribute must be at least 1", attr)
	}
	return value, nil
}

// tableCellText extracts the visible text of a th/td cell. br elements become
// spaces, nested tables and script/style subtrees are skipped, and the result
// has its ASCII whitespace collapsed and trimmed at both ends.
func tableCellText(cell *html.Node) string {
	var builder strings.Builder
	var walk func(*html.Node)
	walk = func(n *html.Node) {
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			if c.Type == html.ElementNode {
				switch c.Data {
				case "table", "script", "style":
					continue
				case "br":
					builder.WriteByte(' ')
					continue
				}
			}
			if c.Type == html.TextNode {
				builder.WriteString(c.Data)
			}
			walk(c)
		}
	}
	walk(cell)
	return collapseASCIIWhitespace(builder.String())
}

// collapseASCIIWhitespace trims ASCII whitespace at both ends and replaces
// every internal run of it with a single space.
func collapseASCIIWhitespace(s string) string {
	var builder strings.Builder
	inRun := false
	started := false
	for _, r := range s {
		if isASCIIWhitespace(r) {
			inRun = true
			continue
		}
		if inRun && started {
			builder.WriteByte(' ')
		}
		builder.WriteRune(r)
		started = true
		inRun = false
	}
	return builder.String()
}

func isASCIIWhitespace(r rune) bool {
	switch r {
	case ' ', '\t', '\n', '\r', '\f':
		return true
	}
	return false
}

// addInt returns a+b or an error if the addition overflows int.
func addInt(a, b int) (int, error) {
	if b > 0 && a > math.MaxInt-b {
		return 0, errOverflow
	}
	if b < 0 && a < math.MinInt-b {
		return 0, errOverflow
	}
	return a + b, nil
}

// mulInt returns a*b or an error if the multiplication overflows int.
func mulInt(a, b int) (int, error) {
	if a == 0 || b == 0 {
		return 0, nil
	}
	// MinInt/-1 has no representable result and can defeat the divide-back
	// check because MinInt/-1 wraps back to MinInt in integer division.
	if (a == math.MinInt && b == -1) || (b == math.MinInt && a == -1) {
		return 0, errOverflow
	}
	result := a * b
	if result/a != b {
		return 0, errOverflow
	}
	return result, nil
}
