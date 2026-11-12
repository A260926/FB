package readability

import (
	"fmt"
	"math"
	"math/rand"
	nurl "net/url"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/go-shiori/dom"
	"github.com/go-shiori/go-readability/internal/re2go"
	"golang.org/x/net/html"
)

// These tests cover the text statistics index used by grabArticle's
// candidate scoring stage. They do two things:
//
//  1. compare the index against an independent slow reference that
//     materializes the normalized text of every node;
//  2. run explicit page samples through the two public entries (Parse
//     and ParseDocument) and check the final DOM, TextContent, Length
//     and Excerpt, so the same statistics are also exercised on the
//     real candidate path.
//
// The samples are built to contain: whitespace runs crossing text node
// boundaries, Chinese and supplementary-plane characters, the comma
// variants of CountCommas, in-page and regular links, empty nodes, a
// forced four-round retry, and parser reuse.

// ---------------------------------------------------------------------
// Sample pages
// ---------------------------------------------------------------------

const tsiSentence1 = `第一段提供足够的逗号，中文逗号，英文, commas, 阿拉伯逗، 小型逗﹐ 竖排逗︐ 让评分路径认真工作，补充平面字符 𠀀𠀁 与表情 😀 也计入长度。`
const tsiSentence2 = `继续补充正文，确保段落足够长。`
const tsiSentence3 = `第三段继续补充正文长度，并且再包含, several, English, commas, 以及全角逗号，确保统计一致。`

// tsiMidText is the raw (not normalized) text of the second paragraph:
// the whitespace runs cross text node and element boundaries, including
// whitespace inside and outside the links.
const tsiMidText = `第二段开头  跨越   文本节点  的空白序列， 普通外部链接  与  页内链接  混排，用来检查链接内外的空白。`

var tsiLongPage = `<html lang="zh-CN"><head><title>长报告样例</title>` +
	`<meta name="description" content="长报告样例的元描述，用于检查 Excerpt 字段。"></head><body>` +
	`<article id="report"><h1>长报告样例</h1>` +
	`<section class="intro"><p>` + strings.Repeat(tsiSentence1, 3) + `</p></section>` +
	`<section class="body"><p>第二段开头 <span> 跨越 </span> <span> 文本节点 </span> 的空白序列，` +
	`<a href="https://example.com/report"> 普通外部链接 </a> 与 <a href="#note"> 页内链接 </a> 混排，用来检查链接内外的空白。` +
	strings.Repeat(tsiSentence2, 6) + `</p>` +
	`<p>` + strings.Repeat(tsiSentence3, 3) + `</p></section>` +
	`<div class="empty-div"></div><p></p><section class="empty-section"></section>` +
	`</article></body></html>`

var tsiLongTextContent = `长报告样例` +
	strings.Repeat(tsiSentence1, 3) +
	tsiMidText +
	strings.Repeat(tsiSentence2, 6) +
	strings.Repeat(tsiSentence3, 3)

var tsiLongContent = `<div id="readability-page-1" class="page"><article id="report"><h2>长报告样例</h2>` +
	`<section><p>` + strings.Repeat(tsiSentence1, 3) + `</p></section>` +
	`<section><p>第二段开头 <span> 跨越 </span> <span> 文本节点 </span> 的空白序列，` +
	`<a href="https://example.com/report"> 普通外部链接 </a> 与 <a href="#note"> 页内链接 </a> 混排，用来检查链接内外的空白。` +
	strings.Repeat(tsiSentence2, 6) + `</p>` +
	`<p>` + strings.Repeat(tsiSentence3, 3) + `</p></section>` +
	`</article></div>`

// tsiShortPage never reaches the 500 character threshold, so grabArticle
// is forced through all four retry rounds.
var tsiShortPage = `<html><head><title>短页</title></head><body>` +
	`<article><p>短短的一小段，不足以达到阈值。</p></article></body></html>`

var tsiShortContent = `<div id="readability-page-1" class="page"><article><p>短短的一小段，不足以达到阈值。</p></article></div>`

var tsiPageURL, _ = nurl.Parse("https://capture.test/report")

