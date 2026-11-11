GO ?= go

.PHONY: build test test-race test-live vet lint fuzz tui clean

build:
	$(GO) build ./...

test:
	$(GO) test -short ./...

test-race:
	$(GO) test -short -race ./...

# Live integration tests against public STAC APIs (Earth Search, Planetary Computer).
# Requires network access; skips upstream 5xx as flakes.
test-live:
	$(GO) test -tags=live -timeout=300s -v -run TestLive ./pkg/client/

vet:
	$(GO) vet ./...

lint:
	$(GO) vet ./...
	gofmt -l . | (! grep .)

fuzz:
	$(GO) test -run=^$$ -fuzz=^FuzzItemUnmarshal$$ -fuzztime=20s ./pkg/stac/
	$(GO) test -run=^$$ -fuzz=^FuzzCollectionUnmarshal$$ -fuzztime=20s ./pkg/stac/
	$(GO) test -run=^$$ -fuzz=^FuzzCatalogUnmarshal$$ -fuzztime=20s ./pkg/stac/
	$(GO) test -run=^$$ -fuzz=^FuzzLinkUnmarshal$$ -fuzztime=20s ./pkg/stac/

tui:
	$(GO) run ./cmd/tui

clean:
	$(GO) clean
