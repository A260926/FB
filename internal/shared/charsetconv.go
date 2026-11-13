package shared

import (
	"encoding/xml"
	"fmt"
	"io"
	"net/url"
	"strings"

	xpp "github.com/mmcdole/goxpp/v2"
	"golang.org/x/net/html/charset"
)

func NewReaderLabel(label string, input io.Reader) (io.Reader, error) {
	conv, err := charset.NewReaderLabel(label, input)

	if err != nil {
		return nil, err
	}

	return conv, nil
}

// xmlNamespace is the reserved namespace of the xml prefix.
const xmlNamespace = "http://www.w3.org/XML/1998/namespace"

// ParseBaseURL validates a caller-supplied document base. Only an absolute
// HTTP(S) URL with a host is accepted; empty, relative and unparseable values
// are rejected. The fragment is dropped, since it plays no part in resolving
// document references.
func ParseBaseURL(raw string) (*url.URL, error) {
	if strings.TrimSpace(raw) == "" {
		return nil, fmt.Errorf("gofeed: base URL must not be empty")
	}
	u, err := url.Parse(raw)
	if err != nil {
		return nil, fmt.Errorf("gofeed: invalid base URL %q: %w", raw, err)
	}
	switch strings.ToLower(u.Scheme) {
	case "http", "https":
	default:
		return nil, fmt.Errorf("gofeed: base URL %q must use the http or https scheme", raw)
	}
	if u.Host == "" {
		return nil, fmt.Errorf("gofeed: base URL %q must be absolute and include a host", raw)
	}
	u.Fragment = ""
	u.RawFragment = ""
	return u, nil
}

// XMLParser is the pull parser used by every gofeed format parser. It wraps
// goxpp.Parser and adds an optional document base URL: the address the
// document was retrieved from. That URL is the outermost xml:base scope, so
// a relative declaration at the root resolves against it, an absolute one
// replaces it, and nested declarations keep resolving against their current
// scope.
//
// goxpp tracks xml:base privately, so this type keeps its own parallel base
// stack fed by every advancement call. The perspective matches goxpp's: an
// end tag still describes the element it closes, and the pop is deferred until
// the next advancement.
type XMLParser struct {
	*xpp.Parser
	docBase    *url.URL
	baseStack  []*url.URL
	pendingPop bool
}

// NewXMLParser returns a pull parser configured the way every gofeed parser
// needs it: non-strict, so real-world feeds with unescaped entities and other
// common mistakes still tokenize, and with charset conversion for feeds that
// declare a non-UTF-8 encoding.
func NewXMLParser(r io.Reader) *XMLParser {
	return newXMLParser(r, nil)
}

// NewXMLParserWithBase is NewXMLParser with an initial document base. base may
// be nil, in which case it behaves like NewXMLParser. The base is held on this
// parser instance only; it is never stored on a shared parser.
func NewXMLParserWithBase(r io.Reader, base *url.URL) *XMLParser {
	return newXMLParser(r, base)
}

func newXMLParser(r io.Reader, base *url.URL) *XMLParser {
	d := xml.NewDecoder(r)
	d.Strict = false
	d.CharsetReader = NewReaderLabel
	return &XMLParser{Parser: xpp.New(d), docBase: base}
}

// BaseURL returns the base in scope for the current element. When a document
// base was supplied, it is the outermost scope: an element with no xml:base
// inherits it, a relative xml:base resolves against it, and an absolute
// xml:base replaces it. An unparseable xml:base is treated as absent, exactly
// as goxpp does.
func (p *XMLParser) BaseURL() *url.URL {
	if n := len(p.baseStack); n > 0 {
		return p.baseStack[n-1]
	}
	return p.docBase
}

// NextToken advances to the next raw token.
func (p *XMLParser) NextToken() (xpp.EventType, error) {
	return p.advance(p.Parser.NextToken)
}

// Next advances to the next token, skipping comments, PIs and directives.
func (p *XMLParser) Next() (xpp.EventType, error) {
	return p.advance(p.Parser.Next)
}

// NextTag advances to the next start or end tag.
func (p *XMLParser) NextTag() (xpp.EventType, error) {
	return p.advance(p.Parser.NextTag)
}

// NextText consumes the current element's text content.
func (p *XMLParser) NextText() (string, error) {
	if p.Parser.Event() != xpp.StartTag {
		return p.Parser.NextText()
	}
	p.applyPendingPop()
	s, err := p.Parser.NextText()
	if err == nil {
		p.observe(p.Parser.Event())
	}
	return s, err
}

// Skip consumes the current element through its matching end tag.
func (p *XMLParser) Skip() error {
	if p.Parser.Event() != xpp.StartTag {
		return p.Parser.Skip()
	}
	p.applyPendingPop()
	err := p.Parser.Skip()
	if err == nil {
		p.observe(p.Parser.Event())
	}
	return err
}

// DecodeElement unmarshals the current element into v.
func (p *XMLParser) DecodeElement(v any) error {
	if p.Parser.Event() != xpp.StartTag {
		return p.Parser.DecodeElement(v)
	}
	p.applyPendingPop()
	err := p.Parser.DecodeElement(v)
	if err == nil {
		p.observe(p.Parser.Event())
	}
	return err
}

// advance wraps one advancement call, applying a deferred scope pop before
// moving and recording the scope of the token landed on.
func (p *XMLParser) advance(call func() (xpp.EventType, error)) (xpp.EventType, error) {
	p.applyPendingPop()
	event, err := call()
	if err == nil {
		p.observe(event)
	}
	return event, err
}

func (p *XMLParser) applyPendingPop() {
	if !p.pendingPop {
		return
	}
	p.pendingPop = false
	if n := len(p.baseStack); n > 0 {
		p.baseStack = p.baseStack[:n-1]
	}
}

// observe records the base scope a newly reached token belongs to. An end tag
// keeps its element's scope visible until the next advancement, mirroring
// goxpp's deferred pop.
func (p *XMLParser) observe(event xpp.EventType) {
	switch event {
	case xpp.StartTag:
		p.pushBase()
	case xpp.EndTag:
		p.pendingPop = true
	}
}

// pushBase mirrors goxpp's xml:base handling using the document base, when
// present, as the outermost parent scope.
func (p *XMLParser) pushBase() {
	var parent *url.URL
	if n := len(p.baseStack); n > 0 {
		parent = p.baseStack[n-1]
	} else {
		parent = p.docBase
	}

	// Match goxpp: the first xml:base attribute in the element's own
	// namespace wins. An absent, empty or unparseable declaration leaves the
	// element inheriting its parent scope.
	var raw string
	for _, attr := range p.Attrs() {
		if attr.Name.Local == "base" && attr.Name.Space == xmlNamespace {
			raw = attr.Value
			break
		}
	}
	if raw == "" {
		p.baseStack = append(p.baseStack, parent)
		return
	}
	u, err := url.Parse(raw)
	if err != nil {
		p.baseStack = append(p.baseStack, parent)
		return
	}
	if parent != nil {
		u = parent.ResolveReference(u)
	}
	p.baseStack = append(p.baseStack, u)
}
