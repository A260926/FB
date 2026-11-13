package readability

import (
	"net/url"
	"strings"
	"testing"

	"github.com/go-shiori/dom"
	"golang.org/x/net/html"
)

var dedupTestURL, _ = url.ParseRequestURI("https://capture.invalid/reports/current")

// dedupPara builds a paragraph long enough to travel the whole
// readability pipeline. The id identifies a copy without participating
// in the content fingerprint.
func dedupPara(id, body string) string {
	return `<p id="` + id + `">` + body + " " +
		strings.Repeat("Independent field measurements and ordinary observations keep this paragraph informative. ", 3) +
		`</p>`
}

// dedupParaText is the comparable text of a paragraph built by dedupPara.
func dedupParaText(body string) string {
	return strings.Join(strings.Fields(body+" "+
		strings.Repeat("Independent field measurements and ordinary observations keep this paragraph informative. ", 3)), " ")
}

// runGrabArticle drives the real grabArticle loop (including candidate
// weight accumulation) on a dedicated parser, without post-processing,
// so the data-readability-score attributes are still observable.
func runGrabArticle(t *testing.T, source string, dedup bool) (*Parser, *html.Node) {
	t.Helper()

	doc, err := dom.Parse(strings.NewReader(source))
	if err != nil {
		t.Fatalf("failed to parse source: %v", err)
	}

	ps := NewParser()
	ps.DeduplicateContent = dedup
	ps.doc = dom.Clone(doc, true)
	ps.documentURI = dedupTestURL
	ps.attempts = []parseAttempt{}
	ps.flags = flags{
		stripUnlikelys:     true,
		useWeightClasses:   true,
		cleanConditionally: true,
	}
	ps.unwrapNoscriptImages(ps.doc)
	ps.removeScripts(ps.doc)
	ps.prepDocument()

	return &ps, ps.grabArticle()
}

