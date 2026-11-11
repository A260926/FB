package goquery

import (
	"math"
	"strings"
	"testing"
)

func parseTable(t *testing.T, markup string) *Selection {
	t.Helper()
	doc, err := NewDocumentFromReader(strings.NewReader(markup))
	if err != nil {
		t.Fatalf("parse document: %v", err)
	}
	return doc.Find("table").First()
}

func zeroSlot() TableSlot { return TableSlot{} }

func TestTableGridSpecExample(t *testing.T) {
	markup := `<table>
<thead>
<tr><th rowspan="0">站点</th><th colspan="2">流量</th></tr>
<tr><th>入口</th><th>出口</th></tr>
</thead>
<tbody>
<tr><td>A</td><td>10</td></tr>
</tbody>
</table>`
	grid, err := parseTable(t, markup).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	if len(grid) != 3 {
		t.Fatalf("rows = %d, want 3", len(grid))
	}
	for r, row := range grid {
		if len(row) != 3 {
			t.Fatalf("row %d width = %d, want 3", r, len(row))
		}
	}

	first := grid[0][0]
	if !first.Present || !first.Header || first.Text != "站点" || first.OriginRow != 0 || first.OriginCol != 0 {
		t.Fatalf("unexpected (0,0): %+v", first)
	}
	// rowspan=0 covers every remaining row of the thead group only.
	if got := grid[1][0]; !got.Present || !got.Header || got.Text != "站点" || got.OriginRow != 0 || got.OriginCol != 0 {
		t.Fatalf("unexpected (1,0): %+v", got)
	}
	traffic := grid[0][1]
	if !traffic.Present || !traffic.Header || traffic.Text != "流量" || traffic.OriginRow != 0 || traffic.OriginCol != 1 {
		t.Fatalf("unexpected (0,1): %+v", traffic)
	}
	if got := grid[0][2]; got != traffic {
		t.Fatalf("colspan slot (0,2) = %+v, want %+v", got, traffic)
	}
	if got := grid[1][1]; !got.Present || !got.Header || got.Text != "入口" || got.OriginRow != 1 || got.OriginCol != 1 {
		t.Fatalf("unexpected (1,1): %+v", got)
	}
	if got := grid[1][2]; !got.Present || !got.Header || got.Text != "出口" || got.OriginRow != 1 || got.OriginCol != 2 {
		t.Fatalf("unexpected (1,2): %+v", got)
	}
	// The tbody row does not inherit the thead's rowspan.
	a := grid[2][0]
	if !a.Present || a.Header || a.Text != "A" || a.OriginRow != 2 || a.OriginCol != 0 {
		t.Fatalf("unexpected (2,0): %+v", a)
	}
	if got := grid[2][1]; !got.Present || got.Header || got.Text != "10" || got.OriginRow != 2 || got.OriginCol != 1 {
		t.Fatalf("unexpected (2,1): %+v", got)
	}
	if got := grid[2][2]; got != zeroSlot() {
		t.Fatalf("padding slot (2,2) = %+v, want zero value", got)
	}
}

func TestTableGridTrailingPlaceholderConflict(t *testing.T) {
	// The rowspan on column 1 is a placeholder behind the next row's cell;
	// its colspan=2 rectangle runs into that occupied slot.
	markup := `<table><tbody>
<tr><td>a</td><td rowspan="2">b</td><td>c</td></tr>
<tr><td colspan="2">d</td><td>e</td></tr>
</tbody></table>`
	grid, err := parseTable(t, markup).TableGrid(0)
	if err == nil {
		t.Fatalf("want overlap error, got grid %v", grid)
	}
	if grid != nil {
		t.Fatalf("grid on error must be nil, got %v", grid)
	}
	if !strings.Contains(err.Error(), "row 1") || !strings.Contains(err.Error(), "cell 0") {
		t.Fatalf("error must locate source row and cell index, got %q", err)
	}
	if !strings.Contains(err.Error(), "(1,1)") {
		t.Fatalf("error must name the collided slot, got %q", err)
	}
}

