#!/usr/bin/env bash
# Source from a runner after changing to the repository root.
export GOENV=off GOTOOLCHAIN=go1.26.4 GOFLAGS= GOWORK=off
export GOCACHE="$PWD/.userfb-build/go-cache" GOMODCACHE="$PWD/.userfb-build/go-mod"
export GOPROXY=https://proxy.golang.org GOSUMDB=sum.golang.org
export GOPRIVATE= GONOPROXY= GONOSUMDB=
USERFB_IMAGE=chromedp/headless-shell@sha256:d0c5b37a80f6180f28b2824d8844bf0c10bdf54ebaaf1f97514b66e5a20bef4f
mkdir -p .userfb-build
