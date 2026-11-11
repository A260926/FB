// This file is part of go-trafilatura, Go package for extracting readable
// content, comments and metadata from a web page. Source available in
// <https://github.com/markusmobius/go-trafilatura>.
//
// Copyright (C) 2021 Markus Mobius
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package trafilatura

import (
	"net/url"
	"strings"
	"sync"
	"testing"

	"github.com/go-shiori/dom"
	"github.com/stretchr/testify/assert"
)

const mainEntityBody = `<body><article><p>` +
	`Collected source content has enough sentences and useful details for the reader. ` +
	`The paragraph keeps going so the extractor keeps the whole article node intact. ` +
	`</p></article></body>`

func mainEntityPageURL(t *testing.T, raw string) *url.URL {
	t.Helper()
	u, err := url.Parse(raw)
	if err != nil {
		t.Fatalf("parse url %q: %v", raw, err)
	}
	return u
}

func pageWithScripts(scripts ...string) string {
	return `<html><head>` + strings.Join(scripts, "") + `</head>` + mainEntityBody + `</html>`
}

func ldScript(content string) string {
	return `<script type="application/ld+json">` + content + `</script>`
}

func TestMainEntityExtraction(t *testing.T) {
	pageURL := mainEntityPageURL(t, "https://example.invalid/story")

	// Positive mainEntity relation inside a @graph, the scenario of TestStartingGraph.
	graphScript := ldScript(`{"@context":"https://schema.org","@graph":[
		{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}},
		{"@type":"NewsArticle","@id":"#recommend","headline":"Recommended article","author":{"@type":"Person","name":"Other Writer"}},
		{"@type":"NewsArticle","@id":"#main","headline":"Main article",
			"articleSection":"Main Section","keywords":"Main Tag",
			"author":{"@id":"#writer"},"publisher":{"@id":"#publisher"}},
		{"@type":"Person","@id":"#writer","name":"Main Writer"},
		{"@type":"Organization","@id":"#publisher","name":"Main Publisher"}]}`)
	source := pageWithScripts(graphScript)

	t.Run("default mode keeps legacy merged output", func(t *testing.T) {
		metadata := testGetMetadataFromHTML(source, Options{OriginalURL: pageURL})
		assert.Equal(t, "Recommended article", metadata.Title)
	})

	t.Run("main entity mode selects the main article", func(t *testing.T) {
		opts := Options{OriginalURL: pageURL, UseMainEntity: true}
		metadata := testGetMetadataFromHTML(source, opts)
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Main Writer", metadata.Author)
		assert.Equal(t, "Main Publisher", metadata.Sitename)
		assert.Equal(t, []string{"Main Section"}, metadata.Categories)
		assert.Equal(t, []string{"Main Tag"}, metadata.Tags)
		assert.Equal(t, "NewsArticle", metadata.PageType)
	})

	t.Run("public Extract entry keeps the article body", func(t *testing.T) {
		result, err := Extract(strings.NewReader(source), Options{
			OriginalURL:     pageURL,
			UseMainEntity:   true,
			ExcludeComments: true,
			Config:          zeroConfig,
		})
		assert.NoError(t, err)
		assert.NotNil(t, result.ContentNode)
		assert.Contains(t, result.ContentText, "Collected source content")
		assert.Equal(t, "Main article", result.Metadata.Title)
		assert.Equal(t, "Main Writer", result.Metadata.Author)
	})

	t.Run("public ExtractDocument entry behaves the same", func(t *testing.T) {
		doc := docFromStr(source)
		result, err := ExtractDocument(doc, Options{
			OriginalURL:     pageURL,
			UseMainEntity:   true,
			ExcludeComments: true,
			Config:          zeroConfig,
		})
		assert.NoError(t, err)
		assert.NotNil(t, result.ContentNode)
		assert.Contains(t, result.ContentText, "Collected source content")
		assert.Equal(t, "Main article", result.Metadata.Title)
		assert.Equal(t, "Main Writer", result.Metadata.Author)
	})

	t.Run("fragment in the original URL is ignored", func(t *testing.T) {
		fragURL := mainEntityPageURL(t, "https://example.invalid/story#section-2")
		metadata := testGetMetadataFromHTML(source, Options{OriginalURL: fragURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
	})

	t.Run("relative ids resolve against the original URL", func(t *testing.T) {
		relScript := ldScript(`{"@context":"https://schema.org","@graph":[
			{"@type":"WebPage","@id":"story","mainEntity":{"@id":"#main"}},
			{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":"Main Writer"}]}`)
		metadata := testGetMetadataFromHTML(pageWithScripts(relScript),
			Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Main Writer", metadata.Author)
	})

	t.Run("inline mainEntity object", func(t *testing.T) {
		inlineScript := ldScript(`{"@context":"https://schema.org","@graph":[
			{"@type":"WebPage","@id":"https://example.invalid/story",
				"mainEntity":{"@type":"Article","headline":"Inline main article","author":{"@type":"Person","name":"Main Writer"}}},
			{"@type":"NewsArticle","headline":"Recommended article","author":{"@type":"Person","name":"Other Writer"}}]}`)
		metadata := testGetMetadataFromHTML(pageWithScripts(inlineScript),
			Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Inline main article", metadata.Title)
		assert.Equal(t, "Main Writer", metadata.Author)
	})

	// Reverse relation: no WebPage statement at all.
	reverseScript := ldScript(`{"@context":"https://schema.org","@graph":[
		{"@type":"NewsArticle","@id":"#main","headline":"Main article",
			"mainEntityOfPage":"https://example.invalid/story","author":{"@type":"Person","name":"Main Writer"}},
		{"@type":"NewsArticle","@id":"#recommend","headline":"Recommended article","author":{"@type":"Person","name":"Other Writer"}}]}`)

	t.Run("mainEntityOfPage back reference is used without positive statement", func(t *testing.T) {
		metadata := testGetMetadataFromHTML(pageWithScripts(reverseScript),
			Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Main Writer", metadata.Author)
	})

	t.Run("mainEntityOfPage object form", func(t *testing.T) {
		objScript := ldScript(`{"@context":"https://schema.org","@graph":[
			{"@type":"Article","@id":"#main","headline":"Main article",
				"mainEntityOfPage":{"@id":"https://example.invalid/story#frag"},"author":{"@type":"Person","name":"Main Writer"}},
			{"@type":"Article","@id":"#recommend","headline":"Recommended article","author":{"@type":"Person","name":"Other Writer"}}]}`)
		metadata := testGetMetadataFromHTML(pageWithScripts(objScript),
			Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
	})

	t.Run("positive statement wins over mainEntityOfPage articles", func(t *testing.T) {
		bothScripts := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":{"@type":"Person","name":"Main Writer"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#recommend","headline":"Recommended article",
				"mainEntityOfPage":"https://example.invalid/story","author":{"@type":"Person","name":"Other Writer"}}`))
		metadata := testGetMetadataFromHTML(bothScripts, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Main Writer", metadata.Author)
	})

	t.Run("keyword-only graph entries with @type are still definitions", func(t *testing.T) {
		sparseGraph := pageWithScripts(
			ldScript(`{"@context":"https://schema.org","@graph":[
				{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}},
				{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":{"@id":"#writer"}},
				{"@type":"Person","@id":"#writer","name":"Main Writer"}]}`))
		metadata := testGetMetadataFromHTML(sparseGraph, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Main Writer", metadata.Author)
	})

	t.Run("relative path references resolve against the original URL", func(t *testing.T) {
		pathRef := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":"/story#main"}`),
			ldScript(`{"@type":"Article","@id":"/story#main","headline":"Main article","author":"Main Writer"}`))
		metadata := testGetMetadataFromHTML(pathRef, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Main Writer", metadata.Author)
	})

	t.Run("author and publisher references cross scripts and chain", func(t *testing.T) {
		crossScripts := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article",
				"author":[{"@id":"#writer1"},{"@type":"Person","name":"Grace Hopper"}],
				"publisher":{"@id":"#org1"}}`),
			ldScript(`{"@type":"Person","@id":"#writer1","name":"Ada Lovelace"}`),
			ldScript(`{"@type":"Organization","@id":"#org1","publisher":{"@id":"#org2"}}`),
			ldScript(`{"@type":"Organization","@id":"#org2","name":"Chained Publisher"}`))
		metadata := testGetMetadataFromHTML(crossScripts, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Ada Lovelace; Grace Hopper", metadata.Author)
		assert.Equal(t, "Chained Publisher", metadata.Sitename)
	})

	t.Run("repeated declarations of the same object count once", func(t *testing.T) {
		dupDecl := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":[{"@id":"#main"},{"@id":"#main"}]}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":{"@type":"Person","name":"Main Writer"}}`))
		metadata := testGetMetadataFromHTML(dupDecl, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
	})

	t.Run("distinct mainEntity targets are ambiguous", func(t *testing.T) {
		ambiguous := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":[{"@id":"#a"},{"@id":"#b"}]}`),
			ldScript(`{"@type":"Article","@id":"#a","headline":"Article A"}`),
			ldScript(`{"@type":"Article","@id":"#b","headline":"Article B"}`),
			`<title>Fallback Title</title>`)
		metadata := testGetMetadataFromHTML(ambiguous, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Fallback Title", metadata.Title)
		assert.Empty(t, metadata.Author)
	})

	t.Run("missing mainEntity reference skips JSON-LD", func(t *testing.T) {
		missing := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#missing"}}`),
			ldScript(`{"@type":"NewsArticle","headline":"Recommended article","author":{"@type":"Person","name":"Other Writer"}}`),
			`<title>Fallback Title</title>`)
		metadata := testGetMetadataFromHTML(missing, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Fallback Title", metadata.Title)
		assert.Empty(t, metadata.Author)
	})

	t.Run("cyclic main article author reference only drops the field", func(t *testing.T) {
		cyclic := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":{"@id":"#p1"}}`),
			ldScript(`{"@type":"Person","@id":"#p1","author":{"@id":"#main"}}`),
			`<meta name="author" content="Meta Writer">`)
		metadata := testGetMetadataFromHTML(cyclic, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Meta Writer", metadata.Author)
	})

	t.Run("mainEntity pointing back at the page is a cycle and skips JSON-LD", func(t *testing.T) {
		cyclic := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"https://example.invalid/story"}}`),
			ldScript(`{"@type":"NewsArticle","headline":"Recommended article","author":{"@type":"Person","name":"Other Writer"}}`),
			`<title>Fallback Title</title>`)
		metadata := testGetMetadataFromHTML(cyclic, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Fallback Title", metadata.Title)
		assert.Empty(t, metadata.Author)
	})

	t.Run("several mainEntityOfPage articles are ambiguous", func(t *testing.T) {
		ambiguous := pageWithScripts(
			ldScript(`{"@type":"Article","@id":"#a","headline":"Article A","mainEntityOfPage":"https://example.invalid/story"}`),
			ldScript(`{"@type":"Article","@id":"#b","headline":"Article B","mainEntityOfPage":{"@id":"https://example.invalid/story"}}`),
			`<title>Fallback Title</title>`)
		metadata := testGetMetadataFromHTML(ambiguous, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Fallback Title", metadata.Title)
	})

	t.Run("duplicate definition used by an author skips the whole JSON-LD pass", func(t *testing.T) {
		dupAuthor := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":{"@id":"#writer"}}`),
			ldScript(`{"@type":"Person","@id":"#writer","name":"Writer One"}`),
			ldScript(`{"@type":"Person","@id":"#writer","name":"Writer Two"}`),
			`<title>Fallback Title</title>`)
		metadata := testGetMetadataFromHTML(dupAuthor, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Fallback Title", metadata.Title)
		assert.Empty(t, metadata.Author)
	})

	t.Run("missing publisher reference only drops the JSON-LD sitename", func(t *testing.T) {
		missingPublisher := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article",
				"author":{"@type":"Person","name":"Main Writer"},"publisher":{"@id":"#gone"}}`),
			ldScript(`{"@type":"Organization","name":"Unrelated Org"}`))
		metadata := testGetMetadataFromHTML(missingPublisher, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Main Writer", metadata.Author)
		// The broken publisher reference never gets replaced by the unrelated
		// organization; the URL-derived hostname fallback is what remains.
		assert.NotEqual(t, "Unrelated Org", metadata.Sitename)
		assert.Equal(t, "example.invalid", metadata.Sitename)
	})

	t.Run("used id with several definitions skips JSON-LD", func(t *testing.T) {
		dupUsed := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article"}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Duplicate article"}`),
			`<title>Fallback Title</title>`)
		metadata := testGetMetadataFromHTML(dupUsed, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Fallback Title", metadata.Title)
	})

	t.Run("unused duplicate id does not poison a clear relation", func(t *testing.T) {
		dupUnused := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":{"@type":"Person","name":"Main Writer"}}`),
			ldScript(`{"@type":"Thing","@id":"#unused","name":"One"}`),
			ldScript(`{"@type":"Thing","@id":"#unused","name":"Two"}`))
		metadata := testGetMetadataFromHTML(dupUnused, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Main Writer", metadata.Author)
	})

	t.Run("malformed script is ignored, other scripts still apply", func(t *testing.T) {
		withBroken := pageWithScripts(
			`{"@type":"Article","headline":"Broken`,
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":{"@type":"Person","name":"Main Writer"}}`))
		metadata := testGetMetadataFromHTML(withBroken, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Main Writer", metadata.Author)
	})

	t.Run("result is independent of script order", func(t *testing.T) {
		first := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":{"@id":"#writer"}}`),
			ldScript(`{"@type":"Person","@id":"#writer","name":"Main Writer"}`))
		second := pageWithScripts(
			ldScript(`{"@type":"Person","@id":"#writer","name":"Main Writer"}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":{"@id":"#writer"}}`),
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`))

		opts := Options{OriginalURL: pageURL, UseMainEntity: true}
		assert.Equal(t, testGetMetadataFromHTML(first, opts), testGetMetadataFromHTML(second, opts))
	})

	t.Run("blacklisted main article author is removed", func(t *testing.T) {
		blacklisted := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article","author":{"@type":"Person","name":"Main Writer"}}`))
		metadata := testGetMetadataFromHTML(blacklisted, Options{
			OriginalURL:        pageURL,
			UseMainEntity:      true,
			BlacklistedAuthors: []string{"Main Writer"},
		})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Empty(t, metadata.Author)
	})

	t.Run("meta author survives when the main article provides none", func(t *testing.T) {
		noAuthor := pageWithScripts(
			ldScript(`{"@type":"WebPage","@id":"https://example.invalid/story","mainEntity":{"@id":"#main"}}`),
			ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Main article"}`),
			`<meta name="author" content="Backup Writer">`)
		metadata := testGetMetadataFromHTML(noAuthor, Options{OriginalURL: pageURL, UseMainEntity: true})
		assert.Equal(t, "Main article", metadata.Title)
		assert.Equal(t, "Backup Writer", metadata.Author)
	})

	t.Run("missing original URL skips JSON-LD", func(t *testing.T) {
		withFallback := strings.Replace(source, "</head>", `<title>Fallback Title</title></head>`, 1)
		metadata := testGetMetadataFromHTML(withFallback, Options{UseMainEntity: true})
		assert.Equal(t, "Fallback Title", metadata.Title)
		assert.Empty(t, metadata.Author)
	})

	t.Run("non HTTP(S) original URL skips JSON-LD", func(t *testing.T) {
		ftpURL := mainEntityPageURL(t, "ftp://example.invalid/story")
		withFallback := strings.Replace(source, "</head>", `<title>Fallback Title</title></head>`, 1)
		metadata := testGetMetadataFromHTML(withFallback, Options{OriginalURL: ftpURL, UseMainEntity: true})
		assert.Equal(t, "Fallback Title", metadata.Title)
		assert.Empty(t, metadata.Author)
	})
}

