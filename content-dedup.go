package readability

import (
	"crypto/sha256"
	nurl "net/url"
	"strings"

	"github.com/go-shiori/dom"
	"golang.org/x/net/html"
)

// transparentDedupShells are element tags that never break the sequence
// of deduplication units and never count as a unit themselves.
var transparentDedupShells = map[string]struct{}{
	"html":    {},
	"body":    {},
	"div":     {},
	"section": {},
	"article": {},
	"main":    {},
}

// dedupMediaTags are standalone media elements that separate sequences
// of deduplication units.
var dedupMediaTags = map[string]struct{}{
	"img":     {},
	"video":   {},
	"audio":   {},
	"svg":     {},
	"iframe":  {},
	"canvas":  {},
	"embed":   {},
	"object":  {},
	"picture": {},
}

// dedupBlockTags are block-level structures (other than the transparent
// shells and p/pre units) that separate sequences of deduplication units.
var dedupBlockTags = map[string]struct{}{
	"h1": {}, "h2": {}, "h3": {}, "h4": {}, "h5": {}, "h6": {},
	"ul": {}, "ol": {}, "li": {}, "dl": {}, "dt": {}, "dd": {},
	"table": {}, "thead": {}, "tbody": {}, "tfoot": {}, "tr": {}, "td": {}, "th": {},
	"blockquote": {}, "hr": {}, "form": {}, "fieldset": {}, "figure": {},
	"figcaption": {}, "header": {}, "footer": {}, "nav": {}, "aside": {},
	"address": {}, "details": {}, "summary": {}, "dialog": {},
	"noscript": {}, "template": {},
}

// contentResource describes a single link or image embedded in a
// deduplication unit, in document order.
type contentResource struct {
	tag string
	url string
	alt string
}

// contentFingerprint identifies the comparable content of a p/pre unit.
// Only the tag, the text and the embedded links/images matter: other
// attributes and inline wrappers such as spans are ignored.
type contentFingerprint struct {
	tag       string
	text      string
	resources []contentResource
	hash      [sha256.Size]byte
}

// anchorRetarget records a fragment link that must be repointed to the
// unit kept in place of a deleted duplicate.
type anchorRetarget struct {
	link  *html.Node
	newID string
}

// equal compares the full fingerprint, not only the hash.
func (f contentFingerprint) equal(other contentFingerprint) bool {
	if f.tag != other.tag || f.text != other.text ||
		len(f.resources) != len(other.resources) {
		return false
	}

	for i := range f.resources {
		if f.resources[i] != other.resources[i] {
			return false
		}
	}
	return true
}

// deduplicateContentUnits removes duplicated device-specific article
// bodies from the cloned document of the current grabArticle attempt.
// It runs after the node prepping loop and before paragraphs start
// accumulating candidate weight, so removed units (and their descendants)
// never contribute to the scoring.
//
// All state is local to the call: every grabArticle attempt works on its
// own clone and builds its own fingerprints and anchor mappings, and the
// same Parser can be reused across pages and option toggles.
func (ps *Parser) deduplicateContentUnits(doc *html.Node) {
	// Start nodes of runs that cannot be removed because of their
	// in-page references. They stay protected for the whole call and
	// must not be retried as shorter runs.
	protected := make(map[*html.Node]struct{})

	for {
		units := ps.collectDedupUnits(doc)
		if len(units) < 3 {
			return
		}

		fingerprints := make([]contentFingerprint, len(units))
		hashIndex := make(map[[sha256.Size]byte][]int)
		for i, unit := range units {
			fp := ps.buildContentFingerprint(unit)
			fingerprints[i] = fp
			hashIndex[fp.hash] = append(hashIndex[fp.hash], i)
		}

		// separators[i] reports a separator sitting between unit i-1
		// and unit i, so consecutive runs cannot cross it. The node
		// prepping pass wraps standalone media in a <p>, which turns
		// the media into a unit of its own: such a media-only unit is
		// itself a separator, not a paragraph that could match.
		separators := make([]bool, len(units))
		for i := 1; i < len(units); i++ {
			separators[i] = hasDedupSeparator(units[i-1], units[i])
		}
		for i, unit := range units {
			if isMediaOnlyDedupUnit(unit) {
				separators[i] = true
			}
		}

		deleted := false
		for i := 1; i < len(units); i++ {
			if _, skip := protected[units[i]]; skip {
				continue
			}

			frontStart, runLength := findDuplicateDedupRun(
				units, fingerprints, hashIndex, separators, i)
			if runLength == 0 {
				continue
			}

			retargets, safe := ps.planDedupAnchorRetargets(
				doc, units, frontStart, i, runLength)
			if !safe {
				// Keep the whole candidate run, including every unit in
				// it: a later start must not trim it into a shorter run
				// to bypass the protection.
				for k := 0; k < runLength; k++ {
					protected[units[i+k]] = struct{}{}
				}
				continue
			}

			for _, retarget := range retargets {
				dom.SetAttribute(retarget.link, "href", "#"+retarget.newID)
			}

			for k := 0; k < runLength; k++ {
				target := units[i+k]
				if target.Parent != nil {
					target.Parent.RemoveChild(target)
				}
			}

			// Restart from the remaining units, walking the later
			// starts from front to back again.
			deleted = true
			break
		}

		if !deleted {
			return
		}
	}
}

