// Package s3 registers an s3:// asset downloader for the STAC client.
//
// Importing this package (typically with a blank identifier) wires the
// downloader into the parent client package's registry:
//
//	import _ "github.com/robert-malhotra/go-stac-client/pkg/client/s3"
//
// Once imported, Client.DownloadAsset accepts s3://bucket/key URLs and
// resolves credentials via the AWS SDK default chain (env, shared config,
// IMDS, etc.). To use a custom *s3.Client, call Register with it instead of
// (or after) the side-effect import.
package s3

import (
	"context"
	"fmt"
	"io"
	"net/url"
	"strings"

	"github.com/aws/aws-sdk-go-v2/config"
	awss3 "github.com/aws/aws-sdk-go-v2/service/s3"

	"github.com/robert-malhotra/go-stac-client/pkg/client"
)

func init() {
	client.RegisterDownloader("s3", defaultDownloader)
}

// Register installs a custom downloader backed by the given *s3.Client. Pass
// nil to fall back to the default credential chain.
func Register(c *awss3.Client) {
	if c == nil {
		client.RegisterDownloader("s3", defaultDownloader)
		return
	}
	client.RegisterDownloader("s3", func(ctx context.Context, u *url.URL, dst io.Writer, progress client.ProgressFunc) error {
		return download(ctx, c, u, dst, progress)
	})
}

func defaultDownloader(ctx context.Context, u *url.URL, dst io.Writer, progress client.ProgressFunc) error {
	cfg, err := config.LoadDefaultConfig(ctx)
	if err != nil {
		return fmt.Errorf("stac/s3: load AWS config: %w", err)
	}
	c := awss3.NewFromConfig(cfg)
	return download(ctx, c, u, dst, progress)
}

func download(ctx context.Context, c *awss3.Client, u *url.URL, dst io.Writer, progress client.ProgressFunc) error {
	bucket := u.Host
	key := strings.TrimPrefix(u.Path, "/")
	if bucket == "" || key == "" {
		return fmt.Errorf("stac/s3: invalid s3 URL %q (need s3://bucket/key)", u.String())
	}
	out, err := c.GetObject(ctx, &awss3.GetObjectInput{Bucket: &bucket, Key: &key})
	if err != nil {
		return fmt.Errorf("stac/s3: get object: %w", err)
	}
	defer out.Body.Close()

	var total int64
	if out.ContentLength != nil {
		total = *out.ContentLength
	}
	if progress != nil {
		progress(0, total)
	}
	if _, err := client.CopyWithProgress(ctx, dst, out.Body, total, progress); err != nil {
		return fmt.Errorf("stac/s3: copy: %w", err)
	}
	return nil
}
