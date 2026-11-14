package geziyor_test

import (
	"fmt"
	"github.com/geziyor/geziyor"
	"github.com/geziyor/geziyor/client"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
)

func TestBaselineNormal(t *testing.T) {
	t.Run("normal", func(t *testing.T) {
		var pages int32
		s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&pages, 1)
			fmt.Fprint(w, "catalog")
		}))
		defer s.Close()
		callbacks := 0
		geziyor.NewGeziyor(&geziyor.Options{StartURLs: []string{s.URL}, RobotsTxtDisabled: true, LogDisabled: true,
			ParseFunc: func(_ *geziyor.Geziyor, r *client.Response) {
				callbacks++
				if string(r.Body) != "catalog" {
					t.Error("wrong body")
				}
			},
		}).Start()
		if pages != 1 || callbacks != 1 {
			t.Fatalf("pages=%d callbacks=%d", pages, callbacks)
		}
		fmt.Printf("BASE_NORMAL pages=%d callbacks=%d\n", pages, callbacks)
	})
}

func TestBaselineLegacyDedupe(t *testing.T) {
	var pages int32
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		atomic.AddInt32(&pages, 1)
		fmt.Fprint(w, "same page")
	}))
	defer s.Close()
	callbacks := 0
	geziyor.NewGeziyor(&geziyor.Options{RobotsTxtDisabled: true, LogDisabled: true,
		StartRequestsFunc: func(g *geziyor.Geziyor) { g.Get(s.URL, nil); g.Get(s.URL, nil) },
		ParseFunc:         func(g *geziyor.Geziyor, r *client.Response) { callbacks++ },
	}).Start()
	if pages != 1 || callbacks != 1 {
		t.Fatalf("pages=%d callbacks=%d", pages, callbacks)
	}
	fmt.Println("BASE_LEGACY_DEDUPE submissions=2 pages=1 callbacks=1")
}
