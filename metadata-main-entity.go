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
	"encoding/json"
	"fmt"
	nurl "net/url"
	"strings"
	"unicode/utf8"

	"github.com/go-shiori/dom"
	"golang.org/x/net/html"
)

// jsonLdRegistry indexes every object found in the page's JSON-LD scripts by
// its resolved "@id". All state is local to a single extraction, so registries
// never leak between documents, including concurrent extractions.
type jsonLdRegistry struct {
	base       *nurl.URL
	pageID     string
	defs       map[string][]map[string]any
	allObjects []map[string]any
}

// isArticleLikeSchema reports whether the schema types belong to the bucket that
// decodeJsonLd already treats as articles (article, posting, report, blog, page
// and listing types).
func isArticleLikeSchema(schemaTypes []string) bool {
	for _, st := range schemaTypes {
		st = strings.ToLower(st)
		if strings.Contains(st, "article") || strings.Contains(st, "posting") ||
			st == "report" || st == "blog" ||
			strings.Contains(st, "page") || strings.Contains(st, "listing") {
			return true
		}
	}
	return false
}

// isWebPageLikeSchema mirrors the "page" type check used while bucketing schemas.
func isWebPageLikeSchema(schemaTypes []string) bool {
	for _, st := range schemaTypes {
		if strings.Contains(strings.ToLower(st), "page") {
			return true
		}
	}
	return false
}

// extractMainEntityArticle finds the unique main article of the page based on
// the WebPage "mainEntity" / "mainEntityOfPage" relations, together with the
// registry used to resolve its author and publisher references later. The last
// return value is false when no unique main article can be established, in
// which case the caller must skip the JSON-LD contribution entirely.
func extractMainEntityArticle(opts Options, doc *html.Node) (SchemaData, *jsonLdRegistry, bool) {
	// The page identity is the caller-provided absolute HTTP(S) URL.
	if opts.OriginalURL == nil {
		return SchemaData{}, nil, false
	}

	pageURL := *opts.OriginalURL
	if (pageURL.Scheme != "http" && pageURL.Scheme != "https") || pageURL.Host == "" {
		return SchemaData{}, nil, false
	}
	pageURL.Fragment = ""

	registry := &jsonLdRegistry{
		base:   opts.OriginalURL,
		pageID: pageURL.String(),
		defs:   map[string][]map[string]any{},
	}

	// Parse every valid JSON-LD script. A malformed script is ignored while the
	// remaining scripts still provide definitions.
	if !registry.indexScripts(opts, doc) {
		return SchemaData{}, nil, false
	}

	articleObj, ok := registry.selectMainArticle()
	if !ok {
		return SchemaData{}, nil, false
	}

	return SchemaData{
		Types: getSchemaTypes(articleObj, false),
		Data:  articleObj,
	}, registry, true
}

// indexScripts decodes all application/ld+json scripts, including top-level
// arrays and "@graph" entries, and registers every object carrying an "@id".
// It returns false when none of the scripts could be parsed.
func (r *jsonLdRegistry) indexScripts(opts Options, doc *html.Node) bool {
	scriptNodes := dom.QuerySelectorAll(doc, `script[type="application/ld+json"]`)
	parsedAny := false

	for _, script := range scriptNodes {
		jsonLdText := strings.TrimSpace(dom.TextContent(script))
		jsonLdText = html.UnescapeString(jsonLdText)
		if jsonLdText == "" {
			continue
		}

		// Decode as an array first, then as a single object.
		var dataList []map[string]any
		jsonLdByte := []byte(jsonLdText)
		if err := json.Unmarshal(jsonLdByte, &dataList); err != nil {
			var data map[string]any
			if err = json.Unmarshal(jsonLdByte, &data); err != nil {
				logWarn(opts, "error in JSON metadata extraction: %v", err)
				continue
			}
			dataList = []map[string]any{data}
		}

		parsedAny = true
		for _, data := range dataList {
			r.walk(data, true)
		}
	}

	return parsedAny
}

// walk traverses objects, arrays and "@graph" entries. Root-level objects (the
// script payload itself, top-level array items and "@graph" entries) carrying
// an "@id" are always indexed as definitions; deeper down, keyword-only objects
// such as an embedded "mainEntity": {"@id": "#x"} are references rather than
// definitions and must not collide with the object they point at.
func (r *jsonLdRegistry) walk(v any, isRoot bool) {
	switch value := v.(type) {
	case map[string]any:
		r.allObjects = append(r.allObjects, value)
		if id, ok := value["@id"].(string); ok && trim(id) != "" {
			if isRoot || !isReferenceObject(value) {
				if key := r.resolveID(id); key != "" {
					r.defs[key] = append(r.defs[key], value)
				}
			}
		}

		for key, child := range value {
			// Entries of "@graph" are top-level nodes of their own.
			r.walk(child, isRoot && key == "@graph")
		}

	case []any:
		for _, item := range value {
			r.walk(item, isRoot)
		}
	}
}

