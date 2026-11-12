package readability

import (
	"strings"
	"unicode"

	"github.com/go-shiori/dom"
	"golang.org/x/net/html"
)

// This file implements the text statistics index used by grabArticle.
//
// During candidate scoring the parser needs, for many nested nodes, the
// length of the normalized inner text (TrimSpace + NormalizeSpaces over
// the raw textContent), the number of commas in it, and the weighted
// link text length used by the link density. Computing those per node
// with getInnerText + GetElementsByTagName makes every ancestor re-scan
// the same text over and over, which is quadratic on deep pages.
//
// The index below is built once per grabArticle round (i.e. per clone
// tree) with a single post-order pass: every node is visited once and
// every text node is decoded once, so building costs O(node count +
// text bytes) time and O(node count) memory. Only fixed-size summaries
// are stored — never per-node subtree strings — and afterwards each
// query is answered in O(1) without walking the subtree again.
//
// The index is only valid for the exact tree it was built from. It is
// used between the end of the pre-processing loop (elementsToScore is
// complete) and the candidate sort; later stages that mutate the tree
// keep using the regular getInnerText/getLinkDensity path.

// wsSeg summarizes the ASCII whitespace runs of one piece of text. It is
// a monoid under concatenation, which is what allows merging child
// summaries in O(1) without materializing the parent's text.
//
// The "reduction" is the number of runes that re2go.NormalizeSpaces
// would remove from this piece: for every maximal run of k >= 2 ASCII
// whitespace runes it contributes k-1. NormalizeSpaces implements the
// pattern \s{2,} where \s is the ASCII class [ \t\n\f\r]; single
// whitespace runes and non-ASCII whitespace are left untouched.
type wsSeg struct {
	runes      int // total rune count of the piece
	leadASCII  int // length of the leading ASCII whitespace run
	trailASCII int // length of the trailing ASCII whitespace run
	reduction  int // runes removed by NormalizeSpaces inside the piece
}

// wsRuneSeg returns the summary of a single rune.
func wsRuneSeg(r rune) wsSeg {
	if isNormalizableSpace(r) {
		return wsSeg{runes: 1, leadASCII: 1, trailASCII: 1}
	}
	return wsSeg{runes: 1}
}

// concat returns the summary of a followed by b.
func (a wsSeg) concat(b wsSeg) wsSeg {
	out := wsSeg{
		runes:      a.runes + b.runes,
		leadASCII:  a.leadASCII,
		trailASCII: b.trailASCII,
		reduction:  a.reduction + b.reduction + mergeReduction(a.trailASCII, b.leadASCII),
	}
	if a.leadASCII == a.runes {
		// a is a single ASCII whitespace run, so it extends b's leading run.
		out.leadASCII = a.runes + b.leadASCII
	}
	if b.trailASCII == b.runes {
		// b is a single ASCII whitespace run, so it extends a's trailing run.
		out.trailASCII = b.runes + a.trailASCII
	}
	return out
}

// wsRunReduction returns how many runes NormalizeSpaces removes from a
// whitespace run of the given length.
func wsRunReduction(length int) int {
	if length >= 2 {
		return length - 1
	}
	return 0
}

// mergeReduction adjusts the reduction when a run of length a and a run
// of length b become one run of length a+b.
func mergeReduction(a, b int) int {
	return wsRunReduction(a+b) - wsRunReduction(a) - wsRunReduction(b)
}

// isNormalizableSpace reports whether r belongs to the whitespace class
// collapsed by re2go.NormalizeSpaces: [ \t\n\f\r]. Note that this is
// narrower than unicode.IsSpace (e.g. U+00A0 or U+3000 are trimmed by
// TrimSpace but never collapsed by NormalizeSpaces).
func isNormalizableSpace(r rune) bool {
	switch r {
	case ' ', '\t', '\n', '\f', '\r':
		return true
	}
	return false
}

// isCommaRune reports whether r is counted by re2go.CountCommas. The set
// covers the comma variants used in Latin, Sindhi, Chinese and other
// scripts: U+002C, U+060C, U+FE50, U+FE10, U+FE11, U+2E41, U+2E34,
// U+2E32 and U+FF0C.
func isCommaRune(r rune) bool {
	switch r {
	case ',', // U+002C
		'،', // U+060C ARABIC COMMA
		'﹐', // U+FE50 SMALL COMMA
		'︐', // U+FE10 PRESENTATION FORM FOR VERTICAL COMMA
		'︑', // U+FE11 PRESENTATION FORM FOR VERTICAL IDEOGRAPHIC COMMA
		'⹁', // U+2E41 REVERSED COMMA
		'⸴', // U+2E34 RAISED COMMA
		'⸲', // U+2E32 TURNED COMMA
		'，': // U+FF0C FULLWIDTH COMMA
		return true
	}
	return false
}

// nodeTextStats summarizes the raw text content of a node's subtree
// (the concatenation of its descendant text nodes, exactly what
// dom.TextContent returns) plus the weighted link text below it.
//
// The summary is composable: the summary of an element is the
// concatenation of the summaries of its children, so the whole index is
// computed in one post-order pass.
type nodeTextStats struct {
	runes      int   // runes in the raw subtree text
	commas     int   // commas in the raw text (normalization never touches them)
	allUniWS   bool  // raw text is empty or only Unicode whitespace
	leadUniWS  int   // runes in the leading Unicode whitespace run
	trailUniWS int   // runes in the trailing Unicode whitespace run
	leadWS     wsSeg // ASCII whitespace structure of the leading Unicode ws run
	trailWS    wsSeg // ASCII whitespace structure of the trailing Unicode ws run
	text       wsSeg // ASCII whitespace structure of the whole raw text
	linkTenths int   // weighted text length of descendant links, in tenths of a rune
}

