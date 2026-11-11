package client

import (
	"testing"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
	"github.com/stretchr/testify/assert"
)

func TestLinkToRequest(t *testing.T) {
	t.Run("nil link returns nil", func(t *testing.T) {
		req := LinkToRequest(nil)
		assert.Nil(t, req)
	})

	t.Run("simple link with only href", func(t *testing.T) {
		link := &stac.Link{
			Href: "https://example.com/next",
			Rel:  "next",
		}
		req := LinkToRequest(link)
		assert.Equal(t, "GET", req.Method)
		assert.Equal(t, "https://example.com/next", req.URL)
		assert.Nil(t, req.Body)
		assert.Nil(t, req.Headers)
	})

	t.Run("link with method in AdditionalFields", func(t *testing.T) {
		link := &stac.Link{
			Href: "https://example.com/search",
			Rel:  "next",
			AdditionalFields: map[string]any{
				"method": "POST",
			},
		}
		req := LinkToRequest(link)
		assert.Equal(t, "POST", req.Method)
		assert.Equal(t, "https://example.com/search", req.URL)
	})

	t.Run("link with body in AdditionalFields", func(t *testing.T) {
		body := map[string]any{
			"collections": []string{"sentinel-2"},
			"limit":       10,
		}
		link := &stac.Link{
			Href: "https://example.com/search",
			Rel:  "next",
			AdditionalFields: map[string]any{
				"method": "POST",
				"body":   body,
			},
		}
		req := LinkToRequest(link)
		assert.Equal(t, "POST", req.Method)
		assert.Equal(t, body, req.Body)
	})

	t.Run("link with headers in AdditionalFields", func(t *testing.T) {
		link := &stac.Link{
			Href: "https://example.com/next",
			Rel:  "next",
			AdditionalFields: map[string]any{
				"headers": map[string]any{
					"X-Custom-Header": "value",
					"Authorization":   "Bearer token",
				},
			},
		}
		req := LinkToRequest(link)
		assert.Equal(t, "GET", req.Method)
		assert.Equal(t, "value", req.Headers["X-Custom-Header"])
		assert.Equal(t, "Bearer token", req.Headers["Authorization"])
	})

	t.Run("link with all fields", func(t *testing.T) {
		body := map[string]any{"page": 2}
		link := &stac.Link{
			Href: "https://example.com/search",
			Rel:  "next",
			AdditionalFields: map[string]any{
				"method": "POST",
				"body":   body,
				"headers": map[string]any{
					"X-Token": "abc123",
				},
			},
		}
		req := LinkToRequest(link)
		assert.Equal(t, "POST", req.Method)
		assert.Equal(t, "https://example.com/search", req.URL)
		assert.Equal(t, body, req.Body)
		assert.Equal(t, "abc123", req.Headers["X-Token"])
	})

	t.Run("non-string header values are ignored", func(t *testing.T) {
		link := &stac.Link{
			Href: "https://example.com/next",
			Rel:  "next",
			AdditionalFields: map[string]any{
				"headers": map[string]any{
					"Valid":   "string-value",
					"Invalid": 123, // non-string, should be ignored
				},
			},
		}
		req := LinkToRequest(link)
		assert.Equal(t, "string-value", req.Headers["Valid"])
		_, hasInvalid := req.Headers["Invalid"]
		assert.False(t, hasInvalid)
	})
}

func TestGet(t *testing.T) {
	req := Get("collections")
	assert.Equal(t, "", req.Method) // defaults to GET when executed
	assert.Equal(t, "collections", req.URL)
	assert.Nil(t, req.Body)
}

func TestPost(t *testing.T) {
	body := map[string]any{"collections": []string{"test"}}
	req := Post("search", body)
	assert.Equal(t, "POST", req.Method)
	assert.Equal(t, "search", req.URL)
	assert.Equal(t, body, req.Body)
}