func TestContentDedupRuns(t *testing.T) {
	bodyA := "First paragraph about alpha sampling conditions."
	bodyB := "Second paragraph about beta collection constraints."
	bodyC := "Third paragraph about gamma calibration results."

	// The same body twice, once in a div shell and once in a section
	// shell nested behind article/main transparent wrappers.
	shellPage := `<html><head><title>Field report</title></head><body>
<main><article><div id="first-copy">` +
		dedupPara("a1", bodyA) + dedupPara("b1", bodyB) + dedupPara("c1", bodyC) +
		`</div></article></main>
<section id="second-copy">` +
		dedupPara("a2", bodyA) + dedupPara("b2", bodyB) + dedupPara("c2", bodyC) +
		`</section>
</body></html>`

	t.Run("different shells collapse through both entries", func(t *testing.T) {
		// Public Parse entry.
		psOn := NewParser()
		psOn.DeduplicateContent = true
		articleOn, err := psOn.Parse(strings.NewReader(shellPage), dedupTestURL)
		if err != nil {
			t.Fatalf("parse with dedup enabled failed: %v", err)
		}

		if got := strings.Count(articleOn.TextContent, dedupParaText(bodyA)); got != 1 {
			t.Errorf("body A occurrences = %d, want 1", got)
		}
		if got := strings.Count(articleOn.Content, `id="a2"`); got != 0 {
			t.Errorf("deleted unit a2 still present in content, occurrences = %d", got)
		}
		if !strings.Contains(articleOn.Content, `id="a1"`) {
			t.Errorf("kept unit a1 missing from content")
		}
		if articleOn.Length != charCount(articleOn.TextContent) {
			t.Errorf("Length %d does not match TextContent runes %d",
				articleOn.Length, charCount(articleOn.TextContent))
		}
		foldedText := strings.Join(strings.Fields(articleOn.TextContent), " ")
		if articleOn.Excerpt != "" && !strings.Contains(foldedText, strings.Join(strings.Fields(articleOn.Excerpt), " ")) {
			t.Errorf("excerpt %q is not consistent with text content", articleOn.Excerpt)
		}

		// ParseDocument entry: same result, caller tree untouched.
		callerDoc, err := dom.Parse(strings.NewReader(shellPage))
		if err != nil {
			t.Fatalf("failed to parse caller document: %v", err)
		}
		before := dom.OuterHTML(callerDoc)

		psDoc := NewParser()
		psDoc.DeduplicateContent = true
		articleDoc, err := psDoc.ParseDocument(callerDoc, dedupTestURL)
		if err != nil {
			t.Fatalf("ParseDocument with dedup enabled failed: %v", err)
		}
		if articleDoc.TextContent != articleOn.TextContent {
			t.Errorf("ParseDocument text %q != Parse text %q",
				articleDoc.TextContent, articleOn.TextContent)
		}
		if after := dom.OuterHTML(callerDoc); after != before {
			t.Errorf("ParseDocument mutated the caller document")
		}
		if articleDoc.Node == nil || dom.TagName(articleDoc.Node) == "" {
			t.Errorf("article Node is not populated")
		}

		// Switching the toggle off must restore the original result.
		psOff := NewParser()
		articleOff, err := psOff.Parse(strings.NewReader(shellPage), dedupTestURL)
		if err != nil {
			t.Fatalf("parse with dedup disabled failed: %v", err)
		}
		if got := strings.Count(articleOff.TextContent, dedupParaText(bodyA)); got != 2 {
			t.Errorf("body A occurrences with toggle off = %d, want 2", got)
		}
		if !strings.Contains(articleOff.Content, `id="a2"`) {
			t.Errorf("second copy must be kept when toggle is off")
		}
	})

	t.Run("removed copies stop contributing candidate weight", func(t *testing.T) {
		psOn, articleOn := runGrabArticle(t, shellPage, true)
		psOff, articleOff := runGrabArticle(t, shellPage, false)

		firstOn := dom.GetElementByID(articleOn, "first-copy")
		if firstOn == nil {
			t.Fatalf("kept wrapper first-copy missing from grabbed article")
		}
		if !psOn.hasContentScore(firstOn) || psOn.getContentScore(firstOn) <= 0 {
			t.Errorf("first-copy should accumulate positive weight, got %v",
				psOn.getContentScore(firstOn))
		}
		if second := dom.GetElementByID(articleOn, "second-copy"); second != nil {
			t.Errorf("duplicate wrapper second-copy must not enter the article, got %s",
				dom.OuterHTML(second))
		}
		if node := dom.GetElementByID(articleOn, "a2"); node != nil {
			t.Errorf("duplicate paragraph a2 still reached the scored tree")
		}

		// Same pipeline without dedup: the duplicate wrapper does
		// accumulate weight and enters the article.
		secondOff := dom.GetElementByID(articleOff, "second-copy")
		if secondOff == nil {
			t.Fatalf("without dedup, second-copy should be grabbed")
		}
		if !psOff.hasContentScore(secondOff) || psOff.getContentScore(secondOff) <= 0 {
			t.Errorf("second-copy should accumulate weight without dedup, got %v",
				psOff.getContentScore(secondOff))
		}
	})

	t.Run("longest non-overlapping run wins", func(t *testing.T) {
		para := func(id, marker string) string {
			return dedupPara(id, marker+" paragraph with its own wording.")
		}
		// Units: X A B Y A B C D A B C D.
		// The earlier A B only matches for two units, so it is not
		// touched. The final A B C D run can pair with that length-2
		// front and the length-4 run in the middle; the longest wins.
		longestPage := `<body><div>` +
			para("x1", "X") + para("a1", "A") + para("b1", "B") + para("y1", "Y") +
			para("a2", "A") + para("b2", "B") + para("c2", "C") + para("d2", "D") +
			para("a3", "A") + para("b3", "B") + para("c3", "C") + para("d3", "D") +
			`</div></body>`

		ps := NewParser()
		ps.DeduplicateContent = true
		article, err := ps.Parse(strings.NewReader(longestPage), dedupTestURL)
		if err != nil {
			t.Fatalf("parse failed: %v", err)
		}
		for _, kept := range []string{"x1", "a1", "b1", "y1", "a2", "b2", "c2", "d2"} {
			if !strings.Contains(article.Content, `id="`+kept+`"`) {
				t.Errorf("expected unit %s to be kept", kept)
			}
		}
		for _, removed := range []string{"a3", "b3", "c3", "d3"} {
			if strings.Contains(article.Content, `id="`+removed+`"`) {
				t.Errorf("unit %s should have been removed as the longer duplicate", removed)
			}
		}
	})

	t.Run("three identical runs keep only the earliest", func(t *testing.T) {
		triplePage := `<body><div>` +
			dedupPara("t1a", bodyA) + dedupPara("t1b", bodyB) + dedupPara("t1c", bodyC) +
			dedupPara("t2a", bodyA) + dedupPara("t2b", bodyB) + dedupPara("t2c", bodyC) +
			dedupPara("t3a", bodyA) + dedupPara("t3b", bodyB) + dedupPara("t3c", bodyC) +
			`</div></body>`
		ps := NewParser()
		ps.DeduplicateContent = true
		triple, err := ps.Parse(strings.NewReader(triplePage), dedupTestURL)
		if err != nil {
			t.Fatalf("parse failed: %v", err)
		}
		if got := strings.Count(triple.Content, `id="t1a"`); got != 1 {
			t.Errorf("earliest run missing, t1a occurrences = %d", got)
		}
		for _, removed := range []string{"t2a", "t3a"} {
			if strings.Contains(triple.Content, `id="`+removed+`"`) {
				t.Errorf("unit %s should have been removed", removed)
			}
		}
	})

	t.Run("separators stop runs", func(t *testing.T) {
		// A list between the copies is a block separator, so the
		// copies are not consecutive and both stay.
		separatedPage := `<body><article><div>` +
			dedupPara("s1a", bodyA) + dedupPara("s1b", bodyB) + dedupPara("s1c", bodyC) +
			`<ul><li>Unrelated list item acting as a separator.</li></ul>` +
			dedupPara("s2a", bodyA) + dedupPara("s2b", bodyB) + dedupPara("s2c", bodyC) +
			`</div></article></body>`

		ps := NewParser()
		ps.DeduplicateContent = true
		article, err := ps.Parse(strings.NewReader(separatedPage), dedupTestURL)
		if err != nil {
			t.Fatalf("parse failed: %v", err)
		}
		if got := strings.Count(article.Content, `id="s2a"`); got != 1 {
			t.Errorf("copy behind a list separator should be kept, occurrences = %d", got)
		}
		if got := strings.Count(article.Content, `id="s1a"`); got != 1 {
			t.Errorf("first copy missing, occurrences = %d", got)
		}

		// A standalone image is a separator as well.
		imagePage := `<body><div>` +
			dedupPara("i1a", bodyA) + dedupPara("i1b", bodyB) + dedupPara("i1c", bodyC) +
			`<img src="/media/break.png" alt="break">` +
			dedupPara("i2a", bodyA) + dedupPara("i2b", bodyB) + dedupPara("i2c", bodyC) +
			`</div></body>`
		ps2 := NewParser()
		ps2.DeduplicateContent = true
		imageArticle, err := ps2.Parse(strings.NewReader(imagePage), dedupTestURL)
		if err != nil {
			t.Fatalf("parse failed: %v", err)
		}
		if !strings.Contains(imageArticle.Content, `id="i2a"`) {
			t.Errorf("copy behind an image separator should be kept")
		}
	})

	t.Run("different resources keep both copies", func(t *testing.T) {
		padding := strings.Repeat("Extra padding sentence to keep the paragraph substantial. ", 3)
		linkBody := "Same paragraph text surrounding a resource link for both devices."
		linkPage := `<body><div>` +
			`<p id="l1a">` + linkBody + ` <a href="https://cdn.invalid/first">ref</a> ` + padding + `</p>` +
			dedupPara("l1b", bodyB) + dedupPara("l1c", bodyC) +
			`<p id="l2a">` + linkBody + ` <a href="https://cdn.invalid/second">ref</a> ` + padding + `</p>` +
			dedupPara("l2b", bodyB) + dedupPara("l2c", bodyC) +
			`</div></body>`

		ps := NewParser()
		ps.DeduplicateContent = true
		article, err := ps.Parse(strings.NewReader(linkPage), dedupTestURL)
		if err != nil {
			t.Fatalf("parse failed: %v", err)
		}
		if !strings.Contains(article.Content, `id="l1a"`) ||
			!strings.Contains(article.Content, `id="l2a"`) {
			t.Errorf("paragraphs with different link targets must both be kept")
		}

		imgBody := "Same paragraph text surrounding an image for both devices."
		imgPage := `<body><div>` +
			`<p id="m1a">` + imgBody + ` <img src="/media/same.png" alt="first"> ` + padding + `</p>` +
			dedupPara("m1b", bodyB) + dedupPara("m1c", bodyC) +
			`<p id="m2a">` + imgBody + ` <img src="/media/same.png" alt="second"> ` + padding + `</p>` +
			dedupPara("m2b", bodyB) + dedupPara("m2c", bodyC) +
			`</div></body>`
		ps2 := NewParser()
		ps2.DeduplicateContent = true
		imgArticle, err := ps2.Parse(strings.NewReader(imgPage), dedupTestURL)
		if err != nil {
			t.Fatalf("parse failed: %v", err)
		}
		if !strings.Contains(imgArticle.Content, `id="m1a"`) ||
			!strings.Contains(imgArticle.Content, `id="m2a"`) {
			t.Errorf("paragraphs with different image alt text must both be kept")
		}
	})

	t.Run("short repeats, span wrappers and pre text behave per spec", func(t *testing.T) {
		// Only two repeated units: below the three-unit threshold.
		shortPage := `<body><div>` +
			dedupPara("q1a", bodyA) + dedupPara("q1b", bodyB) +
			dedupPara("q2a", bodyA) + dedupPara("q2b", bodyB) +
			`</div></body>`
		ps := NewParser()
		ps.DeduplicateContent = true
		article, err := ps.Parse(strings.NewReader(shortPage), dedupTestURL)
		if err != nil {
			t.Fatalf("parse failed: %v", err)
		}
		for _, id := range []string{"q1a", "q1b", "q2a", "q2b"} {
			if !strings.Contains(article.Content, `id="`+id+`"`) {
				t.Errorf("short repeat unit %s must be kept", id)
			}
		}

		// Span wrappers and whitespace folding do not affect p identity;
		// pre blocks keep their raw text, so their copies match too.
		preText := "line one\nline  two\n\tindented block"
		wrappedA := `<span>` + bodyA + ` </span>`
		foldedB := "  " + strings.Join(strings.Fields(bodyB), "   ") + "  "
		prePage := `<body><article><div>` +
			`<pre id="pr1">` + preText + `</pre>` +
			dedupPara("pp1", bodyA) + dedupPara("pp2", bodyB) +
			`</div><section>` +
			`<pre id="pr2">` + preText + `</pre>` +
			`<p id="pp3">` + wrappedA + " " +
			strings.Repeat("Independent field measurements and ordinary observations keep this paragraph informative. ", 3) + `</p>` +
			`<p id="pp4">` + foldedB + " " +
			strings.Repeat("Independent field measurements and ordinary observations keep this paragraph informative. ", 3) + `</p>` +
			`</section></article></body>`
		ps2 := NewParser()
		ps2.DeduplicateContent = true
		preArticle, err := ps2.Parse(strings.NewReader(prePage), dedupTestURL)
		if err != nil {
			t.Fatalf("parse failed: %v", err)
		}
		if !strings.Contains(preArticle.Content, `id="pr1"`) {
			t.Errorf("earliest pre block missing")
		}
		if strings.Contains(preArticle.Content, `id="pr2"`) {
			t.Errorf("duplicate pre block should be removed")
		}
		if strings.Contains(preArticle.Content, `id="pp3"`) ||
			strings.Contains(preArticle.Content, `id="pp4"`) {
			t.Errorf("span-wrapped and whitespace-folded duplicates should be removed")
		}
		if !strings.Contains(preArticle.Content, `id="pp1"`) ||
			!strings.Contains(preArticle.Content, `id="pp2"`) {
			t.Errorf("kept paragraph copies are missing")
		}
	})

	t.Run("fingerprint selection picks longest then earliest", func(t *testing.T) {
		// Direct coverage of the wired selection helper: equal-length
		// fronts resolve to the earliest start, longer runs win.
		doc, err := dom.Parse(strings.NewReader(`<body><div>` +
			dedupPara("u0", bodyA) + dedupPara("u1", bodyB) + dedupPara("u2", bodyC) +
			dedupPara("u3", bodyA) + dedupPara("u4", bodyB) + dedupPara("u5", bodyC) +
			`</div></body>`))
		if err != nil {
			t.Fatalf("parse failed: %v", err)
		}
		ps := NewParser()
		ps.documentURI = dedupTestURL
		units := ps.collectDedupUnits(doc)
		fps := make([]contentFingerprint, len(units))
		index := make(map[[32]byte][]int)
		for i, unit := range units {
			fps[i] = ps.buildContentFingerprint(unit)
			index[fps[i].hash] = append(index[fps[i].hash], i)
		}
		separators := make([]bool, len(units))

		start, length := findDuplicateDedupRun(units, fps, index, separators, 3)
		if start != 0 || length != 3 {
			t.Errorf("equal-length match = (start %d, length %d), want (0, 3)", start, length)
		}
		if _, none := findDuplicateDedupRun(units, fps, index, separators, 2); none != 0 {
			t.Errorf("two-unit overlap must not qualify, got length %d", none)
		}
	})
}