// ---------------------------------------------------------------------
// Slow reference: materializes the text of every node, the way the
// original scoring code did.
// ---------------------------------------------------------------------

func tsiSlowInnerText(node *html.Node) string {
	return re2go.NormalizeSpaces(strings.TrimSpace(dom.TextContent(node)))
}

func tsiSlowStats(node *html.Node) (length, commas int, density float64) {
	innerText := tsiSlowInnerText(node)
	length = utf8.RuneCountInString(innerText)
	commas = re2go.CountCommas(innerText)
	if length == 0 {
		return length, commas, 0
	}

	var linkLength float64
	for _, link := range dom.GetElementsByTagName(node, "a") {
		href := strings.TrimSpace(dom.GetAttribute(link, "href"))
		coefficient := 1.0
		if href != "" && rxHashURL.MatchString(href) {
			coefficient = 0.3
		}
		linkLength += float64(utf8.RuneCountInString(tsiSlowInnerText(link))) * coefficient
	}
	return length, commas, linkLength / float64(length)
}

func tsiCollectNodes(root *html.Node) []*html.Node {
	var nodes []*html.Node
	var walk func(*html.Node)
	walk = func(node *html.Node) {
		nodes = append(nodes, node)
		for child := node.FirstChild; child != nil; child = child.NextSibling {
			walk(child)
		}
	}
	walk(root)
	return nodes
}

func tsiNodeName(node *html.Node) string {
	switch node.Type {
	case html.TextNode:
		runes := []rune(node.Data)
		if len(runes) > 20 {
			runes = append(runes[:20], []rune("...")...)
		}
		return fmt.Sprintf("text(%q)", string(runes))
	case html.ElementNode:
		return "element<" + node.Data + ">"
	case html.DocumentNode:
		return "document"
	case html.CommentNode:
		return "comment"
	case html.DoctypeNode:
		return "doctype"
	}
	return "node"
}

func tsiAssertMatchesSlowReference(t *testing.T, root *html.Node) {
	t.Helper()
	idx := buildTextStatsIndex(root)
	for _, node := range tsiCollectNodes(root) {
		wantLen, wantCommas, wantDensity := tsiSlowStats(node)
		if got := idx.charCount(node); got != wantLen {
			t.Errorf("charCount(%s) = %d, want %d", tsiNodeName(node), got, wantLen)
		}
		if got := idx.commas(node); got != wantCommas {
			t.Errorf("commas(%s) = %d, want %d", tsiNodeName(node), got, wantCommas)
		}
		if got := idx.linkDensity(node); math.Abs(got-wantDensity) > 1e-12 {
			t.Errorf("linkDensity(%s) = %v, want %v", tsiNodeName(node), got, wantDensity)
		}
	}
}

// tsiEdgeTree builds a tree by hand so the tricky byte sequences
// (vertical tab, form feed, NBSP, ideographic space) are exactly where
// the test wants them, independent of HTML parser normalization.
func tsiEdgeTree() *html.Node {
	text := dom.CreateTextNode
	elem := func(tag string, children ...*html.Node) *html.Node {
		e := dom.CreateElement(tag)
		for _, child := range children {
			dom.AppendChild(e, child)
		}
		return e
	}
	link := func(href string, children ...*html.Node) *html.Node {
		a := elem("a", children...)
		if href != "" {
			dom.SetAttribute(a, "href", href)
		}
		return a
	}

	return elem("div",
		text("  开头，"),
		elem("span", text(" \t\n"), text("跨节点  空白")),
		text("\v\v垂直表符后 "),
		elem("span", text("\u00A0\u00A0")),
		elem("span", text("\u3000 ")),
		text("\f换页\f"),
		elem("b"),
		elem("span"),
		link("#frag", text(" 页内"), elem("b", text("链接 "))),
		link("https://example.com/a", text("普通  链接，")),
		link("#", text("仅井号")),
		link("", text("空链接")),
		link("  #pad  ", text("空白链接﹐")),
		elem("a", text("没有href")),
		text("逗号，﹑︐︑"),
		elem("span", text("⹁⸴⸲，")),
		text("补充平面𠀀𠀁😀"),
		elem("p", text("   ")),
		text(""),
	)
}