func TestTableGridSpanGroupBoundaries(t *testing.T) {
	// A positive rowspan cannot reach past its own row group.
	markup := `<table>
<thead><tr><th rowspan="2">h</th></tr></thead>
<tbody><tr><td>x</td></tr></tbody>
</table>`
	if grid, err := parseTable(t, markup).TableGrid(0); err == nil {
		t.Fatalf("want rowspan-past-group error, got %v", grid)
	}

	// rowspan=0 stays inside a loose-tr group and does not follow into tbody.
	markup = `<table>
<tr><td rowspan="0">loose</td><td>a</td></tr>
<tbody><tr><td>b</td><td>c</td></tr></tbody>
</table>`
	grid, err := parseTable(t, markup).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	if len(grid) != 2 || len(grid[0]) != 2 || len(grid[1]) != 2 {
		t.Fatalf("unexpected shape: %d x %d", len(grid), gridWidth(grid))
	}
	if got := grid[0][0]; got.Text != "loose" || got.OriginRow != 0 || got.OriginCol != 0 {
		t.Fatalf("unexpected loose slot: %+v", got)
	}
	// Explicit assertions for the tbody row: the rowspan never crossed groups.
	if got := grid[1][0]; !got.Present || got.Text != "b" || got.OriginRow != 1 || got.OriginCol != 0 {
		t.Fatalf("unexpected (1,0): %+v", got)
	}
	if got := grid[1][1]; !got.Present || got.Text != "c" || got.OriginRow != 1 || got.OriginCol != 1 {
		t.Fatalf("unexpected (1,1): %+v", got)
	}

	// Multiple tbody sections are independent groups.
	markup = `<table>
<tbody><tr><td rowspan="2">g1</td></tr><tr><td>x</td></tr></tbody>
<tbody><tr><td>g2</td></tr></tbody>
</table>`
	grid, err = parseTable(t, markup).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	if len(grid) != 3 {
		t.Fatalf("rows = %d, want 3", len(grid))
	}
	if got := grid[2][0]; got.Text != "g2" || got.OriginRow != 2 {
		t.Fatalf("second tbody not laid out independently: %+v", got)
	}
}

func TestTableGridNestedTablesEmptyRowsAndText(t *testing.T) {
	markup := `<table id="outer">
<caption>title</caption>
<colgroup><col span="2"/></colgroup>
<thead><tr><th> Site &amp; region </th></tr></thead>
<tbody>
<tr>
<td>
A<br/>B
<script>var x = 1;</script>
<style>.c{}</style>
<table id="inner"><tr><td>nested</td></tr></table>
</td>
</tr>
<tr></tr>
</tbody>
<tfoot><tr><td>foot</td></tr></tfoot>
</table>`
	sel := parseTable(t, markup)
	grid, err := sel.TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	// thead row, two tbody rows (including the fully empty one), tfoot row.
	if len(grid) != 4 {
		t.Fatalf("rows = %d, want 4", len(grid))
	}
	if got := grid[0][0]; !got.Header || got.Text != "Site & region" {
		t.Fatalf("header text = %+v", got)
	}
	if got := grid[1][0]; !got.Present || got.Header || got.Text != "A B" {
		t.Fatalf("cell text = %+v, want collapsed %q without nested/script/style text", got, "A B")
	}
	if got := grid[2]; len(got) != 1 || got[0] != zeroSlot() {
		t.Fatalf("empty row must be retained padded: %+v", got)
	}
	if got := grid[3][0]; got.Text != "foot" || got.Header {
		t.Fatalf("tfoot cell = %+v", got)
	}
	// The DOM is untouched: the nested table and caption are still there.
	if sel.Find("#inner").Length() != 1 || sel.Find("caption").Length() != 1 {
		t.Fatal("TableGrid modified the source DOM")
	}
	if sel.Find("tr").Length() != 5 {
		t.Fatalf("nested table row count changed: %d", sel.Find("tr").Length())
	}
}

