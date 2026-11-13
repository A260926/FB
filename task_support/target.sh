#!/bin/bash
set -euo pipefail
test -f sitemap_target_test.go
bash task_support/env.sh go test -race -count=1 -timeout=30s -run '^TestSitemap(Dispatch|Document|Compatibility)$' -v .
bash task_support/env.sh go test -count=1 -timeout=30s -run '^(TestHandleBaseTag|TestCloneForRedirect|TestRedirectURLContext)$' .