// tsiLayeredPage generates a synthetic tree with the given number of
// nested layers; every layer carries its own text and links.
func tsiLayeredPage(layers int) string {
	var sb strings.Builder
	sb.WriteString(`<html><head><title>deep</title></head><body><article>`)
	for i := 0; i < layers; i++ {
		fmt.Fprintf(&sb,
			`<section><p>第%d层自己的文字，with commas, 和全角逗号，𠀀。</p>`+
				`<a href="https://example.com/%d">普通链接%d</a>`+
				`<a href="#frag%d">页内链接</a>`, i, i, i, i)
	}
	for i := 0; i < layers; i++ {
		sb.WriteString(`</section>`)
	}
	sb.WriteString(`</article></body></html>`)
	return sb.String()
}

// ---------------------------------------------------------------------
// 1. Index versus slow reference
// ---------------------------------------------------------------------

func TestTextStatsIndexMatchesSlowReference(t *testing.T) {
	pages := map[string]string{
		"cross-node whitespace and links": `<html><body>` +
			`<div id="cross">甲 <span> </span> <span>  </span> 乙` +
			`<a href="#in"> 页内 <b> 链接 </b> </a> 之间` +
			`<a href="https://example.com/x"> 普通 <b> 链接 </b> </a> 结束</div>` +
			`<p>   </p><div></div><span></span>` +
			`</body></html>`,
		"non-ascii whitespace": `<html><body><div>` + "\u00A0\u00A0" + `非ASCII空白` + "\u3000\u3000" +
			`<span>` + "\u00A0 " + `<b> 混合 </b>` + " \u00A0" + `</span>` +
			`换行` + "\n\n" + `和制表` + "\t\t" + `还有回车` + "\r\r" +
			`<a href="#x">` + "\u3000" + ` 页内 ` + "\u00A0" + `</a>` +
			`</div></body></html>`,
		"multi-script commas": `<html><body><div>` +
			`<p>one,two,three</p><p>中文，逗号﹑小逗︐竖排︑</p>` +
			`<p>反向⹁抬高⸴转身⸲全角，</p>` +
			`<a href="https://example.com/c">链接，里的，逗号</a>` +
			`</div></body></html>`,
		"empty nodes": `<html><body><div></div><p></p><span> </span>` +
			`<a href="#x"></a><a href="https://example.com/e"></a>` +
			`<div><span><b></b></span></div></body></html>`,
		"long report sample": tsiLongPage,
		"deep nesting":       tsiLayeredPage(64),
	}

	for name, page := range pages {
		t.Run(name, func(t *testing.T) {
			doc, err := dom.Parse(strings.NewReader(page))
			if err != nil {
				t.Fatalf("failed to parse page: %v", err)
			}
			tsiAssertMatchesSlowReference(t, doc)
		})
	}

	t.Run("hand-built edge tree", func(t *testing.T) {
		tsiAssertMatchesSlowReference(t, tsiEdgeTree())
	})
}

// TestTextStatsIndexRandomTrees builds many small random trees from a
// pool of nasty text fragments (whitespace runs, non-ASCII whitespace,
// commas, supplementary-plane runes) and checks the index against the
// slow reference on every node. The generator is seeded, so the trees
// are deterministic.
func TestTextStatsIndexRandomTrees(t *testing.T) {
	rng := rand.New(rand.NewSource(1))

	fragments := []string{
		"", " ", "  ", "   ", "\t", "\n", "\n\n", "\f", "\v", "\r",
		"a", "ab", " a ", "x  y", "字", "𠀀", "😀", "链接",
		",", "،", "﹐", "︐", "︑", "⹁", "⸴", "⸲", "，",
		" ", "  ", "  ", "　", "　　", " \t ",
	}
	hrefs := []string{"", "#", "#frag", " #pad ", "https://example.com/x"}
	tags := []string{"div", "span", "p", "b", "section"}

	var build func(depth int) *html.Node
	build = func(depth int) *html.Node {
		if depth >= 4 || rng.Intn(3) == 0 {
			return dom.CreateTextNode(fragments[rng.Intn(len(fragments))])
		}
		tag := tags[rng.Intn(len(tags))]
		if rng.Intn(4) == 0 {
			tag = "a"
		}
		node := dom.CreateElement(tag)
		if tag == "a" {
			if href := hrefs[rng.Intn(len(hrefs))]; href != "" {
				dom.SetAttribute(node, "href", href)
			}
		}
		for i, n := 0, rng.Intn(5); i < n; i++ {
			dom.AppendChild(node, build(depth+1))
		}
		return node
	}

	for i := 0; i < 300; i++ {
		root := dom.CreateElement("div")
		for j, n := 0, 1+rng.Intn(4); j < n; j++ {
			dom.AppendChild(root, build(0))
		}
		tsiAssertMatchesSlowReference(t, root)
	}
}