func TestMainEntityIsolation(t *testing.T) {
	pageURL := mainEntityPageURL(t, "https://example.invalid/shared")

	// Two documents reuse the same "@id" values but describe different articles.
	docA := pageWithScripts(
		ldScript(`{"@type":"WebPage","@id":"https://example.invalid/shared","mainEntity":{"@id":"#main"}}`),
		ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Article A","author":{"@id":"#writer"}}`),
		ldScript(`{"@type":"Person","@id":"#writer","name":"Writer A"}`))
	docB := pageWithScripts(
		ldScript(`{"@context":"https://schema.org","@graph":[{"@type":"WebPage","@id":"https://example.invalid/shared","mainEntity":{"@id":"#main"}}]}`),
		ldScript(`{"@type":"NewsArticle","@id":"#main","headline":"Article B","author":{"@id":"#writer"}}`),
		ldScript(`{"@type":"Person","@id":"#writer","name":"Writer B"}`))

	opts := Options{OriginalURL: pageURL, UseMainEntity: true, Config: zeroConfig}

	t.Run("sequential interleaved calls keep per-document indexes", func(t *testing.T) {
		mA := testGetMetadataFromHTML(docA, opts)
		mB := testGetMetadataFromHTML(docB, opts)
		mA2 := testGetMetadataFromHTML(docA, opts)
		mB2 := testGetMetadataFromHTML(docB, opts)

		assert.Equal(t, "Article A", mA.Title)
		assert.Equal(t, "Writer A", mA.Author)
		assert.Equal(t, "Article B", mB.Title)
		assert.Equal(t, "Writer B", mB.Author)
		assert.Equal(t, mA, mA2)
		assert.Equal(t, mB, mB2)
	})

	t.Run("concurrent interleaved calls keep per-document indexes", func(t *testing.T) {
		var wg sync.WaitGroup
		errs := make(chan error, 64)
		extract := func(source, wantTitle, wantAuthor string) {
			defer wg.Done()
			result, err := Extract(strings.NewReader(source), Options{
				OriginalURL:     pageURL,
				UseMainEntity:   true,
				ExcludeComments: true,
				Config:          zeroConfig,
			})
			if err != nil {
				errs <- err
				return
			}
			if result.Metadata.Title != wantTitle || result.Metadata.Author != wantAuthor {
				errs <- &isolationError{title: result.Metadata.Title, author: result.Metadata.Author}
			}
		}

		for i := 0; i < 32; i++ {
			wg.Add(2)
			go extract(docA, "Article A", "Writer A")
			go extract(docB, "Article B", "Writer B")
		}

		wg.Wait()
		close(errs)
		for err := range errs {
			assert.NoError(t, err)
		}
	})

	t.Run("ExtractDocument does not modify the supplied tree", func(t *testing.T) {
		doc := docFromStr(docA)
		before := dom.OuterHTML(doc)

		_, err := ExtractDocument(doc, Options{
			OriginalURL:     pageURL,
			UseMainEntity:   true,
			ExcludeComments: true,
			Config:          zeroConfig,
		})
		assert.NoError(t, err)

		after := dom.OuterHTML(doc)
		assert.Equal(t, before, after)

		// The script nodes and their text are still reachable on the original tree.
		scripts := dom.QuerySelectorAll(doc, `script[type="application/ld+json"]`)
		assert.Len(t, scripts, 3)
		assert.Contains(t, dom.TextContent(scripts[1]), "Article A")
	})

	t.Run("node identity is preserved through the call", func(t *testing.T) {
		doc := docFromStr(docA)
		originalChild := doc.FirstChild
		_, err := ExtractDocument(doc, Options{
			OriginalURL:     pageURL,
			UseMainEntity:   true,
			ExcludeComments: true,
			Config:          zeroConfig,
		})
		assert.NoError(t, err)
		assert.Same(t, originalChild, doc.FirstChild)
	})
}

type isolationError struct {
	title  string
	author string
}

func (e *isolationError) Error() string {
	return "unexpected metadata: title=" + e.title + " author=" + e.author
}
