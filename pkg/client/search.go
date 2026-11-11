package client

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"iter"
	"net/url"
	"strconv"
	"strings"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

// SortDirection is "asc" or "desc".
type SortDirection string

const (
	SortAsc  SortDirection = "asc"
	SortDesc SortDirection = "desc"
)

// SearchParams contains the parameters for a STAC API /search request.
//
// All fields are optional. Filter accepts any value that JSON-marshals into
// a CQL2 expression — typically the raw JSON output of the
// github.com/exergy-dev/go-cql2/json encoder, a json.RawMessage, or a
// stac-aware CQL2 AST type. Use FilterLang to identify the encoding
// ("cql2-json" or "cql2-text").
type SearchParams struct {
	Collections []string       `json:"collections,omitempty"`
	IDs         []string       `json:"ids,omitempty"`
	Bbox        []float64      `json:"bbox,omitempty"`
	Intersects  any            `json:"intersects,omitempty"`
	Datetime    string         `json:"datetime,omitempty"`
	Query       map[string]any `json:"query,omitempty"`
	Limit       int            `json:"limit,omitempty"`
	SortBy      []SortField    `json:"sortby,omitempty"`
	Fields      *FieldsFilter  `json:"fields,omitempty"`
	Filter      any            `json:"filter,omitempty"`
	FilterLang  string         `json:"filter-lang,omitempty"`
	FilterCRS   string         `json:"filter-crs,omitempty"`
}

// SortField specifies a field and direction for sorting.
type SortField struct {
	Field     string        `json:"field"`
	Direction SortDirection `json:"direction"`
}

// FieldsFilter specifies which fields to include or exclude from items.
type FieldsFilter struct {
	Include []string `json:"include,omitempty"`
	Exclude []string `json:"exclude,omitempty"`
}

// Search performs a POST-based STAC search and returns an iterator over
// matching items. Pagination follows next-links honoring method/body
// foreign members on the link.
func (c *Client) Search(ctx context.Context, params SearchParams) iter.Seq2[*stac.Item, error] {
	return Iterate(ctx, c, Post("search", params), ItemDecoder())
}

// SearchPages performs a POST-based STAC search and returns an iterator over
// raw pages, exposing numberMatched/numberReturned and the page's items.
func (c *Client) SearchPages(ctx context.Context, params SearchParams) iter.Seq2[*PageResult[stac.Item], error] {
	return IteratePages(ctx, c, Post("search", params), ItemDecoder())
}

// SearchSimple performs a GET-based STAC search using URL query parameters.
//
// SearchSimple cannot represent the Intersects parameter (use Search instead);
// passing it returns ErrUnsupportedForGET. The Filter parameter is supported
// only for FilterLang "cql2-text" (the default for GET); cql2-json filters
// require Search.
func (c *Client) SearchSimple(ctx context.Context, params SearchParams) iter.Seq2[*stac.Item, error] {
	path, err := buildSearchPath(params)
	if err != nil {
		return errorIter[stac.Item](err)
	}
	return Iterate(ctx, c, Get(path), ItemDecoder())
}

// buildSearchPath builds a search path with query parameters from SearchParams.
func buildSearchPath(params SearchParams) (string, error) {
	if params.Intersects != nil {
		return "", fmt.Errorf("%w: intersects", ErrUnsupportedForGET)
	}

	q := url.Values{}

	if len(params.Collections) > 0 {
		coll := make([]string, 0, len(params.Collections))
		for _, c := range params.Collections {
			if c == "" {
				return "", errors.New("stac: empty collection ID in SearchParams.Collections")
			}
			coll = append(coll, c)
		}
		q.Set("collections", strings.Join(coll, ","))
	}
	if len(params.IDs) > 0 {
		q.Set("ids", strings.Join(params.IDs, ","))
	}
	if len(params.Bbox) > 0 {
		if !(len(params.Bbox) == 4 || len(params.Bbox) == 6) {
			return "", fmt.Errorf("stac: bbox must have 4 or 6 elements, got %d", len(params.Bbox))
		}
		coords := make([]string, len(params.Bbox))
		for i, v := range params.Bbox {
			coords[i] = strconv.FormatFloat(v, 'f', -1, 64)
		}
		q.Set("bbox", strings.Join(coords, ","))
	}
	if params.Datetime != "" {
		q.Set("datetime", params.Datetime)
	}
	if params.Limit > 0 {
		q.Set("limit", strconv.Itoa(params.Limit))
	}
	if len(params.SortBy) > 0 {
		var parts []string
		for _, s := range params.SortBy {
			if s.Field == "" {
				return "", errors.New("stac: sortby field is empty")
			}
			sign := "+"
			switch strings.ToLower(string(s.Direction)) {
			case "", "asc":
				sign = "+"
			case "desc":
				sign = "-"
			default:
				return "", fmt.Errorf("stac: invalid sort direction %q (expected asc or desc)", s.Direction)
			}
			parts = append(parts, sign+s.Field)
		}
		q.Set("sortby", strings.Join(parts, ","))
	}
	if params.Query != nil {
		queryJSON, err := json.Marshal(params.Query)
		if err != nil {
			return "", fmt.Errorf("stac: encode query: %w", err)
		}
		q.Set("query", string(queryJSON))
	}
	if params.Fields != nil {
		fieldsJSON, err := json.Marshal(params.Fields)
		if err != nil {
			return "", fmt.Errorf("stac: encode fields: %w", err)
		}
		q.Set("fields", string(fieldsJSON))
	}
	if params.Filter != nil {
		lang := params.FilterLang
		if lang == "" {
			lang = "cql2-text"
		}
		if lang != "cql2-text" {
			return "", fmt.Errorf("%w: filter with filter-lang=%q (only cql2-text is encodable on GET)",
				ErrUnsupportedForGET, lang)
		}
		filterStr, ok := params.Filter.(string)
		if !ok {
			return "", fmt.Errorf("stac: GET filter must be a CQL2-text string, got %T", params.Filter)
		}
		q.Set("filter", filterStr)
		q.Set("filter-lang", "cql2-text")
		if params.FilterCRS != "" {
			q.Set("filter-crs", params.FilterCRS)
		}
	}

	if len(q) > 0 {
		return "search?" + q.Encode(), nil
	}
	return "search", nil
}
