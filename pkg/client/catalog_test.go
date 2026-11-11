package client

import (
	"context"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestClient_GetCatalog(t *testing.T) {
	catalogJSON := `{
		"type": "Catalog",
		"stac_version": "1.0.0",
		"id": "test-catalog",
		"title": "Test Catalog",
		"description": "A test STAC catalog",
		"conformsTo": [
			"https://api.stacspec.org/v1.0.0/core",
			"https://api.stacspec.org/v1.0.0/collections",
			"https://api.stacspec.org/v1.0.0/item-search",
			"http://www.opengis.net/spec/cql2/1.0/conf/cql2-json"
		],
		"links": [
			{"rel": "self", "href": "https://example.com/stac"},
			{"rel": "root", "href": "https://example.com/stac"},
			{"rel": "collections", "href": "https://example.com/stac/collections"},
			{"rel": "search", "href": "https://example.com/stac/search", "method": "POST"}
		],
		"custom_field": "custom_value"
	}`

	t.Run("success", func(t *testing.T) {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			assert.Equal(t, "/", r.URL.Path)
			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte(catalogJSON))
		}))
		defer server.Close()

		client, err := NewClient(server.URL)
		require.NoError(t, err)

		cat, err := client.GetCatalog(context.Background())
		require.NoError(t, err)

		assert.Equal(t, "1.0.0", cat.Version)
		assert.Equal(t, "test-catalog", cat.ID)
		assert.Equal(t, "Test Catalog", cat.Title)
		assert.Equal(t, "A test STAC catalog", cat.Description)
		assert.Len(t, cat.ConformsTo, 4)
		assert.Len(t, cat.Links, 4)

		// Check foreign members
		assert.Equal(t, "custom_value", cat.AdditionalFields["custom_field"])
	})

	t.Run("not found", func(t *testing.T) {
		server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusNotFound)
		}))
		defer server.Close()

		client, err := NewClient(server.URL)
		require.NoError(t, err)

		_, err = client.GetCatalog(context.Background())
		assert.Error(t, err)
		assert.Contains(t, err.Error(), "404")
	})
}

func TestClient_GetConformance(t *testing.T) {
	catalogJSON := `{
		"type": "Catalog",
		"stac_version": "1.0.0",
		"id": "test-catalog",
		"description": "A test catalog",
		"conformsTo": [
			"https://api.stacspec.org/v1.0.0/core",
			"https://api.stacspec.org/v1.0.0/item-search"
		],
		"links": []
	}`

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(catalogJSON))
	}))
	defer server.Close()

	client, err := NewClient(server.URL)
	require.NoError(t, err)

	conformance, err := client.GetConformance(context.Background())
	require.NoError(t, err)

	assert.Len(t, conformance, 2)
	assert.Contains(t, conformance, stac.ConformanceCore)
	assert.Contains(t, conformance, stac.ConformanceItemSearch)
}

func TestClient_SupportsConformance(t *testing.T) {
	catalogJSON := `{
		"type": "Catalog",
		"stac_version": "1.0.0",
		"id": "test-catalog",
		"description": "A test catalog",
		"conformsTo": [
			"https://api.stacspec.org/v1.0.0/core",
			"http://www.opengis.net/spec/cql2/1.0/conf/cql2-json"
		],
		"links": []
	}`

	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(catalogJSON))
	}))
	defer server.Close()

	client, err := NewClient(server.URL)
	require.NoError(t, err)

	t.Run("supports core", func(t *testing.T) {
		supported, err := client.SupportsConformance(context.Background(), stac.ConformanceCore)
		require.NoError(t, err)
		assert.True(t, supported)
	})

	t.Run("supports cql2-json", func(t *testing.T) {
		supported, err := client.SupportsConformance(context.Background(), stac.ConformanceCQL2JSON)
		require.NoError(t, err)
		assert.True(t, supported)
	})

	t.Run("does not support collections", func(t *testing.T) {
		supported, err := client.SupportsConformance(context.Background(), stac.ConformanceCollections)
		require.NoError(t, err)
		assert.False(t, supported)
	})
}

func TestCatalog_GetLink(t *testing.T) {
	cat := &stac.Catalog{
		Links: []*stac.Link{
			{Rel: "self", Href: "https://example.com/stac"},
			{Rel: "search", Href: "https://example.com/stac/search"},
			{Rel: "collections", Href: "https://example.com/stac/collections"},
		},
	}

	t.Run("found", func(t *testing.T) {
		link := cat.GetLink("search")
		require.NotNil(t, link)
		assert.Equal(t, "https://example.com/stac/search", link.Href)
	})

	t.Run("not found", func(t *testing.T) {
		link := cat.GetLink("nonexistent")
		assert.Nil(t, link)
	})
}

func TestCatalog_GetLinks(t *testing.T) {
	cat := &stac.Catalog{
		Links: []*stac.Link{
			{Rel: "child", Href: "https://example.com/stac/child1"},
			{Rel: "self", Href: "https://example.com/stac"},
			{Rel: "child", Href: "https://example.com/stac/child2"},
			{Rel: "child", Href: "https://example.com/stac/child3"},
		},
	}

	links := cat.GetLinks("child")
	assert.Len(t, links, 3)
}

func TestCatalog_HasConformance(t *testing.T) {
	cat := &stac.Catalog{
		ConformsTo: []string{
			stac.ConformanceCore,
			stac.ConformanceItemSearch,
			stac.ConformanceCQL2JSON,
		},
	}

	assert.True(t, cat.HasConformance(stac.ConformanceCore))
	assert.True(t, cat.HasConformance(stac.ConformanceCQL2JSON))
	assert.False(t, cat.HasConformance(stac.ConformanceCollections))
}