// ---------------------------------------------------------------------
// 2. Public entries on explicit samples
// ---------------------------------------------------------------------

func tsiCheckLongArticle(t *testing.T, article Article) {
	t.Helper()
	if article.Title != "长报告样例" {
		t.Errorf("title = %q, want %q", article.Title, "长报告样例")
	}
	if article.Excerpt != "长报告样例的元描述，用于检查 Excerpt 字段。" {
		t.Errorf("excerpt = %q", article.Excerpt)
	}
	if article.TextContent != tsiLongTextContent {
		t.Errorf("textContent = %q, want %q", article.TextContent, tsiLongTextContent)
	}
	if article.Length != utf8.RuneCountInString(tsiLongTextContent) {
		t.Errorf("length = %d, want %d", article.Length, utf8.RuneCountInString(tsiLongTextContent))
	}
	if article.Content != tsiLongContent {
		t.Errorf("content = %q, want %q", article.Content, tsiLongContent)
	}
}

func TestTextStatsIndexParseEntries(t *testing.T) {
	// Entry 1: Parse.
	parser := NewParser()
	fromParse, err := parser.Parse(strings.NewReader(tsiLongPage), tsiPageURL)
	if err != nil {
		t.Fatalf("Parse failed: %v", err)
	}
	if len(parser.attempts) != 0 {
		t.Errorf("long page took %d retry rounds, want 0", len(parser.attempts))
	}
	tsiCheckLongArticle(t, fromParse)

	// Entry 2: ParseDocument, which must not modify the caller's tree.
	doc, err := dom.Parse(strings.NewReader(tsiLongPage))
	if err != nil {
		t.Fatalf("failed to parse sample: %v", err)
	}
	treeBefore := dom.OuterHTML(doc)
	fromDocument, err := parser.ParseDocument(doc, tsiPageURL)
	if err != nil {
		t.Fatalf("ParseDocument failed: %v", err)
	}
	if treeAfter := dom.OuterHTML(doc); treeAfter != treeBefore {
		t.Errorf("ParseDocument modified the caller's tree")
	}
	tsiCheckLongArticle(t, fromDocument)

	// Both entries must agree.
	if fromParse.Content != fromDocument.Content ||
		fromParse.TextContent != fromDocument.TextContent ||
		fromParse.Length != fromDocument.Length ||
		fromParse.Excerpt != fromDocument.Excerpt ||
		fromParse.Title != fromDocument.Title {
		t.Errorf("Parse and ParseDocument disagree")
	}
}

func TestTextStatsIndexForcedRetries(t *testing.T) {
	parser := NewParser()
	article, err := parser.Parse(strings.NewReader(tsiShortPage), tsiPageURL)
	if err != nil {
		t.Fatalf("Parse failed: %v", err)
	}

	// The short page never reaches the character threshold, so
	// grabArticle must have gone through all four retry rounds, each
	// with its own freshly built index.
	if len(parser.attempts) != 4 {
		t.Errorf("attempts = %d, want 4 retry rounds", len(parser.attempts))
	}

	if article.Title != "短页" {
		t.Errorf("title = %q, want %q", article.Title, "短页")
	}
	if article.Excerpt != "短短的一小段，不足以达到阈值。" {
		t.Errorf("excerpt = %q", article.Excerpt)
	}
	if article.TextContent != "短短的一小段，不足以达到阈值。" {
		t.Errorf("textContent = %q", article.TextContent)
	}
	if article.Length != 15 {
		t.Errorf("length = %d, want 15", article.Length)
	}
	if article.Content != tsiShortContent {
		t.Errorf("content = %q, want %q", article.Content, tsiShortContent)
	}
}

