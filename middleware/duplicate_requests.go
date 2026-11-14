package middleware

import (
	"github.com/geziyor/geziyor/client"
	"github.com/geziyor/geziyor/internal"
	"sync"
)

// DuplicateRequests checks for already visited URLs
type DuplicateRequests struct {
	RevisitEnabled bool
	visitedURLs    sync.Map
	logOnlyOnce    sync.Map
}

// visitedKey scopes visited URLs to the source of the request.
// Requests submitted through different source handles (or rebuilt handles
// with the same name) never deduplicate against each other, while requests
// without a source keep sharing the default scope as before.
type visitedKey struct {
	source interface{}
	url    string
}

func (a *DuplicateRequests) ProcessRequest(r *client.Request) {
	if !a.RevisitEnabled && r.Request.Method == "GET" {
		key := visitedKey{source: r.Source, url: r.Request.URL.String()}
		if _, visited := a.visitedURLs.LoadOrStore(key, struct{}{}); visited {
			if _, logged := a.logOnlyOnce.LoadOrStore(key, struct{}{}); !logged {
				internal.Logger.Printf("URL already visited %s\n", key.url)
			}
			r.Cancel()
		}
	}
}