func TestTableGridBudget(t *testing.T) {
	// A wide colspan blows a small budget before the big row is allocated.
	markup := `<table><tr><td colspan="1000">wide</td></tr></table>`
	grid, err := parseTable(t, markup).TableGrid(100)
	if err == nil {
		t.Fatalf("want budget error, got %d columns", len(grid[0]))
	}
	if grid != nil {
		t.Fatalf("grid on error must be nil")
	}

	// Negative budgets are rejected.
	if _, err := parseTable(t, `<table><tr><td>a</td></tr></table>`).TableGrid(-1); err == nil {
		t.Fatal("negative maxSlots must error")
	}

	// A table exactly at the default budget is accepted and is rectangular.
	markup = `<table><tr>` + strings.Repeat(`<td>x</td>`, 64) + `</tr>` +
		`<tr>` + strings.Repeat(`<td>y</td>`, 64) + `</tr></table>`
	grid, err = parseTable(t, markup).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid within budget: %v", err)
	}
	if len(grid) != 2 || len(grid[0]) != 64 || len(grid[1]) != 64 {
		t.Fatalf("unexpected shape %d x %d", len(grid), gridWidth(grid))
	}

	// One slot over an explicit budget fails.
	if _, err := parseTable(t, markup).TableGrid(127); err == nil {
		t.Fatal("128-slot table with maxSlots 127 must error")
	}
}

func TestTableGridSpanParsing(t *testing.T) {
	cases := []struct {
		name   string
		attr   string
		errMsg string
	}{
		{"non numeric", `colspan="2x"`, "decimal"},
		{"zero colspan", `colspan="0"`, "at least 1"},
		{"negative rowspan", `rowspan="-1"`, "decimal"},
		{"blank", `colspan="   "`, "decimal"},
		{"overflow", `colspan="999999999999999999999999"`, "overflows int"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			markup := `<table><tr><td ` + tc.attr + `>a</td></tr></table>`
			grid, err := parseTable(t, markup).TableGrid(0)
			if err == nil {
				t.Fatalf("want error, got %v", grid)
			}
			if !strings.Contains(err.Error(), "row 0") || !strings.Contains(err.Error(), "cell 0") {
				t.Fatalf("error must locate row/cell, got %q", err)
			}
			if !strings.Contains(err.Error(), tc.errMsg) {
				t.Fatalf("error %q does not mention %q", err, tc.errMsg)
			}
		})
	}

	// Trimmed decimal strings and unit defaults parse fine.
	markup := `<table><tr><td colspan=" 2 " rowspan=" 2 ">a</td><td>b</td></tr><tr><td>c</td></tr></table>`
	grid, err := parseTable(t, markup).TableGrid(0)
	if err != nil {
		t.Fatalf("trimmed decimal spans should parse: %v", err)
	}
	// "a" fills a 2x2 rectangle; b and c flow into the third column.
	if len(grid) != 2 || len(grid[0]) != 3 || len(grid[1]) != 3 {
		t.Fatalf("unexpected shape: %d x %d", len(grid), gridWidth(grid))
	}
	for r := 0; r < 2; r++ {
		for c := 0; c < 2; c++ {
			if got := grid[r][c]; !got.Present || got.Text != "a" || got.OriginRow != 0 || got.OriginCol != 0 {
				t.Fatalf("(%d,%d) = %+v", r, c, got)
			}
		}
	}
	if got := grid[0][2]; got.Text != "b" || got.OriginRow != 0 || got.OriginCol != 2 {
		t.Fatalf("overflow cell b = %+v", got)
	}
	if got := grid[1][2]; got.Text != "c" || got.OriginRow != 1 || got.OriginCol != 2 {
		t.Fatalf("overflow cell c = %+v", got)
	}
}

func TestTableGridEmptyTables(t *testing.T) {
	grid, err := parseTable(t, `<table></table>`).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	if grid == nil || len(grid) != 0 {
		t.Fatalf("no-row table must return empty non-nil grid, got %v", grid)
	}

	grid, err = parseTable(t, `<table><tbody><tr></tr></tbody></table>`).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	if len(grid) != 1 || grid[0] == nil || len(grid[0]) != 0 {
		t.Fatalf("zero-column row must be retained as empty slice, got %v", grid)
	}
}

func TestTableGridReceiverValidation(t *testing.T) {
	doc, err := NewDocumentFromReader(strings.NewReader(`<div><table></table><table></table></div>`))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := doc.Find("nope").TableGrid(0); err == nil {
		t.Fatal("empty selection must error")
	}
	if _, err := doc.Find("table").TableGrid(0); err == nil {
		t.Fatal("multi-node selection must error")
	}
	if _, err := doc.Find("div").First().TableGrid(0); err == nil {
		t.Fatal("non-table selection must error")
	}
}

