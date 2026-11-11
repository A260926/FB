package client

import (
	"context"
	"errors"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

// GetCatalog fetches the root catalog document from the STAC API.
//
// The root catalog is typically the entry point for exploring a STAC API,
// containing links to collections, the search endpoint, and the conformance
// classes the API implements.
func (c *Client) GetCatalog(ctx context.Context) (*stac.Catalog, error) {
	return doJSON[stac.Catalog](ctx, c, Get(""))
}

// GetConformance fetches the conformance classes supported by the STAC API.
//
// It first checks the root catalog's conformsTo field. If that field is empty
// (or absent — common for OGC API Features–only servers), it falls back to
// fetching the dedicated /conformance endpoint. A *HTTPError satisfying
// errors.Is(err, ErrNotFound) is returned only when neither source provides
// conformance information.
func (c *Client) GetConformance(ctx context.Context) ([]string, error) {
	cat, err := c.GetCatalog(ctx)
	if err == nil && len(cat.ConformsTo) > 0 {
		return cat.ConformsTo, nil
	}

	type conformanceDoc struct {
		ConformsTo []string `json:"conformsTo"`
	}
	doc, ferr := doJSON[conformanceDoc](ctx, c, Get("conformance"))
	if ferr == nil && len(doc.ConformsTo) > 0 {
		return doc.ConformsTo, nil
	}
	if err == nil {
		// Root succeeded but had no conformsTo; surface the fallback error.
		if ferr != nil {
			return nil, ferr
		}
		return nil, errors.New("stac: server exposes neither root.conformsTo nor /conformance")
	}
	return nil, err
}

// SupportsConformance reports whether the STAC API declares the given
// conformance class.
//
// Use the stac.Conformance* constants for common conformance class URIs.
func (c *Client) SupportsConformance(ctx context.Context, conformanceClass string) (bool, error) {
	classes, err := c.GetConformance(ctx)
	if err != nil {
		return false, err
	}
	for _, c := range classes {
		if c == conformanceClass {
			return true, nil
		}
	}
	return false, nil
}
