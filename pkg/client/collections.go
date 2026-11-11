package client

import (
	"context"
	"errors"
	"fmt"
	"iter"
	"net/url"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

// GetCollection fetches a single collection document by ID.
//
// Returns a *HTTPError satisfying errors.Is(err, ErrNotFound) when the
// collection does not exist.
func (c *Client) GetCollection(ctx context.Context, collectionID string) (*stac.Collection, error) {
	if collectionID == "" {
		return nil, fmt.Errorf("stac: collection ID cannot be empty")
	}
	path := fmt.Sprintf("collections/%s", url.PathEscape(collectionID))
	return doJSON[stac.Collection](ctx, c, Get(path))
}

// GetCollections returns an iterator over all collections exposed by the
// STAC API.
func (c *Client) GetCollections(ctx context.Context) iter.Seq2[*stac.Collection, error] {
	return Iterate(ctx, c, Get("collections"), CollectionDecoder())
}

// GetCollectionsPages returns an iterator over pages of collections.
func (c *Client) GetCollectionsPages(ctx context.Context) iter.Seq2[*PageResult[stac.Collection], error] {
	return IteratePages(ctx, c, Get("collections"), CollectionDecoder())
}

// GetQueryables fetches the queryable properties for a collection.
//
// The endpoint is /collections/{collectionId}/queryables per OGC API Features
// Part 3. Returns a *HTTPError satisfying errors.Is(err, ErrNotFound) when
// the collection does not expose queryables.
func (c *Client) GetQueryables(ctx context.Context, collectionID string) (*stac.Queryables, error) {
	if collectionID == "" {
		return nil, fmt.Errorf("stac: collection ID cannot be empty")
	}
	path := fmt.Sprintf("collections/%s/queryables", url.PathEscape(collectionID))
	return doJSON[stac.Queryables](ctx, c, Get(path))
}

// GetGlobalQueryables fetches the global queryable properties for the STAC API.
//
// The endpoint is /queryables per OGC API Features Part 3. Returns a
// *HTTPError satisfying errors.Is(err, ErrNotFound) when the API does not
// expose this endpoint.
func (c *Client) GetGlobalQueryables(ctx context.Context) (*stac.Queryables, error) {
	return doJSON[stac.Queryables](ctx, c, Get("queryables"))
}

// isLimitErr is a small helper used by doJSON to detect the limited-body
// sentinel.
func isLimitErr(err error, target **limitedReadError) bool {
	return err != nil && errors.As(err, target)
}
