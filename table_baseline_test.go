package goquery

import (
	"strings"
	"testing"
)

func TestTableBaselineDOM(t *testing.T) {
	input := `<table id="outer"><thead><tr><th rowspan="0">Site &amp; region</th></tr></thead><tbody><tr><td>A<table id="inner"><tr><td>nested</td></tr></table></td></tr><tr></tr></tbody></table>`
	doc, err := NewDocumentFromReader(strings.NewReader(input))
	if err != nil {
		t.Fatal(err)
	}
	table := doc.Find("#outer")
	direct := table.ChildrenFiltered("thead,tbody").ChildrenFiltered("tr")
	if direct.Length() != 3 || table.Find("tr").Length() != 4 {
		t.Fatalf("direct rows=%d all rows=%d", direct.Length(), table.Find("tr").Length())
	}
	header := direct.First().ChildrenFiltered("th")
	if header.AttrOr("rowspan", "") != "0" || header.Text() != "Site & region" {
		t.Fatalf("source header = %q %q", header.AttrOr("rowspan", ""), header.Text())
	}
	copyOfCell := direct.Eq(1).ChildrenFiltered("td").Clone()
	copyOfCell.Find("table").Remove()
	if copyOfCell.Text() != "A" || table.Find("#inner").Length() != 1 {
		t.Fatal("text extraction changed the original table")
	}
}