// resolveID resolves a (possibly relative) "@id" against the original page URL.
// The fragment is kept here so distinct fragment ids such as the page URL and
// "#main" stay separate; fragment ignoring only applies to page identity
// comparisons (see matchesPageIdentity).
func (r *jsonLdRegistry) resolveID(id string) string {
	u, err := nurl.Parse(id)
	if err != nil {
		return ""
	}

	return r.base.ResolveReference(u).String()
}

// matchesPageIdentity reports whether the resolved id points at this page,
// ignoring the fragment while leaving the rest of the URL untouched.
func (r *jsonLdRegistry) matchesPageIdentity(id string) bool {
	u, err := nurl.Parse(id)
	if err != nil {
		return false
	}

	resolved := r.base.ResolveReference(u)
	resolved.Fragment = ""
	return resolved.String() == r.pageID
}

// isReferenceObject reports whether an object is an embedded "@id" reference
// rather than a full definition: it carries an "@id" but no schema data
// properties, only JSON-LD keywords such as "@type".
func isReferenceObject(obj map[string]any) bool {
	if _, ok := obj["@id"].(string); !ok {
		return false
	}

	for key := range obj {
		if !strings.HasPrefix(key, "@") {
			return false
		}
	}
	return true
}

// strictLookup resolves an "@id" reference. The boolean result is false for
// missing references and cycles; hardFail is true when the referenced "@id"
// has more than one full definition, which invalidates the whole JSON-LD pass.
// The visited set is never mutated here: callers derive per-branch copies with
// withVisited so repeated references to the same object stay distinguishable
// from an actual reference cycle.
func (r *jsonLdRegistry) strictLookup(id string, visited map[string]bool) (map[string]any, bool, bool) {
	key := r.resolveID(id)
	if key == "" || visited[key] {
		// Unparseable id or reference cycle.
		return nil, false, false
	}

	switch len(r.defs[key]) {
	case 0:
		// Missing reference: the field or relation is simply unavailable.
		return nil, false, false
	case 1:
		return r.defs[key][0], true, false
	default:
		// A used "@id" with several definitions makes the data ambiguous.
		return nil, false, true
	}
}

// withVisited returns a copy of visited with key added.
func withVisited(visited map[string]bool, key string) map[string]bool {
	next := make(map[string]bool, len(visited)+1)
	for k := range visited {
		next[k] = true
	}
	next[key] = true
	return next
}

// selectMainArticle applies the relation precedence: positive WebPage
// "mainEntity" statements first, then "mainEntityOfPage" back references.
func (r *jsonLdRegistry) selectMainArticle() (map[string]any, bool) {
	candidates := map[string]map[string]any{}
	positiveDeclared := false

	// First pass: WebPage objects matching the page identity and their mainEntity.
	for _, obj := range r.allObjects {
		id, ok := obj["@id"].(string)
		if !ok || trim(id) == "" || !r.matchesPageIdentity(id) {
			continue
		}
		if !isWebPageLikeSchema(getSchemaTypes(obj, true)) {
			continue
		}

		// The page id is now referenced and must be uniquely defined.
		pageKey := r.resolveID(id)
		if len(r.defs[pageKey]) > 1 {
			return nil, false
		}

		mainEntity, exists := obj["mainEntity"]
		if !exists || isEmptyJsonLdValue(mainEntity) {
			continue
		}

		// A positive statement exists: only mainEntity targets may be selected,
		// recommended articles can never be appended afterwards. A statement
		// without any article target is not a valid one and falls through.
		visited := map[string]bool{pageKey: true}
		candidateCount := len(candidates)
		ok, hardFail := r.addMainEntityTargets(mainEntity, visited, candidates)
		if !ok || hardFail {
			return nil, false
		}
		if len(candidates) > candidateCount {
			positiveDeclared = true
		}
	}

	// Without any valid positive statement, look for articles pointing back at
	// the page through mainEntityOfPage.
	if !positiveDeclared {
		for _, obj := range r.allObjects {
			if !isArticleLikeSchema(getSchemaTypes(obj, true)) {
				continue
			}
			if !r.mainEntityOfPageMatches(obj["mainEntityOfPage"]) {
				continue
			}

			// An "@id" used to identify the selected article must be unique.
			if id, ok := obj["@id"].(string); ok && trim(id) != "" {
				if len(r.defs[r.resolveID(id)]) > 1 {
					return nil, false
				}
				candidates["id:"+r.resolveID(id)] = obj
			} else {
				candidates[inlineObjectKey(obj)] = obj
			}
		}
	}

	// Several distinct articles mean the main article is not unambiguous.
	if len(candidates) != 1 {
		return nil, false
	}

	for _, article := range candidates {
		return article, true
	}
	return nil, false
}

