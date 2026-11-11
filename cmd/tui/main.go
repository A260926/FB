package main

import (
	"context"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	// Register the s3:// asset downloader so the TUI can fetch assets
	// from S3-backed STAC catalogs.
	_ "github.com/robert-malhotra/go-stac-client/pkg/client/s3"
)

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	tui := NewTUI(ctx)
	go func() {
		<-ctx.Done()
		tui.Stop()
	}()

	if err := tui.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}
}
