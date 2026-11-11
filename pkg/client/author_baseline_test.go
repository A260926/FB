package client

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"reflect"
	"testing"
)

func TestAuthorPostMergeBaseline(t *testing.T) {
	var bodies []map[string]any
	var srv *httptest.Server
	srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var b map[string]any
		if err := json.NewDecoder(r.Body).Decode(&b); err != nil {
			t.Error(err)
		}
		bodies = append(bodies, b)
		w.Header().Set("Content-Type", "application/geo+json")
		if len(bodies) == 1 {
			fmt.Fprintf(w, `{"type":"FeatureCollection","features":[{"type":"Feature","id":"first"}],"links":[{"rel":"next","href":%q,"method":"POST","merge":true,"body":{"token":"t2"}}]}`, srv.URL+"/search")
			return
		}
		id := "matched"
		if b["collections"] == nil || b["filter"] == nil {
			id = "unfiltered"
		}
		fmt.Fprintf(w, `{"type":"FeatureCollection","features":[{"type":"Feature","id":%q}],"links":[]}`, id)
	}))
	defer srv.Close()
	c, err := NewClient(srv.URL)
	if err != nil {
		t.Fatal(err)
	}
	params := SearchParams{Collections: []string{"sentinel-2"}, FilterLang: "cql2-json", Filter: map[string]any{"op": "=", "args": []any{map[string]any{"property": "platform"}, "sentinel-2a"}}}
	var ids []string
	for it, e := range c.Search(context.Background(), params) {
		if e != nil {
			t.Fatal(e)
		}
		ids = append(ids, it.ID)
	}
	if !reflect.DeepEqual(ids, []string{"first", "unfiltered"}) || len(bodies) != 2 || bodies[0]["collections"] == nil || bodies[1]["collections"] != nil {
		t.Fatalf("ids=%v bodies=%v", ids, bodies)
	}
	t.Log("BASELINE_STAC_POST_MERGE_DROPS_COLLECTION_AND_FILTER")
}