// addMainEntityTargets collects the article targets of a "mainEntity" value.
// Inline objects and "@id" references are both accepted; any broken reference
// (missing, cyclic or multiply defined) invalidates the whole selection.
func (r *jsonLdRegistry) addMainEntityTargets(v any, visited map[string]bool, candidates map[string]map[string]any) (bool, bool) {
	switch value := v.(type) {
	case string:
		if trim(value) == "" {
			return true, false
		}

		target, ok, hardFail := r.strictLookup(value, visited)
		if hardFail {
			return false, true
		}
		if !ok {
			return false, false
		}
		if isArticleLikeSchema(getSchemaTypes(target, true)) {
			if id, ok := target["@id"].(string); ok && trim(id) != "" {
				candidates["id:"+r.resolveID(id)] = target
			} else {
				candidates[inlineObjectKey(target)] = target
			}
		}
		// A mainEntity pointing at a non-article object is not a usable
		// statement, so it neither contributes a candidate nor aborts selection.

	case map[string]any:
		if id, ok := value["@id"].(string); ok && trim(id) != "" && isReferenceObject(value) {
			target, found, hardFail := r.strictLookup(id, visited)
			if hardFail {
				return false, true
			}
			if !found {
				return false, false
			}
			if isArticleLikeSchema(getSchemaTypes(target, true)) {
				candidates["id:"+r.resolveID(id)] = target
			}
			return true, false
		}

		if isArticleLikeSchema(getSchemaTypes(value, true)) {
			candidates[inlineObjectKey(value)] = value
		}

	case []any:
		for _, item := range value {
			ok, hardFail := r.addMainEntityTargets(item, visited, candidates)
			if hardFail {
				return false, true
			}
			if !ok {
				return false, false
			}
		}
	}

	return true, false
}

// mainEntityOfPageMatches checks whether a "mainEntityOfPage" value refers back
// to the page identity, ignoring fragments.
func (r *jsonLdRegistry) mainEntityOfPageMatches(v any) bool {
	switch value := v.(type) {
	case string:
		return r.urlMatchesIdentity(value)

	case map[string]any:
		if id, ok := value["@id"].(string); ok && trim(id) != "" {
			return r.urlMatchesIdentity(id)
		}
		for _, u := range getStringValues(value, "url") {
			if r.urlMatchesIdentity(u) {
				return true
			}
		}
		return false

	case []any:
		for _, item := range value {
			if r.mainEntityOfPageMatches(item) {
				return true
			}
		}
	}

	return false
}

// urlMatchesIdentity resolves a (possibly relative) URL against the original
// page URL before comparing it with the page identity.
func (r *jsonLdRegistry) urlMatchesIdentity(raw string) bool {
	u, err := nurl.Parse(raw)
	if err != nil {
		return false
	}

	resolved := r.base.ResolveReference(u)
	resolved.Fragment = ""
	return resolved.String() == r.pageID
}

// isEmptyJsonLdValue reports whether a relation value carries no statement.
func isEmptyJsonLdValue(v any) bool {
	switch value := v.(type) {
	case nil:
		return true
	case string:
		return trim(value) == ""
	case []any:
		return len(value) == 0
	case map[string]any:
		return len(value) == 0
	}
	return false
}

// inlineObjectKey builds a stable identity for an inline object without "@id".
func inlineObjectKey(obj map[string]any) string {
	return fmt.Sprintf("obj:%p", obj)
}

// looksLikeReference reports whether a string should be interpreted as an
// "@id" reference rather than a literal name.
func looksLikeReference(s string) bool {
	if strings.HasPrefix(s, "#") || strings.HasPrefix(s, "/") {
		return true
	}

	u, err := nurl.Parse(s)
	return err == nil && u.IsAbs()
}

