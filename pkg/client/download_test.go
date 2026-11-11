package client

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestDownloadAsset_HTTP(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Length", "11")
		_, _ = w.Write([]byte("hello world"))
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL)

	dir := t.TempDir()
	dest := filepath.Join(dir, "out.bin")
	var seenTotal int64
	err := c.DownloadAssetWithProgress(context.Background(), srv.URL+"/x", dest, func(_, total int64) {
		if total > 0 {
			seenTotal = total
		}
	})
	require.NoError(t, err)
	b, err := os.ReadFile(dest)
	require.NoError(t, err)
	assert.Equal(t, "hello world", string(b))
	assert.Equal(t, int64(11), seenTotal)
}

func TestDownloadAsset_RemovesPartialOnError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
		_, _ = w.Write([]byte("boom"))
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL)

	dir := t.TempDir()
	dest := filepath.Join(dir, "out.bin")
	err := c.DownloadAssetWithProgress(context.Background(), srv.URL+"/x", dest, nil)
	require.Error(t, err)
	_, statErr := os.Stat(dest)
	assert.True(t, os.IsNotExist(statErr), "partial file must be removed on error")
}