// collectDedupUnits gathers p and pre elements in document order. Once a
// unit is entered, its descendants are not collected. Only transparent
// shells are traversed; every other structure stops the walk and acts as
// a separator.
func (ps *Parser) collectDedupUnits(root *html.Node) []*html.Node {
	var units []*html.Node

	var walk func(*html.Node)
	walk = func(node *html.Node) {
		if node.Type == html.ElementNode {
			tag := dom.TagName(node)
			if tag == "p" || tag == "pre" {
				units = append(units, node)
				return
			}

			if _, transparent := transparentDedupShells[tag]; !transparent {
				return
			}
		}

		for child := node.FirstChild; child != nil; child = child.NextSibling {
			walk(child)
		}
	}

	walk(root)
	return units
}

// buildContentFingerprint builds the comparable identity of a unit. The
// paragraph text has Unicode whitespace folded with strings.Fields,
// while pre keeps its raw text. Links and images are recorded in document
// order, with URLs resolved through the parser's page address rules.
func (ps *Parser) buildContentFingerprint(unit *html.Node) contentFingerprint {
	tag := dom.TagName(unit)

	var text string
	if tag == "pre" {
		text = dom.TextContent(unit)
	} else {
		text = strings.Join(strings.Fields(dom.TextContent(unit)), " ")
	}

	var resources []contentResource
	var walk func(*html.Node)
	walk = func(node *html.Node) {
		if node.Type == html.ElementNode {
			switch dom.TagName(node) {
			case "a":
				if href := dom.GetAttribute(node, "href"); href != "" {
					resources = append(resources, contentResource{
						tag: "a",
						url: toAbsoluteURI(href, ps.documentURI),
					})
				}
			case "img":
				if src := dom.GetAttribute(node, "src"); src != "" {
					resources = append(resources, contentResource{
						tag: "img",
						url: toAbsoluteURI(src, ps.documentURI),
						alt: dom.GetAttribute(node, "alt"),
					})
				}
			}
		}

		for child := node.FirstChild; child != nil; child = child.NextSibling {
			walk(child)
		}
	}
	walk(unit)

	fp := contentFingerprint{
		tag:       tag,
		text:      text,
		resources: resources,
	}

	var builder strings.Builder
	writeField := func(value string) {
		builder.WriteString(value)
		builder.WriteByte(0)
	}
	writeField(fp.tag)
	writeField(fp.text)
	for _, resource := range fp.resources {
		writeField(resource.tag)
		writeField(resource.url)
		writeField(resource.alt)
	}
	fp.hash = sha256.Sum256([]byte(builder.String()))

	return fp
}

// findDuplicateDedupRun looks for the best earlier, non-overlapping run
// matching the run that starts at dupStart. It returns the front start
// and the matched length (zero when nothing qualifies). The longest run
// wins; on equal length the earliest front start wins.
func findDuplicateDedupRun(
	units []*html.Node,
	fingerprints []contentFingerprint,
	hashIndex map[[sha256.Size]byte][]int,
	separators []bool,
	dupStart int,
) (frontStart int, runLength int) {
	for _, candidate := range hashIndex[fingerprints[dupStart].hash] {
		if candidate >= dupStart {
			continue
		}

		// The two runs must sit in one uninterrupted sequence: any
		// separator between the earlier unit and the later start
		// (inside either run or between the two copies) forbids the
		// match. A plain unit in between is not a separator and is
		// handled by the fingerprint/overlap checks below.
		blocked := false
		for k := candidate + 1; k <= dupStart; k++ {
			if separators[k] {
				blocked = true
				break
			}
		}
		if blocked {
			continue
		}

		length := 1
		for {
			dupIndex := dupStart + length
			frontIndex := candidate + length

			// Keep the two runs non-overlapping.
			if dupIndex >= len(units) || frontIndex >= dupStart {
				break
			}

			// Both sequences must stay contiguous: a separator
			// extending either run breaks it.
			if separators[dupIndex] || separators[frontIndex] {
				break
			}

			if fingerprints[dupIndex].hash != fingerprints[frontIndex].hash ||
				!fingerprints[dupIndex].equal(fingerprints[frontIndex]) {
				break
			}

			length++
		}

		if length < 3 {
			continue
		}

		if length > runLength || (length == runLength && candidate < frontStart) {
			frontStart = candidate
			runLength = length
		}
	}

	return frontStart, runLength
}