func TestTableGridReextractAfterFailure(t *testing.T) {
	doc, err := NewDocumentFromReader(strings.NewReader(`
<html><body>
<table id="bad"><tr><td colspan="0">x</td></tr></table>
<table id="good"><thead><tr><th>h</th></tr></thead><tbody><tr><td>a</td><td>b</td></tr></tbody></table>
</body></html>`))
	if err != nil {
		t.Fatal(err)
	}
	bad := doc.Find("#bad")
	if grid, err := bad.TableGrid(0); err == nil {
		t.Fatalf("bad table must fail, got %v", grid)
	}
	// The failed run leaves the DOM usable for a fresh extraction.
	grid, err := doc.Find("#good").TableGrid(0)
	if err != nil {
		t.Fatalf("re-extraction after failure: %v", err)
	}
	if len(grid) != 2 || grid[0][0].Text != "h" || grid[1][1].Text != "b" {
		t.Fatalf("unexpected grid after re-extraction: %+v", grid)
	}

	// The returned grid is an independent snapshot.
	grid[0][0].Text = "mutated"
	again, err := doc.Find("#good").TableGrid(0)
	if err != nil {
		t.Fatalf("second extraction: %v", err)
	}
	if again[0][0].Text != "h" {
		t.Fatalf("snapshot shares mutable state with DOM/grid: %q", again[0][0].Text)
	}
	if again[1][0].OriginRow != 1 {
		t.Fatalf("unexpected origin after re-extraction: %+v", again[1][0])
	}
}

func TestTableGridWidthPaddingAndRowspan(t *testing.T) {
	markup := `<table><tbody>
<tr><td rowspan="2">tall</td><td>x</td><td>y</td></tr>
<tr><td>z</td></tr>
</tbody></table>`
	grid, err := parseTable(t, markup).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	if len(grid) != 2 || len(grid[0]) != 3 || len(grid[1]) != 3 {
		t.Fatalf("unexpected shape: %d x %d", len(grid), gridWidth(grid))
	}
	if got := grid[1][0]; got.Text != "tall" || got.OriginRow != 0 || got.OriginCol != 0 {
		t.Fatalf("rowspan slot = %+v", got)
	}
	if got := grid[1][1]; got.Text != "z" || got.OriginRow != 1 || got.OriginCol != 1 {
		t.Fatalf("cursor should skip the rowspan slot: %+v", got)
	}
	if got := grid[1][2]; got != zeroSlot() {
		t.Fatalf("short row must be padded with zero slots: %+v", got)
	}
}

func gridWidth(grid [][]TableSlot) int {
	if len(grid) == 0 {
		return 0
	}
	return len(grid[0])
}

func TestTableGridCoveredAndEmptyRows(t *testing.T) {
	// The second row introduces no cell; it must still exist, keep the
	// rowspan-covered slot and be padded where nothing covers it.
	markup := `<table><tbody><tr><td rowspan="2">x</td><td>a</td></tr><tr></tr></tbody></table>`
	grid, err := parseTable(t, markup).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	if len(grid) != 2 || len(grid[1]) != 2 {
		t.Fatalf("unexpected shape: %d x %d", len(grid), gridWidth(grid))
	}
	if got := grid[1][0]; !got.Present || got.Text != "x" || got.OriginRow != 0 || got.OriginCol != 0 {
		t.Fatalf("covered slot = %+v", got)
	}
	if got := grid[1][1]; got.Present {
		t.Fatalf("uncovered slot must be a zero padding slot, got %+v", got)
	}
}

func TestTableGridRowspanZeroRestOfGroup(t *testing.T) {
	markup := `<table><tbody>
<tr><td rowspan="0">x</td></tr>
<tr><td>a</td></tr>
<tr><td>b</td></tr>
</tbody></table>`
	grid, err := parseTable(t, markup).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	if len(grid) != 3 {
		t.Fatalf("rows = %d, want 3", len(grid))
	}
	for r := 1; r <= 2; r++ {
		if got := grid[r][0]; !got.Present || got.Text != "x" || got.OriginRow != 0 || got.OriginCol != 0 {
			t.Fatalf("row %d col 0 = %+v, want rest-of-group x", r, got)
		}
	}
	if grid[1][1].Text != "a" || grid[2][1].Text != "b" {
		t.Fatalf("side cells misplaced: %q %q", grid[1][1].Text, grid[2][1].Text)
	}
}

