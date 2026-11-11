package client

import (
	"context"
	"encoding/json"
	"iter"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/robert-malhotra/go-stac-client/pkg/stac"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// -----------------------------------------------------------------------------
// Helpers & test data
// -----------------------------------------------------------------------------

const fixtureDir = "test_resources"

// fixture reads a file from test_resources and fails the test on error.
func fixture(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(fixtureDir, name))
	require.NoErrorf(t, err, "cannot read fixture %s", name)
	return b
}

// collect pulls every item from the iterator until it terminates.

func collect(seq iter.Seq2[*stac.Item, error]) ([]*stac.Item, error) {
	var (
		out    []*stac.Item
		outErr error
	)
	seq(func(it *stac.Item, err error) bool {
		if err != nil {
			outErr = err
			return false
		}
		out = append(out, it)
		return true
	})
	return out, outErr
}

// -----------------------------------------------------------------------------
// Mock STAC Items API – enough behaviour to exercise the client
// -----------------------------------------------------------------------------

type mockServer struct {
	*httptest.Server

	item  []byte
	page1 []byte
	page2 []byte
	empty []byte
}

func newMockServer(t *testing.T) *mockServer {
	m := &mockServer{
		item:  fixture(t, "item.json"),
		page1: fixture(t, "items_page1.json"),
		page2: fixture(t, "items_page2.json"),
		empty: fixture(t, "empty_items_test.json"),
	}

	handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")

		parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/"), "/")
		// Expect shape /collections/{cid}/items[/{iid}]
		if len(parts) >= 3 && parts[0] == "collections" && parts[2] == "items" {
			cid := parts[1]

			// ------------------------------------------------------------------
			// Single item
			// ------------------------------------------------------------------
			if len(parts) == 4 { // /items/{iid}
				if parts[3] == "test-item-001" {
					w.Write(m.item)
				} else {
					http.Error(w, "item not found", http.StatusNotFound)
				}
				return
			}

			// ------------------------------------------------------------------
			// Item collections (maybe paginated)
			// ------------------------------------------------------------------
			switch cid {
			case "test-collection-paginated":
				// page param decides which fixture
				if r.URL.Query().Get("page") == "2" {
					w.Write(m.page2)
				} else {
					w.Write(m.page1)
				}
				return
			case "empty-collection":
				w.Write(m.empty)
				return
			case "error-collection":
				http.Error(w, "boom", http.StatusInternalServerError)
				return
			default:
				// Generic single‑page response.  TRY to strip next link so
				// the client doesn't loop.  If the JSON is malformed (decode‑
				// error test), just return it verbatim so the client sees the
				// bad payload.
				var il stac.ItemsList
				if err := json.Unmarshal(m.page1, &il); err == nil {
					il.Links = nil
					if b, err := json.Marshal(il); err == nil {
						w.Write(b)
						return
					}
				}
				// Fallback
				w.Write(m.page1)
				return
			}
		}

		http.NotFound(w, r)
	})

	m.Server = httptest.NewServer(handler)
	return m
}

// -----------------------------------------------------------------------------
// Tests – GetItem
// -----------------------------------------------------------------------------

func TestClient_GetItem(t *testing.T) {
	mock := newMockServer(t)
	defer mock.Close()

	cli, err := NewClient(mock.URL)
	require.NoError(t, err)

	t.Run("success", func(t *testing.T) {
		it, err := cli.GetItem(context.Background(), "dummy", "test-item-001")
		require.NoError(t, err)
		assert.Equal(t, "test-item-001", it.ID)
	})

	t.Run("not-found", func(t *testing.T) {
		_, err := cli.GetItem(context.Background(), "dummy", "missing")
		require.Error(t, err)
		assert.ErrorIs(t, err, ErrNotFound)
	})
}

// -----------------------------------------------------------------------------
// Tests – GetItems
// -----------------------------------------------------------------------------

func TestClient_GetItems(t *testing.T) {
	mock := newMockServer(t)
	defer mock.Close()

	cli, err := NewClient(mock.URL)
	require.NoError(t, err)

	t.Run("single page", func(t *testing.T) {
		items, err := collect(cli.GetItems(context.Background(), "single-page"))
		require.NoError(t, err)
		require.Len(t, items, 1)
		assert.Equal(t, "item-page1-001", items[0].ID)
	})

	t.Run("multiple pages", func(t *testing.T) {
		items, err := collect(cli.GetItems(context.Background(), "test-collection-paginated"))
		require.NoError(t, err)
		require.Len(t, items, 2)
		assert.Equal(t, "item-page1-001", items[0].ID)
		assert.Equal(t, "item-page2-001", items[1].ID)
	})

	t.Run("decode error", func(t *testing.T) {
		// corrupt fixture for this test only
		saved := mock.page1
		mock.page1 = []byte(`{"broken":`) // invalid JSON
		defer func() { mock.page1 = saved }()

		_, err := collect(cli.GetItems(context.Background(), "single-page"))
		require.Error(t, err)
		assert.Contains(t, err.Error(), "decode error")
	})

	t.Run("early stop", func(t *testing.T) {
		seq := cli.GetItems(context.Background(), "test-collection-paginated")
		var count int
		seq(func(it *stac.Item, err error) bool {
			require.NoError(t, err)
			count++
			return false // stop immediately
		})
		assert.Equal(t, 1, count)
	})

}
