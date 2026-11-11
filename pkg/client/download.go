package client

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/url"
	"os"
	"sync"
)

// ProgressFunc reports cumulative bytes downloaded and the expected total.
// total is -1 when the upstream Content-Length is unknown.
type ProgressFunc func(downloaded, total int64)

// SchemeDownloader downloads a resource identified by a parsed URL with a
// resolved scheme other than http/https. Register implementations via
// RegisterDownloader so that DownloadAsset can dispatch to them — typical
// uses are s3:// and gs://.
type SchemeDownloader func(ctx context.Context, u *url.URL, dest io.Writer, progress ProgressFunc) error

var (
	downloaderMu  sync.RWMutex
	downloaderReg = map[string]SchemeDownloader{}
)

// RegisterDownloader registers handler for the given URL scheme (lower-cased).
// Re-registering replaces any previous handler.
func RegisterDownloader(scheme string, handler SchemeDownloader) {
	if scheme == "" || handler == nil {
		return
	}
	downloaderMu.Lock()
	defer downloaderMu.Unlock()
	downloaderReg[scheme] = handler
}

func lookupDownloader(scheme string) SchemeDownloader {
	downloaderMu.RLock()
	defer downloaderMu.RUnlock()
	return downloaderReg[scheme]
}

// DownloadAsset retrieves the asset at assetURL and writes it to destPath.
func (c *Client) DownloadAsset(ctx context.Context, assetURL, destPath string) error {
	return c.DownloadAssetWithProgress(ctx, assetURL, destPath, nil)
}

// DownloadAssetWithProgress downloads an asset while reporting progress.
//
// The asset URL may be relative (resolved against the client's base URL) or
// absolute. http and https schemes go through the client's HTTP transport and
// middleware chain; other schemes are dispatched to a downloader registered
// via RegisterDownloader. If no downloader is registered, an error is returned.
func (c *Client) DownloadAssetWithProgress(
	ctx context.Context,
	assetURL string,
	destPath string,
	progress ProgressFunc,
) (retErr error) {
	if c == nil {
		return fmt.Errorf("stac: client is nil")
	}

	u, err := url.Parse(assetURL)
	if err != nil {
		return fmt.Errorf("stac: parse asset URL: %w", err)
	}
	if u.Scheme == "" {
		u = c.baseURL.ResolveReference(u)
	}

	out, err := os.Create(destPath)
	if err != nil {
		return fmt.Errorf("stac: create destination file: %w", err)
	}
	defer func() {
		cerr := out.Close()
		if retErr == nil {
			retErr = cerr
		}
		if retErr != nil {
			_ = os.Remove(destPath)
		}
	}()

	switch u.Scheme {
	case "http", "https":
		return c.downloadHTTP(ctx, u.String(), out, progress)
	default:
		dl := lookupDownloader(u.Scheme)
		if dl == nil {
			return fmt.Errorf("stac: no downloader registered for scheme %q (import a downloader package, e.g. pkg/client/s3, to enable it)", u.Scheme)
		}
		return dl(ctx, u, out, progress)
	}
}

func (c *Client) downloadHTTP(ctx context.Context, assetURL string, dst io.Writer, progress ProgressFunc) error {
	resp, err := c.Do(ctx, &Request{URL: assetURL})
	if err != nil {
		return fmt.Errorf("stac: download asset: %w", err)
	}
	defer resp.Body.Close()

	if err := checkStatus(resp); err != nil {
		return err
	}

	total := resp.ContentLength
	if progress != nil {
		progress(0, total)
	}
	_, err = copyWithProgress(ctx, dst, resp.Body, total, progress)
	if err != nil {
		return fmt.Errorf("stac: write asset: %w", err)
	}
	return nil
}

// CopyWithProgress is exported so registered SchemeDownloaders can reuse the
// same context-aware progress-reporting copy loop.
func CopyWithProgress(ctx context.Context, dst io.Writer, src io.Reader, total int64, progress ProgressFunc) (int64, error) {
	return copyWithProgress(ctx, dst, src, total, progress)
}

func copyWithProgress(ctx context.Context, dst io.Writer, src io.Reader, total int64, progress ProgressFunc) (int64, error) {
	const bufSize = 32 * 1024
	buf := make([]byte, bufSize)
	var written int64
	for {
		if ctx != nil {
			if err := ctx.Err(); err != nil {
				return written, err
			}
		}
		n, readErr := src.Read(buf)
		if n > 0 {
			w, writeErr := dst.Write(buf[:n])
			if writeErr != nil {
				return written, writeErr
			}
			if w != n {
				return written, io.ErrShortWrite
			}
			written += int64(w)
			if progress != nil {
				progress(written, total)
			}
		}
		if readErr != nil {
			if errors.Is(readErr, io.EOF) {
				return written, nil
			}
			return written, readErr
		}
	}
}
