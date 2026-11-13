#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
export GOPROXY=off GOSUMDB=off GOWORK=off GOTOOLCHAIN=local
go test -mod=vendor -count=1 -v ./internal/testassert
go run -mod=vendor ./task_support/baseline
go test -mod=vendor -count=1 -v ./browser -run '^(TestRedirect|TestCookieHeader|TestTabInheritance|TestLinksWithBaseTag|TestSubmitMultipart)$'
