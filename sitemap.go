package gocrawl

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"encoding/xml"
	"errors"
	"io"
	"io/ioutil"
	"strings"
)

const (
	// sitemapNamespace is the XML namespace of the sitemap protocol, as
	// described by http://www.sitemaps.org/protocol.php. Documents that
	// use no namespace at all are also accepted, but loc elements found
	// in any other (extension) namespace are ignored.
	sitemapNamespace = "http://www.sitemaps.org/schemas/sitemap/0.9"

	// maxSitemapBodySize is the maximum size in bytes of a sitemap's
	// content, once decompressed.
	maxSitemapBodySize = 8 << 20 // 8 MiB

	// maxSitemapLocCount is the maximum number of valid loc entries
	// accepted from a single sitemap document.
	maxSitemapLocCount = 10000
)

var (
	// errSitemapTooLarge is returned when a sitemap's decompressed content
	// exceeds maxSitemapBodySize.
	errSitemapTooLarge = errors.New("sitemap content exceeds the maximum size")

	// errSitemapTooManyLocs is returned when a sitemap holds more than
	// maxSitemapLocCount valid loc entries.
	errSitemapTooManyLocs = errors.New("sitemap exceeds the maximum number of loc entries")
)

// readSitemapBody reads a sitemap response body and returns its content.
// If the first bytes are the gzip magic number, the content is decompressed
// once - this is independent of (and in addition to) any HTTP Content-Encoding
// already handled by Fetch, so the response headers are not consulted. The
// decompressed content is limited to maxSitemapBodySize bytes.
func readSitemapBody(r io.Reader) ([]byte, error) {
	br := bufio.NewReader(r)
	var src io.Reader = br

	// Sniff the first bytes for the gzip magic number (0x1f 0x8b)
	if head, err := br.Peek(2); err == nil && head[0] == 0x1f && head[1] == 0x8b {
		gz, err := gzip.NewReader(br)
		if err != nil {
			return nil, err
		}
		defer gz.Close()
		// Decompress a single stream
		gz.Multistream(false)
		src = gz
	}

	// Read at most one byte more than the limit, to detect overflow
	body, err := ioutil.ReadAll(io.LimitReader(src, maxSitemapBodySize+1))
	if err != nil {
		return nil, err
	}
	if len(body) > maxSitemapBodySize {
		return nil, errSitemapTooLarge
	}
	return body, nil
}

// parseSitemapDocument parses a sitemap XML document and returns whether it
// is a sitemap index (true) or a URL set (false), along with the raw loc
// values found in the document. The loc of an entry (sitemap or url) is only
// collected when it is a direct child of that entry, in the document's
// namespace. Malformed XML, an unexpected root element and documents over
// the loc count limit return an error, and no partial result is returned
// in those cases.
func parseSitemapDocument(data []byte) (isIndex bool, locs []string, err error) {
	var (
		docNS    string
		rootSeen bool
		depth    int
		inEntry  bool
		inLoc    bool
		locText  []byte
	)

	dec := xml.NewDecoder(bytes.NewReader(data))
	for {
		tok, terr := dec.Token()
		if terr == io.EOF {
			break
		}
		if terr != nil {
			return false, nil, terr
		}
		switch t := tok.(type) {
		case xml.StartElement:
			depth++
			switch {
			case !rootSeen:
				// The root element must be a sitemapindex or a urlset, either
				// in the sitemap namespace or in no namespace at all.
				if t.Name.Space != "" && t.Name.Space != sitemapNamespace {
					return false, nil, errors.New("unsupported sitemap root namespace: " + t.Name.Space)
				}
				switch t.Name.Local {
				case "sitemapindex":
					isIndex = true
				case "urlset":
					isIndex = false
				default:
					return false, nil, errors.New("unexpected sitemap root element: " + t.Name.Local)
				}
				docNS = t.Name.Space
				rootSeen = true

			case depth == 2 && !inEntry && t.Name.Space == docNS && t.Name.Local == sitemapEntryName(isIndex):
				inEntry = true

			case depth == 3 && inEntry && !inLoc && t.Name.Space == docNS && t.Name.Local == "loc":
				inLoc = true
				locText = locText[:0]
			}

		case xml.CharData:
			if inLoc {
				locText = append(locText, t...)
			}

		case xml.EndElement:
			if inLoc && depth == 3 && t.Name.Space == docNS && t.Name.Local == "loc" {
				if s := strings.TrimSpace(string(locText)); s != "" {
					locs = append(locs, s)
					if len(locs) > maxSitemapLocCount {
						return false, nil, errSitemapTooManyLocs
					}
				}
				inLoc = false
			} else if inEntry && depth == 2 {
				inEntry = false
			}
			depth--
		}
	}
	if !rootSeen {
		return false, nil, errors.New("empty sitemap document")
	}
	return isIndex, locs, nil
}

// sitemapEntryName returns the name of the entry element of a sitemap
// document: sitemap for an index, url for a URL set.
func sitemapEntryName(isIndex bool) string {
	if isIndex {
		return "sitemap"
	}
	return "url"
}