func TestContentDedupAnchors(t *testing.T) {
	bodyA := "First paragraph about alpha sampling conditions."
	bodyB := "Second paragraph about beta collection constraints."
	bodyC := "Third paragraph about gamma calibration results."
	linkPara := func(href, label string) string {
		return `<p>` + strings.Repeat("Closing paragraph with an in-page reference for readers. ", 3) +
			`<a href="` + href + `">` + label + `</a></p>`
	}

	t.Run("fragment links are repointed to the kept unique id", func(t *testing.T) {
		source := `<html><body><main><article><div>` +
			dedupPara("a1", bodyA) + dedupPara("b1", bodyB) + dedupPara("c1", bodyC) +
			`</div><section>` +
			dedupPara("a2", bodyA) + dedupPara("b2", bodyB) + dedupPara("c2", bodyC) +
			`</section>` +
			linkPara("#b2", "measurement") +
			linkPara("#b%32", "decoded measurement") +
			linkPara("https://example.invalid/other#frag", "external") +
			`</article></main></body></html>`

		ps := NewParser()
		ps.DeduplicateContent = true
		article, err := ps.Parse(strings.NewReader(source), dedupTestURL)
		if err != nil {
			t.Fatalf("parse failed: %v", err)
		}

		if strings.Contains(article.Content, `href="#b2"`) {
			t.Errorf("link to deleted id b2 was not retargeted")
		}
		if got := strings.Count(article.Content, `href="#b1"`); got != 2 {
			t.Errorf("both raw and percent-encoded links should point at b1, occurrences = %d", got)
		}
		if !strings.Contains(article.Content, `id="b1"`) {
			t.Errorf("retargeted id b1 must exist in the output")
		}
		if !strings.Contains(article.Content, `href="https://example.invalid/other#frag"`) {
			t.Errorf("non-fragment addresses must not be modified")
		}
		if got := strings.Count(article.TextContent, dedupParaText(bodyB)); got != 1 {
			t.Errorf("duplicate body B still present, occurrences = %d", got)
		}
	})

	t.Run("plus signs in fragments stay literal", func(t *testing.T) {
		// An element carries the id "k2c plus" (space). A link using
		// "#k2c+plus" must not match it, because '+' is not decoded as
		// space, so the link is left untouched rather than retargeted.
		source := `<html><body><div>` +
			strings.Replace(dedupPara("k1c", bodyC), `id="k1c"`, `id="k2c plus"`, 1) +
			dedupPara("k1a", bodyA) + dedupPara("k1b", bodyB) +
			`</div><section>` +
			dedupPara("k2a", bodyA) + dedupPara("k2b", bodyB) + dedupPara("k2c", bodyC) +
			`</section>` + linkPara("#k2c+plus", "plus ref") + `</body></html>`

		ps := NewParser()
		ps.DeduplicateContent = true
		article, err := ps.Parse(strings.NewReader(source), dedupTestURL)
		if err != nil {
			t.Fatalf("parse failed: %v", err)
		}
		if !strings.Contains(article.Content, `href="#k2c+plus"`) {
			t.Errorf("literal '+' fragment must be left unchanged")
		}
		if strings.Contains(article.Content, `href="#k2c plus"`) {
			t.Errorf("'+' was wrongly decoded as a space")
		}
	})

	t.Run("references without a clear mapping protect the whole run", func(t *testing.T) {
		build := func(extra string) string {
			return `<html><body><div>` +
				dedupPara("k1a", bodyA) + dedupPara("k1b", bodyB) + dedupPara("k1c", bodyC) +
				`</div><section>` +
				dedupPara("k2a", bodyA) + dedupPara("k2b", bodyB) + dedupPara("k2c", bodyC) +
				`</section>` + extra + `</body></html>`
		}
		assertKept := func(t *testing.T, source string) {
			t.Helper()
			ps := NewParser()
			ps.DeduplicateContent = true
			article, err := ps.Parse(strings.NewReader(source), dedupTestURL)
			if err != nil {
				t.Fatalf("parse failed: %v", err)
			}
			if !strings.Contains(article.Content, `id="k2a"`) {
				t.Errorf("protected duplicate run was partially or fully removed")
			}
			if got := strings.Count(article.TextContent, dedupParaText(bodyA)); got != 2 {
				t.Errorf("protected run must keep both copies, occurrences = %d", got)
			}
		}

		// Kept counterpart has no id, so the link cannot be repointed.
		t.Run("no front id", func(t *testing.T) {
			noFrontID := build(linkPara("#k2b", "ref"))
			noFrontID = strings.ReplaceAll(noFrontID, `id="k1b"`, "")
			assertKept(t, noFrontID)
		})

		// Target id is duplicated in the document.
		t.Run("duplicate target id", func(t *testing.T) {
			duplicateID := build(linkPara("#k2b", "ref") +
				`<span id="k2b">shadow anchor</span>`)
			assertKept(t, duplicateID)
		})

		// Link reaches a descendant id inside a to-delete unit. The
		// empty span keeps the unit fingerprint unchanged so the run
		// would otherwise be removed.
		t.Run("descendant id", func(t *testing.T) {
			k2bWithInner := `<p id="k2b">` + bodyB + " " +
				strings.Repeat("Independent field measurements and ordinary observations keep this paragraph informative. ", 3) +
				`<span id="inner-k2b"></span></p>`
			descendant := `<html><body><div>` +
				dedupPara("k1a", bodyA) + dedupPara("k1b", bodyB) + dedupPara("k1c", bodyC) +
				`</div><section>` +
				dedupPara("k2a", bodyA) + k2bWithInner + dedupPara("k2c", bodyC) +
				`</section>` + linkPara("#inner-k2b", "inner ref") + `</body></html>`
			assertKept(t, descendant)
		})

		// Percent-decoded fragment ("%2B" -> "+") hits a duplicated id,
		// so the ambiguous mapping protects the run instead of splitting
		// it into a shorter run.
		t.Run("decoded plus duplicated", func(t *testing.T) {
			plusSource := build(linkPara("#k2c%2Bplus", "plus ref"))
			plusSource = strings.ReplaceAll(plusSource, `id="k2c"`, `id="k2c+plus"`)
			plusSource = strings.ReplaceAll(plusSource,
				`</body>`, `<span id="k2c+plus">shadow anchor</span></body>`)
			assertKept(t, plusSource)
		})
	})
}

