package client

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/url"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

// LinkDecoder creates a PageDecoder for STAC link-based pagination.
//
// Items are extracted from itemsField (e.g. "features", "collections"); the
// next page is taken from the first link with rel="next". numberMatched and
// numberReturned are populated when present at the top level (STAC API 1.x)
// or under a legacy "context" object (deprecated extension).
func LinkDecoder[V any](itemsField string) PageDecoder[V] {
	return func(resp *http.Response) (*PageResult[V], error) {
		var raw map[string]json.RawMessage
		if err := json.NewDecoder(resp.Body).Decode(&raw); err != nil {
			return nil, err
		}

		items, err := decodeItems[V](raw, itemsField)
		if err != nil {
			return nil, err
		}

		var links []*stac.Link
		if data, ok := raw["links"]; ok {
			if err := json.Unmarshal(data, &links); err != nil {
				return nil, fmt.Errorf("decode links: %w", err)
			}
		}

		matched, returned := decodeCounts(raw)

		var next *Request
		for _, l := range links {
			if l != nil && l.Rel == "next" {
				next = LinkToRequest(l)
				break
			}
		}

		return &PageResult[V]{
			Items:          items,
			Next:           next,
			NumberMatched:  matched,
			NumberReturned: returned,
		}, nil
	}
}

// ItemDecoder returns a PageDecoder for standard STAC item responses.
func ItemDecoder() PageDecoder[stac.Item] { return LinkDecoder[stac.Item]("features") }

// CollectionDecoder returns a PageDecoder for standard STAC collection responses.
func CollectionDecoder() PageDecoder[stac.Collection] {
	return LinkDecoder[stac.Collection]("collections")
}

// CursorDecoder creates a PageDecoder factory for cursor-based pagination.
//
// Each call returns a fresh decoder so it is safe to reuse the factory across
// independent iterations. Items are extracted from itemsField; the cursor
// value is read from cursorField; the next request is built by adding/replacing
// the configured cursor query parameter on endpoint.
func CursorDecoder[V any](itemsField, cursorField, endpoint, cursorParam string) PageDecoder[V] {
	if cursorParam == "" {
		cursorParam = "cursor"
	}
	return func(resp *http.Response) (*PageResult[V], error) {
		var raw map[string]json.RawMessage
		if err := json.NewDecoder(resp.Body).Decode(&raw); err != nil {
			return nil, err
		}
		items, err := decodeItems[V](raw, itemsField)
		if err != nil {
			return nil, err
		}
		var cursor string
		if data, ok := raw[cursorField]; ok {
			if err := json.Unmarshal(data, &cursor); err != nil {
				return nil, fmt.Errorf("decode %s: %w", cursorField, err)
			}
		}
		var next *Request
		if cursor != "" {
			u, err := mergeQuery(endpoint, map[string]string{cursorParam: cursor})
			if err != nil {
				return nil, err
			}
			next = &Request{URL: u}
		}
		return &PageResult[V]{Items: items, Next: next}, nil
	}
}

// OffsetDecoder creates a PageDecoder factory for offset/limit pagination.
//
// Each call returns a fresh stateful decoder. Items are extracted from
// itemsField; total from totalField. The factory's offset starts at zero and
// advances by len(items) per page; iteration stops when offset >= total.
func OffsetDecoder[V any](itemsField, totalField, endpoint string, limit int) PageDecoder[V] {
	if limit <= 0 {
		limit = 100
	}
	offset := 0
	return func(resp *http.Response) (*PageResult[V], error) {
		var raw map[string]json.RawMessage
		if err := json.NewDecoder(resp.Body).Decode(&raw); err != nil {
			return nil, err
		}
		items, err := decodeItems[V](raw, itemsField)
		if err != nil {
			return nil, err
		}
		var total int
		if data, ok := raw[totalField]; ok {
			if err := json.Unmarshal(data, &total); err != nil {
				return nil, fmt.Errorf("decode %s: %w", totalField, err)
			}
		}
		offset += len(items)
		var next *Request
		if offset < total {
			u, err := mergeQuery(endpoint, map[string]string{
				"offset": fmt.Sprintf("%d", offset),
				"limit":  fmt.Sprintf("%d", limit),
			})
			if err != nil {
				return nil, err
			}
			next = &Request{URL: u}
		}
		return &PageResult[V]{Items: items, Next: next}, nil
	}
}