// summarizeText builds the summary of one text node's data.
func summarizeText(s string) nodeTextStats {
	stats := nodeTextStats{allUniWS: true}
	leading := true
	var trailing wsSeg // Unicode whitespace run since the last non-ws rune

	for _, r := range s {
		stats.runes++
		if isCommaRune(r) {
			stats.commas++
		}

		runeSeg := wsRuneSeg(r)
		stats.text = stats.text.concat(runeSeg)

		if unicode.IsSpace(r) {
			trailing = trailing.concat(runeSeg)
			if leading {
				stats.leadWS = stats.leadWS.concat(runeSeg)
				stats.leadUniWS++
			}
		} else {
			stats.allUniWS = false
			leading = false
			trailing = wsSeg{}
		}
	}

	if stats.allUniWS {
		// The whole text is one Unicode whitespace run.
		stats.trailUniWS = stats.leadUniWS
		stats.trailWS = stats.leadWS
	} else {
		stats.trailUniWS = trailing.runes
		stats.trailWS = trailing
	}
	return stats
}

// concat returns the summary of a's raw text followed by b's raw text.
func (a nodeTextStats) concat(b nodeTextStats) nodeTextStats {
	out := nodeTextStats{
		runes:      a.runes + b.runes,
		commas:     a.commas + b.commas,
		text:       a.text.concat(b.text),
		allUniWS:   a.allUniWS && b.allUniWS,
		linkTenths: a.linkTenths + b.linkTenths,
	}

	// Leading Unicode whitespace run: it only reaches into b when a is
	// entirely whitespace.
	if a.allUniWS {
		out.leadUniWS = a.runes + b.leadUniWS
		out.leadWS = a.text.concat(b.leadWS)
	} else {
		out.leadUniWS = a.leadUniWS
		out.leadWS = a.leadWS
	}

	// Trailing Unicode whitespace run: symmetric to the leading one.
	if b.allUniWS {
		out.trailUniWS = b.runes + a.trailUniWS
		out.trailWS = a.trailWS.concat(b.text)
	} else {
		out.trailUniWS = b.trailUniWS
		out.trailWS = b.trailWS
	}

	return out
}

// normalizedLength returns the rune count of the node's inner text as
// computed by getInnerText(node, true): TrimSpace (Unicode whitespace)
// first, then NormalizeSpaces (collapse of ASCII whitespace runs).
//
// Because TrimSpace removes the maximal leading/trailing Unicode
// whitespace runs, every ASCII whitespace run that survives is strictly
// interior, so the surviving reduction is the total reduction minus the
// reductions of those two trimmed regions.
func (st nodeTextStats) normalizedLength() int {
	if st.allUniWS {
		return 0
	}
	trimmed := st.runes - st.leadUniWS - st.trailUniWS
	reduction := st.text.reduction - st.leadWS.reduction - st.trailWS.reduction
	return trimmed - reduction
}

// textStatsIndex holds the per-node summaries of one grabArticle round.
// The work counters exist so in-package tests can verify the build is
// linear and that queries do no tree work at all.
type textStatsIndex struct {
	stats     map[*html.Node]nodeTextStats
	workNodes int // nodes visited while building the index
	workBytes int // text node bytes decoded while building the index
}

// buildTextStatsIndex computes the text statistics of every node below
// root (inclusive) in a single post-order pass.
func buildTextStatsIndex(root *html.Node) *textStatsIndex {
	idx := &textStatsIndex{stats: make(map[*html.Node]nodeTextStats)}

	var walk func(*html.Node) nodeTextStats
	walk = func(node *html.Node) nodeTextStats {
		idx.workNodes++

		var stats nodeTextStats
		if node.Type == html.TextNode {
			idx.workBytes += len(node.Data)
			stats = summarizeText(node.Data)
		} else {
			stats = nodeTextStats{allUniWS: true}
			for child := node.FirstChild; child != nil; child = child.NextSibling {
				stats = stats.concat(walk(child))
			}
		}

		// The stored numerator covers descendant links only, exactly
		// like GetElementsByTagName(node, "a") in getLinkDensity: an
		// anchor's own text does not count towards its own density.
		idx.stats[node] = stats

		// ... but it does count towards its ancestors. The weighted
		// link text is accumulated in tenths of a rune so the merge
		// order cannot introduce floating point error: 10 per rune
		// for regular links, 3 for in-page ("#...") links.
		if node.Type == html.ElementNode && node.Data == "a" {
			weight := 10
			href := strings.TrimSpace(dom.GetAttribute(node, "href"))
			if href != "" && rxHashURL.MatchString(href) {
				weight = 3
			}
			stats.linkTenths += stats.normalizedLength() * weight
		}

		return stats
	}

	walk(root)
	return idx
}

// charCount returns the rune count of the node's normalized inner text,
// equivalent to charCount(ps.getInnerText(node, true)).
func (idx *textStatsIndex) charCount(node *html.Node) int {
	return idx.stats[node].normalizedLength()
}

// commas returns the number of commas in the node's normalized inner
// text, equivalent to re2go.CountCommas(ps.getInnerText(node, true)).
func (idx *textStatsIndex) commas(node *html.Node) int {
	return idx.stats[node].commas
}

// linkDensity returns the weighted link text length divided by the
// normalized text length, equivalent to ps.getLinkDensity(node). The
// numerator is converted back from tenths of a rune here, once, at
// query time.
func (idx *textStatsIndex) linkDensity(node *html.Node) float64 {
	stats := idx.stats[node]
	textLength := stats.normalizedLength()
	if textLength == 0 {
		return 0
	}
	return (float64(stats.linkTenths) / 10) / float64(textLength)
}