// resolveReferenceField dereferences the author/publisher value of the selected
// article. References may cross scripts and chain through several objects.
// Cycles and missing references only make the field unavailable; unrelated
// objects never take their place. A hard failure means a referenced "@id" has
// several definitions and the whole JSON-LD pass must be skipped.
func (r *jsonLdRegistry) resolveReferenceField(v any, field string, visited map[string]bool) ([]any, bool) {
	switch value := v.(type) {
	case string:
		if !looksLikeReference(value) {
			// Bare string, e.g. a literal author name.
			return []any{value}, false
		}

		key := r.resolveID(value)
		target, ok, hardFail := r.strictLookup(value, visited)
		if hardFail {
			return nil, true
		}
		if !ok {
			return nil, false
		}
		return r.resolveReferenceField(target, field, withVisited(visited, key))

	case map[string]any:
		// An embedded keyword-only "@id" object is a reference, dereference it.
		if id, ok := value["@id"].(string); ok && trim(id) != "" && isReferenceObject(value) {
			key := r.resolveID(id)
			target, found, hardFail := r.strictLookup(id, visited)
			if hardFail {
				return nil, true
			}
			if !found {
				return nil, false
			}
			return r.resolveReferenceField(target, field, withVisited(visited, key))
		}

		// Keep following when the object only chains the same property.
		if _, hasName := value["name"]; !hasName {
			if nested, exists := value[field]; exists {
				return r.resolveReferenceField(nested, field, visited)
			}
		}
		return []any{value}, false

	case []any:
		var terminals []any
		for _, item := range value {
			items, hardFail := r.resolveReferenceField(item, field, visited)
			if hardFail {
				return nil, true
			}
			terminals = append(terminals, items...)
		}
		return terminals, false
	}

	return nil, false
}

// extractJsonLdFromArticle extracts article-level metadata from the single
// selected main article, keeping the same normalization and meta tag precedence
// as the legacy multi-article extraction. The boolean result is false when a
// referenced "@id" carries several definitions, in which case the whole
// JSON-LD contribution must be skipped.
func extractJsonLdFromArticle(registry *jsonLdRegistry, article SchemaData, originalMetadata Metadata) (Metadata, bool) {
	var metadata Metadata

	// Grab "author" property. It may be an inline object, a literal string, an
	// ordered array or an "@id" reference (possibly chained and cross-script).
	if authorValue, exists := article.Data["author"]; exists {
		terminals, hardFail := registry.resolveReferenceField(authorValue, "author", map[string]bool{})
		if hardFail {
			return originalMetadata, false
		}

		var authorNames string
		for _, terminal := range terminals {
			for _, author := range getSchemaNames(terminal, "person") {
				author = validateMetadataName(author)
				authorNames = normalizeAuthors(authorNames, author)
			}
		}

		if authorNames != "" {
			metadata.Author = authorNames
		}
	}

	// Grab sitename from the publisher, which supports the same reference shapes
	// (except ordered author arrays).
	if publisherValue, exists := article.Data["publisher"]; exists {
		terminals, hardFail := registry.resolveReferenceField(publisherValue, "publisher", map[string]bool{})
		if hardFail {
			return originalMetadata, false
		}

		for _, terminal := range terminals {
			if sitenames := getSchemaNames(terminal); len(sitenames) > 0 {
				metadata.Sitename = sitenames[0]
				break
			}
		}
	}

	// Grab category
	categories := getStringValues(article.Data, "articleSection")
	if len(categories) != 0 {
		metadata.Categories = append(metadata.Categories, categories...)
	}

	// Grab tags
	tags := getSchemaNames(article.Data["keywords"])
	if len(tags) > 0 {
		metadata.Tags = append(metadata.Tags, tags...)
	}

	// Grab title
	metadata.Title = getSingleStringValue(article.Data, "name")

	// If title is empty or only consist of one word, try to look in headline
	if metadata.Title == "" || strWordCount(metadata.Title) == 1 {
		for attr := range article.Data {
			if !strings.Contains(strings.ToLower(attr), "headline") {
				continue
			}

			title := getSingleStringValue(article.Data, attr)
			if title != "" && !strings.Contains(title, "...") {
				metadata.Title = title
				break
			}
		}
	}

	// If title found, use article type as page type
	if metadata.Title != "" && len(article.Types) > 0 {
		metadata.PageType = article.Types[0]
	}

	// Uniquify tags and categories
	metadata.Tags = uniquifyLists(metadata.Tags...)
	metadata.Categories = uniquifyLists(metadata.Categories...)

	// If available, override type, title, author, categories and tags in original metadata
	originalMetadata.Title = strOr(originalMetadata.Title, metadata.Title)
	originalMetadata.PageType = strOr(originalMetadata.PageType, metadata.PageType)
	originalMetadata.Author = strOr(metadata.Author, originalMetadata.Author)

	if len(metadata.Categories) > 0 {
		originalMetadata.Categories = metadata.Categories
	}

	if len(metadata.Tags) > 0 {
		originalMetadata.Tags = metadata.Tags
	}

	// If the new sitename exist and longer, override the original
	if utf8.RuneCountInString(metadata.Sitename) > utf8.RuneCountInString(originalMetadata.Sitename) {
		originalMetadata.Sitename = metadata.Sitename
	}

	return originalMetadata, true
}
