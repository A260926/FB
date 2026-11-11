package client

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestClient_SearchSimple(t *testing.T) {
	var requestLog []struct {
		Method string
		URL    string
	}

	var server *httptest.Server
	server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requestLog = append(requestLog, struct {
			Method string
			URL    string
		}{r.Method, r.URL.String()})

		if r.Method != http.MethodGet {
			http.Error(w, "unexpected method", http.StatusMethodNotAllowed)
			return
		}

		switch r.URL.Query().Get("page") {
		case "", "1":
			assert.Equal(t, "sentinel-2", r.URL.Query().Get("collections"))
			assert.Equal(t, "1,2,3,4,5,6", r.URL.Query().Get("bbox"))
			assert.Equal(t, "-acquired", r.URL.Query().Get("sortby"))
			require.NotEmpty(t, r.URL.Query().Get("query"))
			require.NotEmpty(t, r.URL.Query().Get("fields"))

			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte(`{
                "type": "FeatureCollection",
                "features": [{"type":"Feature","id":"item-1","properties":{},"geometry":null,"assets":{},"links":[]}],
                "links": [{"rel":"next","href":"/search?page=2"}]
            }`))
		case "2":
			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte(`{
                "type": "FeatureCollection",
                "features": [{"type":"Feature","id":"item-2","properties":{},"geometry":null,"assets":{},"links":[]}],
                "links": []
            }`))
		default:
			http.Error(w, "unexpected page", http.StatusNotFound)
		}
	}))
	defer server.Close()

	cli, err := NewClient(server.URL)
	require.NoError(t, err)

	seq := cli.SearchSimple(context.Background(), SearchParams{
		Collections: []string{"sentinel-2"},
		Bbox:        []float64{1, 2, 3, 4, 5, 6},
		Datetime:    "2020-01-01/2020-01-02",
		Limit:       100,
		SortBy:      []SortField{{Field: "acquired", Direction: SortDesc}},
		Query: map[string]any{
			"eo:cloud_cover": map[string]any{"lt": 10},
		},
		Fields: &FieldsFilter{Include: []string{"id"}, Exclude: []string{"geometry"}},
	})

	items, err := collect(seq)
	require.NoError(t, err)
	require.Len(t, items, 2)
	assert.Equal(t, "item-1", items[0].ID)
	assert.Equal(t, "item-2", items[1].ID)
	assert.Len(t, requestLog, 2)
}

func TestClient_SearchCQL2(t *testing.T) {
	var (
		hitCount int
		server   *httptest.Server
	)
	server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hitCount++

		switch hitCount {
		case 1:
			assert.Equal(t, http.MethodPost, r.Method)
			var payload SearchParams
			require.NoError(t, json.NewDecoder(r.Body).Decode(&payload))
			assert.Equal(t, []string{"SENTINEL-1"}, payload.Collections)

			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte(fmt.Sprintf(`{
                "type": "FeatureCollection",
                "features": [{"type":"Feature","id":"page-1","properties":{},"geometry":null,"assets":{},"links":[]}],
                "links": [{"rel":"next","href":"%s/search?page=2"}]
            }`, server.URL)))
		case 2:
			assert.Equal(t, http.MethodGet, r.Method)
			assert.Equal(t, "2", r.URL.Query().Get("page"))

			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte(`{
                "type": "FeatureCollection",
                "features": [{"type":"Feature","id":"page-2","properties":{},"geometry":null,"assets":{},"links":[]}],
                "links": []
            }`))
		default:
			http.Error(w, "unexpected request", http.StatusTeapot)
		}
	}))
	defer server.Close()

	cli, err := NewClient(server.URL, WithTimeout(30*time.Second))
	require.NoError(t, err)

	seq := cli.Search(context.Background(), SearchParams{Collections: []string{"SENTINEL-1"}})
	items, err := collect(seq)
	require.NoError(t, err)
	require.Len(t, items, 2)
	assert.Equal(t, "page-1", items[0].ID)
	assert.Equal(t, "page-2", items[1].ID)
	assert.Equal(t, 2, hitCount)
}

func TestClient_SearchCQL2_Error(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusBadRequest)
		json.NewEncoder(w).Encode(APIError{Code: 400, Description: "bad request", Type: "InvalidQuery"})
	}))
	defer server.Close()

	cli, err := NewClient(server.URL)
	require.NoError(t, err)

	seq := cli.Search(context.Background(), SearchParams{Collections: []string{"SENTINEL-2"}})
	items, err := collect(seq)
	require.Error(t, err)
	assert.Nil(t, items)
	assert.Contains(t, err.Error(), "bad request")
}