func TestTextStatsIndexParserReuse(t *testing.T) {
	// Reference results from fresh parsers.
	fresh := NewParser()
	wantLong, err := fresh.Parse(strings.NewReader(tsiLongPage), tsiPageURL)
	if err != nil {
		t.Fatalf("Parse failed: %v", err)
	}
	fresh = NewParser()
	wantShort, err := fresh.Parse(strings.NewReader(tsiShortPage), tsiPageURL)
	if err != nil {
		t.Fatalf("Parse failed: %v", err)
	}

	// One parser handles both pages, then the first page again. Every
	// round rebuilds its own index, so results must not drift.
	parser := NewParser()
	gotLong1, err := parser.Parse(strings.NewReader(tsiLongPage), tsiPageURL)
	if err != nil {
		t.Fatalf("Parse failed: %v", err)
	}
	gotShort, err := parser.Parse(strings.NewReader(tsiShortPage), tsiPageURL)
	if err != nil {
		t.Fatalf("Parse failed: %v", err)
	}
	if len(parser.attempts) != 4 {
		t.Errorf("attempts = %d, want 4 retry rounds", len(parser.attempts))
	}
	gotLong2, err := parser.Parse(strings.NewReader(tsiLongPage), tsiPageURL)
	if err != nil {
		t.Fatalf("Parse failed: %v", err)
	}
	if len(parser.attempts) != 0 {
		t.Errorf("long page took %d retry rounds after reuse, want 0", len(parser.attempts))
	}

	for i, got := range []Article{gotLong1, gotLong2} {
		if got.Content != wantLong.Content || got.TextContent != wantLong.TextContent ||
			got.Length != wantLong.Length || got.Excerpt != wantLong.Excerpt ||
			got.Title != wantLong.Title {
			t.Errorf("reused parser long page #%d disagrees with fresh parser", i+1)
		}
	}
	if gotShort.Content != wantShort.Content || gotShort.TextContent != wantShort.TextContent ||
		gotShort.Length != wantShort.Length || gotShort.Excerpt != wantShort.Excerpt ||
		gotShort.Title != wantShort.Title {
		t.Errorf("reused parser short page disagrees with fresh parser")
	}
}

// ---------------------------------------------------------------------
// 3. Linear work guarantee
// ---------------------------------------------------------------------

func TestTextStatsIndexLinearWork(t *testing.T) {
	for _, layers := range []int{256, 512, 1024} {
		t.Run(fmt.Sprintf("layers-%d", layers), func(t *testing.T) {
			doc, err := dom.Parse(strings.NewReader(tsiLayeredPage(layers)))
			if err != nil {
				t.Fatalf("failed to parse page: %v", err)
			}

			// Independently measure N (node count) and B (text bytes).
			nodes := tsiCollectNodes(doc)
			n := len(nodes)
			b := 0
			for _, node := range nodes {
				if node.Type == html.TextNode {
					b += len(node.Data)
				}
			}

			idx := buildTextStatsIndex(doc)
			if idx.workNodes != n {
				t.Errorf("build visited %d nodes, want exactly N=%d", idx.workNodes, n)
			}
			if idx.workBytes != b {
				t.Errorf("build decoded %d text bytes, want exactly B=%d", idx.workBytes, b)
			}

			// Query every node; this must not visit nodes or decode
			// text again, so the counters have to stay exactly where
			// the build left them.
			for _, node := range nodes {
				_ = idx.charCount(node)
				_ = idx.commas(node)
				_ = idx.linkDensity(node)
			}
			if idx.workNodes != n || idx.workBytes != b {
				t.Errorf("queries did tree work: nodes %d -> %d, bytes %d -> %d",
					n, idx.workNodes, b, idx.workBytes)
			}

			work := idx.workNodes + idx.workBytes
			if limit := 20 * (n + b); work > limit {
				t.Errorf("work %d exceeds 20*(N+B)=%d (N=%d, B=%d)", work, limit, n, b)
			}
		})
	}
}