func TestContentDedupAttempts(t *testing.T) {
	bodyA := "First paragraph about alpha sampling conditions."
	bodyB := "Second paragraph about beta collection constraints."
	bodyC := "Third paragraph about gamma calibration results."

	// Short duplicated page: every grabArticle attempt stays below the
	// inflated character threshold, forcing all relaxed flag rounds.
	shortPage := `<html><body><article><div>` +
		`<p id="w1a">` + bodyA + `</p><p id="w1b">` + bodyB + `</p><p id="w1c">` + bodyC + `</p>` +
		`</div><section>` +
		`<p id="w2a">` + bodyA + `</p><p id="w2b">` + bodyB + `</p><p id="w2c">` + bodyC + `</p>` +
		`</section></article></body></html>`

	t.Run("each relaxed attempt dedups its own clone", func(t *testing.T) {
		doc, err := dom.Parse(strings.NewReader(shortPage))
		if err != nil {
			t.Fatalf("failed to parse source: %v", err)
		}

		ps := NewParser()
		ps.DeduplicateContent = true
		ps.CharThresholds = 100000
		ps.doc = dom.Clone(doc, true)
		ps.documentURI = dedupTestURL
		ps.attempts = []parseAttempt{}
		ps.flags = flags{
			stripUnlikelys:     true,
			useWeightClasses:   true,
			cleanConditionally: true,
		}
		ps.unwrapNoscriptImages(ps.doc)
		ps.removeScripts(ps.doc)
		ps.prepDocument()

		article := ps.grabArticle()
		if len(ps.attempts) != 4 {
			t.Fatalf("expected four relaxed attempts, got %d", len(ps.attempts))
		}

		for i, attempt := range ps.attempts {
			text := dom.TextContent(attempt.articleContent)
			if got := strings.Count(text, bodyA); got != 1 {
				t.Errorf("attempt %d keeps %d copies of body A, want 1", i, got)
			}
			if node := dom.GetElementByID(attempt.articleContent, "w2a"); node != nil {
				t.Errorf("attempt %d still contains deleted unit w2a", i)
			}
		}

		// The parser's working document is shared across attempts but
		// only clones are mutated, so both copies must remain there.
		if node := dom.GetElementByID(ps.doc, "w2a"); node == nil {
			t.Errorf("shared working document lost the duplicate unit")
		}
		if node := dom.GetElementByID(article, "w2a"); node != nil {
			t.Errorf("returned fallback article still contains the duplicate")
		}
		if node := dom.GetElementByID(article, "w1a"); node == nil {
			t.Errorf("returned fallback article lost the kept copy")
		}
	})

	t.Run("parser reuse carries no fingerprints or anchor maps", func(t *testing.T) {
		// The reference paragraph lives inside the article shell so it
		// travels through the real scoring/extraction path and remains
		// in the final content with its (possibly retargeted) href.
		anchorPage := `<html><body><main><article><div>` +
			dedupPara("r1a", bodyA) + dedupPara("r1b", bodyB) + dedupPara("r1c", bodyC) +
			`</div><section>` +
			dedupPara("r2a", bodyA) + dedupPara("r2b", bodyB) + dedupPara("r2c", bodyC) +
			`</section><p>` + strings.Repeat("Closing paragraph with an in-page reference for readers. ", 3) +
			`<a href="#r2b">ref</a></p></article></main></body></html>`

		ps := NewParser()
		ps.DeduplicateContent = true

		first, err := ps.Parse(strings.NewReader(anchorPage), dedupTestURL)
		if err != nil {
			t.Fatalf("first parse failed: %v", err)
		}
		if strings.Contains(first.Content, `href="#r2b"`) ||
			!strings.Contains(first.Content, `href="#r1b"`) {
			t.Fatalf("first parse did not perform the expected anchor retarget")
		}

		// Toggle off on the same instance: result must match a fresh
		// default parser exactly, including the formerly retargeted
		// anchor and both content copies.
		ps.DeduplicateContent = false
		reusedOff, err := ps.Parse(strings.NewReader(anchorPage), dedupTestURL)
		if err != nil {
			t.Fatalf("reused parse with toggle off failed: %v", err)
		}
		freshParser := NewParser()
		freshOff, err := freshParser.Parse(strings.NewReader(anchorPage), dedupTestURL)
		if err != nil {
			t.Fatalf("fresh parse failed: %v", err)
		}
		if reusedOff.Content != freshOff.Content ||
			reusedOff.TextContent != freshOff.TextContent ||
			reusedOff.Length != freshOff.Length ||
			reusedOff.Excerpt != freshOff.Excerpt {
			t.Errorf("reused parser with toggle off diverges from a fresh parser")
		}
		if !strings.Contains(reusedOff.Content, `href="#r2b"`) {
			t.Errorf("previous anchor mapping leaked into the toggle-off parse")
		}

		// Toggle back on and feed another page: the earlier mapping
		// must not influence it, and output equals a fresh enabled parser.
		ps.DeduplicateContent = true
		otherPage := `<html><body><article>` +
			dedupPara("n1a", bodyA) + dedupPara("n1b", bodyB) + dedupPara("n1c", bodyC) +
			dedupPara("n2a", bodyA) + dedupPara("n2b", bodyB) + dedupPara("n2c", bodyC) +
			`</article></body></html>`
		reusedOn, err := ps.Parse(strings.NewReader(otherPage), dedupTestURL)
		if err != nil {
			t.Fatalf("reused parse with toggle on failed: %v", err)
		}
		freshOn, err := func() (Article, error) {
			fresh := NewParser()
			fresh.DeduplicateContent = true
			return fresh.Parse(strings.NewReader(otherPage), dedupTestURL)
		}()
		if err != nil {
			t.Fatalf("fresh enabled parse failed: %v", err)
		}
		if reusedOn.Content != freshOn.Content ||
			reusedOn.TextContent != freshOn.TextContent ||
			reusedOn.Length != freshOn.Length {
			t.Errorf("reused parser with toggle on diverges from a fresh enabled parser")
		}
		if strings.Contains(reusedOn.Content, `id="n2a"`) {
			t.Errorf("reused parser failed to deduplicate the new page")
		}
	})
}