// planDedupAnchorRetargets validates the in-page references touching a
// candidate duplicate run and computes the link rewrites. A run is only
// removable when every fragment link pointing at it can be repointed to
// a single, uniquely identified kept unit; otherwise the whole run is
// kept.
func (ps *Parser) planDedupAnchorRetargets(
	doc *html.Node,
	units []*html.Node,
	frontStart, dupStart, runLength int,
) ([]anchorRetarget, bool) {
	dupUnits := units[dupStart : dupStart+runLength]
	frontUnits := units[frontStart : frontStart+runLength]

	// Index every non-empty id in this round's document.
	idOwners := make(map[string][]*html.Node)
	for _, element := range dom.GetElementsByTagName(doc, "*") {
		if id := dom.ID(element); id != "" {
			idOwners[id] = append(idOwners[id], element)
		}
	}

	var retargets []anchorRetarget

	for _, link := range dom.GetElementsByTagName(doc, "a") {
		href := dom.GetAttribute(link, "href")
		if !strings.HasPrefix(href, "#") {
			continue
		}

		// Decode percent escapes once. PathUnescape, unlike
		// QueryUnescape, leaves '+' literally untouched.
		fragment := href[1:]
		if decoded, err := nurl.PathUnescape(fragment); err == nil {
			fragment = decoded
		}
		if fragment == "" {
			continue
		}

		owners := idOwners[fragment]
		if len(owners) == 0 {
			// Already dangling; nothing we can make worse.
			continue
		}

		for _, owner := range owners {
			if offset := indexOfDedupUnit(dupUnits, owner); offset >= 0 {
				// A link aimed at the root id of a to-delete unit:
				// the id must be unique and the kept counterpart
				// must have a unique non-empty id of its own.
				if len(owners) != 1 {
					return nil, false
				}

				front := frontUnits[offset]
				frontID := dom.ID(front)
				if frontID == "" || len(idOwners[frontID]) != 1 {
					return nil, false
				}

				retargets = append(retargets, anchorRetarget{link: link, newID: frontID})
				continue
			}

			// A link aimed at a descendant id inside a to-delete
			// unit cannot be mapped unambiguously.
			for _, dupUnit := range dupUnits {
				if dedupNodeContains(dupUnit, owner) {
					return nil, false
				}
			}
		}
	}

	return retargets, true
}

// indexOfDedupUnit returns the position of node in units, or -1.
func indexOfDedupUnit(units []*html.Node, node *html.Node) int {
	for i, unit := range units {
		if unit == node {
			return i
		}
	}
	return -1
}

// dedupNodeContains reports whether descendant lives inside ancestor.
func dedupNodeContains(ancestor, descendant *html.Node) bool {
	for node := descendant.Parent; node != nil; node = node.Parent {
		if node == ancestor {
			return true
		}
	}
	return false
}

// hasDedupSeparator reports whether anything other than transparent
// shells and inline wrappers sits between two consecutive units.
func hasDedupSeparator(before, after *html.Node) bool {
	node := after
	for node != nil {
		for sibling := node.PrevSibling; sibling != nil; sibling = sibling.PrevSibling {
			// Reached the subtree that holds the earlier unit: all
			// intervening content has already been checked.
			if sibling == before || dedupNodeContains(sibling, before) {
				return false
			}

			if dedupNodeBlocks(sibling) {
				return true
			}
		}

		node = node.Parent
	}
	return false
}

// dedupNodeBlocks reports whether a node encountered between two units
// separates them. Non-empty text and standalone media always block.
// Block-level structures block, while transparent shells and inline
// wrappers only block through their contents.
func dedupNodeBlocks(node *html.Node) bool {
	switch node.Type {
	case html.TextNode:
		return strings.TrimSpace(node.Data) != ""
	case html.ElementNode:
		tag := dom.TagName(node)
		if tag == "p" || tag == "pre" {
			return true
		}
		if _, media := dedupMediaTags[tag]; media {
			return true
		}
		if _, block := dedupBlockTags[tag]; block {
			return true
		}

		for child := node.FirstChild; child != nil; child = child.NextSibling {
			if dedupNodeBlocks(child) {
				return true
			}
		}
	}
	return false
}

// isMediaOnlyDedupUnit reports whether a p/pre unit carries no text and
// only wraps standalone media. The prepping pass creates such units from
// loose media between paragraphs; they separate sequences rather than
// participate in them.
func isMediaOnlyDedupUnit(unit *html.Node) bool {
	if strings.TrimSpace(dom.TextContent(unit)) != "" {
		return false
	}

	for tag := range dedupMediaTags {
		if len(dom.GetElementsByTagName(unit, tag)) > 0 {
			return true
		}
	}
	return false
}