func TestTableGridSectionOrderAndNestedIsolation(t *testing.T) {
	// Sections are emitted in DOM order even when tfoot precedes thead.
	markup := `<table>
<tbody><tr><td>body</td></tr></tbody>
<tfoot><tr><td>foot</td></tr></tfoot>
<thead><tr><th>head</th></tr></thead>
</table>`
	grid, err := parseTable(t, markup).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	if len(grid) != 3 {
		t.Fatalf("rows = %d, want 3", len(grid))
	}
	if grid[0][0].Text != "body" || grid[1][0].Text != "foot" || grid[2][0].Text != "head" || !grid[2][0].Header {
		t.Fatalf("sections reordered: %q %q %q", grid[0][0].Text, grid[1][0].Text, grid[2][0].Text)
	}

	// Rows of the nested table never enter the outer grid.
	markup = `<table><tbody>
<tr><td>outer1<table><thead><tr><th>inner-h</th></tr></thead><tbody><tr><td>inner</td></tr></tbody></table></td></tr>
<tr><td>outer2</td></tr>
</tbody></table>`
	grid, err = parseTable(t, markup).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	if len(grid) != 2 {
		t.Fatalf("outer rows = %d, want 2", len(grid))
	}
	if grid[0][0].Text != "outer1" || grid[1][0].Text != "outer2" || grid[1][0].OriginRow != 1 {
		t.Fatalf("nested rows leaked into outer grid: %+v %+v", grid[0][0], grid[1][0])
	}
}

func TestTableGridLooseGroupsSplitAroundSection(t *testing.T) {
	// Loose tr runs before and after a tbody are distinct groups, so the
	// rowspan=0 on the first run does not cover the later rows.
	markup := `<table>
<tr><td rowspan="0">l1</td></tr>
<tbody><tr><td>mid</td></tr></tbody>
<tr><td>l2</td></tr>
</table>`
	grid, err := parseTable(t, markup).TableGrid(0)
	if err != nil {
		t.Fatalf("TableGrid: %v", err)
	}
	if len(grid) != 3 {
		t.Fatalf("rows = %d, want 3", len(grid))
	}
	want := []string{"l1", "mid", "l2"}
	for r, w := range want {
		if grid[r][0].Text != w || grid[r][0].OriginRow != r {
			t.Fatalf("row %d = %+v, want text %q origin row %d", r, grid[r][0], w, r)
		}
	}
}

func TestTableGridIntegerOverflowGuards(t *testing.T) {
	addCases := []struct {
		a, b  int
		valid bool
	}{
		{0, 0, true},
		{1, 2, true},
		{math.MaxInt, 0, true},
		{math.MaxInt - 1, 1, true},
		{math.MaxInt, 1, false},
		{math.MaxInt, 2, false},
		{math.MinInt, 0, true},
		{math.MinInt, -1, false},
		{math.MinInt, 1, true},
	}
	for _, tc := range addCases {
		got, err := addInt(tc.a, tc.b)
		if tc.valid && err != nil {
			t.Fatalf("addInt(%d,%d) unexpected error: %v", tc.a, tc.b, err)
		}
		if !tc.valid && err == nil {
			t.Fatalf("addInt(%d,%d) = %d, want overflow error", tc.a, tc.b, got)
		}
	}

	mulCases := []struct {
		a, b  int
		valid bool
	}{
		{0, math.MaxInt, true},
		{3, 4, true},
		{math.MaxInt, 2, false},
		{math.MinInt, 2, false},
		{math.MinInt, -1, false},
		{-1, math.MinInt, false},
		{math.MinInt / 2, 2, true},
	}
	for _, tc := range mulCases {
		got, err := mulInt(tc.a, tc.b)
		if tc.valid && err != nil {
			t.Fatalf("mulInt(%d,%d) unexpected error: %v", tc.a, tc.b, err)
		}
		if !tc.valid && err == nil {
			t.Fatalf("mulInt(%d,%d) = %d, want overflow error", tc.a, tc.b, got)
		}
	}
}