// PageNumberDecoder creates a PageDecoder factory for page-number pagination.
func PageNumberDecoder[V any](itemsField, totalPagesField, endpoint string) PageDecoder[V] {
	page := 1
	return func(resp *http.Response) (*PageResult[V], error) {
		var raw map[string]json.RawMessage
		if err := json.NewDecoder(resp.Body).Decode(&raw); err != nil {
			return nil, err
		}
		items, err := decodeItems[V](raw, itemsField)
		if err != nil {
			return nil, err
		}
		var totalPages int
		if data, ok := raw[totalPagesField]; ok {
			if err := json.Unmarshal(data, &totalPages); err != nil {
				return nil, fmt.Errorf("decode %s: %w", totalPagesField, err)
			}
		}
		page++
		var next *Request
		if page <= totalPages {
			u, err := mergeQuery(endpoint, map[string]string{"page": fmt.Sprintf("%d", page)})
			if err != nil {
				return nil, err
			}
			next = &Request{URL: u}
		}
		return &PageResult[V]{Items: items, Next: next}, nil
	}
}

// HeaderTokenDecoder creates a PageDecoder for header-based token pagination.
//
// The continuation token is read from tokenHeader on each response and sent
// back as the same header on the next request. Endpoint is the static URL of
// subsequent pages.
func HeaderTokenDecoder[V any](itemsField, tokenHeader, endpoint string) PageDecoder[V] {
	return func(resp *http.Response) (*PageResult[V], error) {
		var raw map[string]json.RawMessage
		if err := json.NewDecoder(resp.Body).Decode(&raw); err != nil {
			return nil, err
		}
		items, err := decodeItems[V](raw, itemsField)
		if err != nil {
			return nil, err
		}
		var next *Request
		if token := resp.Header.Get(tokenHeader); token != "" {
			next = &Request{
				URL:     endpoint,
				Headers: map[string]string{tokenHeader: token},
			}
		}
		return &PageResult[V]{Items: items, Next: next}, nil
	}
}

func decodeItems[V any](raw map[string]json.RawMessage, field string) ([]*V, error) {
	var items []*V
	if data, ok := raw[field]; ok && len(data) > 0 {
		if err := json.Unmarshal(data, &items); err != nil {
			return nil, fmt.Errorf("decode %s: %w", field, err)
		}
	}
	return items, nil
}

// decodeCounts extracts numberMatched/numberReturned from the response, with
// a fallback to the deprecated context extension. Unknown shapes are ignored.
func decodeCounts(raw map[string]json.RawMessage) (matched, returned *int) {
	if data, ok := raw["numberMatched"]; ok {
		var n int
		if json.Unmarshal(data, &n) == nil {
			matched = &n
		}
	}
	if data, ok := raw["numberReturned"]; ok {
		var n int
		if json.Unmarshal(data, &n) == nil {
			returned = &n
		}
	}
	if matched == nil || returned == nil {
		if data, ok := raw["context"]; ok {
			var ctx struct {
				Matched  *int `json:"matched"`
				Returned *int `json:"returned"`
			}
			if json.Unmarshal(data, &ctx) == nil {
				if matched == nil && ctx.Matched != nil {
					matched = ctx.Matched
				}
				if returned == nil && ctx.Returned != nil {
					returned = ctx.Returned
				}
			}
		}
	}
	return matched, returned
}

// mergeQuery parses endpoint, sets each (k,v) on its query, and returns the
// re-encoded URL. Existing query values for the keys are replaced.
func mergeQuery(endpoint string, kv map[string]string) (string, error) {
	u, err := url.Parse(endpoint)
	if err != nil {
		return "", fmt.Errorf("stac: invalid endpoint %q: %w", endpoint, err)
	}
	q := u.Query()
	for k, v := range kv {
		q.Set(k, v)
	}
	u.RawQuery = q.Encode()
	return u.String(), nil
}
