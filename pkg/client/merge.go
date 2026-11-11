package client

import (
	"encoding/json"
	"errors"
	"fmt"
	"strings"
)

// mergeSnapshot is a per-iteration frozen copy of the request that started a
// pagination run. STAC Item Search next-links may carry "merge": true, in
// which case the link's body and headers must be overlaid on *this* request
// (the initial one), not on the request used for the previous page.
//
// The body is stored as canonical JSON bytes so that struct, map, and
// json.RawMessage initial bodies all have the same JSON-object semantics,
// and so the caller's maps/slices can never be aliased or written back.
// Method and URL are not snapshotted: those always come from the next link
// itself.
type mergeSnapshot struct {
	body    json.RawMessage // nil when the initial request carried no body
	headers map[string]string
}

// snapshotMerge freezes req's body and headers for later merges.
func snapshotMerge(req *Request) (*mergeSnapshot, error) {
	snap := &mergeSnapshot{headers: cloneHeaders(req.Headers)}
	if req.Body != nil {
		b, err := json.Marshal(req.Body)
		if err != nil {
			return nil, fmt.Errorf("stac: encode initial request body: %w", err)
		}
		snap.body = b
	}
	return snap, nil
}

// applyMerge overlays next's body and headers onto the snapshot of the
// initial request and returns a fresh Request. The method and URL come from
// the next link itself; the snapshot is never mutated, so repeated merges
// cannot accumulate tokens or temporary headers from earlier pages.
func (s *mergeSnapshot) applyMerge(next *Request) (*Request, error) {
	body, err := mergeJSONBodies(s.body, next.Body)
	if err != nil {
		return nil, err
	}
	return &Request{
		Method:  next.Method,
		URL:     next.URL,
		Body:    body,
		Headers: mergeHeaders(s.headers, next.Headers),
	}, nil
}

// mergeJSONBodies overlays the next-link body's top-level fields onto the
// initial request body.
//
// Merge rules follow the STAC Item Search next-link convention:
//   - merging is top-level only: same-named fields are replaced wholesale,
//     arrays are not concatenated, nested CQL expressions are not recursed
//     into;
//   - an explicit JSON null value is preserved as null;
//   - a missing link body is treated as an empty object;
//   - an explicit top-level array or null is not a mergeable object and
//     produces an error instead of silently replacing the request.
//
// It returns nil (no body) only when neither side contributes fields.
func mergeJSONBodies(initial json.RawMessage, nextBody any) (map[string]any, error) {
	base := map[string]any{}
	if len(initial) > 0 {
		if err := json.Unmarshal(initial, &base); err != nil {
			return nil, fmt.Errorf("stac: pagination merge: initial request body is not a JSON object: %w", err)
		}
		if base == nil { // initial body decoded to explicit JSON null
			return nil, errors.New("stac: pagination merge: initial request body is null, not a JSON object")
		}
	}

	if nextBody == nil {
		if len(base) == 0 {
			return nil, nil
		}
		return base, nil
	}
	raw, err := json.Marshal(nextBody)
	if err != nil {
		return nil, fmt.Errorf("stac: pagination merge: encode next-link body: %w", err)
	}
	var overlay map[string]any
	if err := json.Unmarshal(raw, &overlay); err != nil {
		return nil, fmt.Errorf("stac: pagination merge: next-link body is not a JSON object: %w", err)
	}
	if overlay == nil { // link body decoded to explicit JSON null
		return nil, errors.New("stac: pagination merge: next-link body is null, not a JSON object")
	}
	for k, v := range overlay {
		base[k] = v
	}
	if len(base) == 0 {
		return nil, nil
	}
	return base, nil
}

// cloneHeaders returns a detached copy of h.
func cloneHeaders(h map[string]string) map[string]string {
	if len(h) == 0 {
		return nil
	}
	out := make(map[string]string, len(h))
	for k, v := range h {
		out[k] = v
	}
	return out
}

// mergeHeaders overlays next on top of base using HTTP header name matching
// (case-insensitive). A next-link header replaces any base header with the
// same name, taking the link's spelling of the name; unrelated base headers
// survive. A fresh map is always returned.
func mergeHeaders(base, next map[string]string) map[string]string {
	if len(base) == 0 && len(next) == 0 {
		return nil
	}
	out := make(map[string]string, len(base)+len(next))
	lower := make(map[string]string, len(base)+len(next))
	for k, v := range base {
		out[k] = v
		lower[strings.ToLower(k)] = k
	}
	for k, v := range next {
		lk := strings.ToLower(k)
		if existing, ok := lower[lk]; ok {
			delete(out, existing)
		}
		out[k] = v
		lower[lk] = k
	}
	return out
}
