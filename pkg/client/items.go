package client

import (
	"context"
	"encoding/json"
	"fmt"
	"iter"
	"net/url"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
)

// GetItem fetches an individual item from a collection.
//
// Returns a *HTTPError satisfying errors.Is(err, ErrNotFound) when the item
// does not exist.
func (c *Client) GetItem(ctx context.Context, collectionID, itemID string) (*stac.Item, error) {
	if collectionID == "" {
		return nil, fmt.Errorf("stac: collection ID cannot be empty")
	}
	if itemID == "" {
		return nil, fmt.Errorf("stac: item ID cannot be empty")
	}

	path := fmt.Sprintf("collections/%s/items/%s",
		url.PathEscape(collectionID), url.PathEscape(itemID))
	return doJSON[stac.Item](ctx, c, Get(path))
}

// GetItems returns an iterator over all items in a collection.
func (c *Client) GetItems(ctx context.Context, collectionID string) iter.Seq2[*stac.Item, error] {
	if collectionID == "" {
		return errorIter[stac.Item](fmt.Errorf("stac: collection ID cannot be empty"))
	}
	path := fmt.Sprintf("collections/%s/items", url.PathEscape(collectionID))
	return Iterate(ctx, c, Get(path), ItemDecoder())
}

// GetItemsPages returns an iterator over pages of items in a collection.
func (c *Client) GetItemsPages(ctx context.Context, collectionID string) iter.Seq2[*PageResult[stac.Item], error] {
	if collectionID == "" {
		return errorIterPage[stac.Item](fmt.Errorf("stac: collection ID cannot be empty"))
	}
	path := fmt.Sprintf("collections/%s/items", url.PathEscape(collectionID))
	return IteratePages(ctx, c, Get(path), ItemDecoder())
}

// doJSON dispatches a request, validates the response, applies the body
// limit, and decodes JSON into *T.
func doJSON[T any](ctx context.Context, c *Client, req *Request) (*T, error) {
	resp, err := c.Do(ctx, req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if err := checkStatus(resp); err != nil {
		return nil, err
	}
	if err := checkJSONContentType(resp); err != nil {
		return nil, err
	}
	var out T
	dec := json.NewDecoder(limitBody(resp, c.maxBodyBytes))
	if err := dec.Decode(&out); err != nil {
		var lerr *limitedReadError
		if isLimitErr(err, &lerr) {
			return nil, ErrResponseTooLarge
		}
		return nil, fmt.Errorf("stac: decode error: %w", err)
	}
	return &out, nil
}

// errorIter returns an iterator that yields a single error.
func errorIter[V any](err error) iter.Seq2[*V, error] {
	return func(yield func(*V, error) bool) { yield(nil, err) }
}

// errorIterPage returns an iterator that yields a single error for page iteration.
func errorIterPage[V any](err error) iter.Seq2[*PageResult[V], error] {
	return func(yield func(*PageResult[V], error) bool) { yield(nil, err) }
}
