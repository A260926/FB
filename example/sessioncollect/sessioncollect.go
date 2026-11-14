// Package sessioncollect shows how to drive several already attached,
// non-flattened CDP target sessions through a single parent rpcc.Conn.
//
// Every page is controlled through a child Conn created with
// (*rpcc.Conn).NewSession, using the generated runtime client. Product
// records are collected with Runtime.evaluate and page updates arrive over
// an rpcc.NewStream subscription; records are merged and de-duplicated per
// session and product ID and emitted as JSON Lines.
package sessioncollect

import (
	"context"
	"encoding/json"
	"io"
	"sync"
	"time"

	"github.com/mafredri/cdp/protocol/runtime"
	"github.com/mafredri/cdp/rpcc"
)

// consoleAPICalledMethod is the CDP event used for page update notifications.
const consoleAPICalledMethod = "Runtime.consoleAPICalled"

// Product is one merchandise record as exposed by a simulated page.
type Product struct {
	ID    string `json:"id"`
	Name  string `json:"name"`
	Price int    `json:"price"`
}

// Record is a published JSON Lines entry. Source identifies the page the
// record came from.
type Record struct {
	SessionID string  `json:"sessionId"`
	Source    string  `json:"source"`
	Product   Product `json:"product"`
}

// PageFailure reports that a page could not be collected. Err retains the
// original error so callers can use errors.Is and errors.As.
type PageFailure struct {
	SessionID string `json:"sessionId"`
	Source    string `json:"source"`
	Op        string `json:"op"`
	Message   string `json:"message"`
	Err       error  `json:"-"`
}

// PageConfig identifies one attached target session.
type PageConfig struct {
	SessionID string
	Source    string
}

// Options configures a collection run.
type Options struct {
	// Expression is evaluated on every page and must return an array
	// of Product values by value.
	Expression string
	// UpdateTimeout limits how long each page waits for update
	// notifications after the initial evaluation.
	UpdateTimeout time.Duration
}

// consoleEvent is the subset of Runtime.consoleAPICalled that carries a
// product update.
type consoleEvent struct {
	Type string `json:"type"`
	Args []struct {
		Type  string          `json:"type"`
		Value json.RawMessage `json:"value"`
	} `json:"args"`
}

// Collect evaluates every configured page through its own child session
// Conn, merges the initial products with streamed updates (de-duplicating
// by session ID and product ID) and writes the result as JSON Lines to w.
//
// A failure on one page (outer rejection, inner error, malformed message
// or detach) is reported in the returned failure list and never pollutes
// another page's data. The parent Conn is not closed and stays usable.
func Collect(ctx context.Context, parent *rpcc.Conn, pages []PageConfig, opts Options, w io.Writer) ([]PageFailure, error) {
	if opts.UpdateTimeout == 0 {
		opts.UpdateTimeout = 500 * time.Millisecond
	}

	var (
		writeMu sync.Mutex
		seen    = make(map[string]struct{})
		fails   []PageFailure
		failMu  sync.Mutex
		wg      sync.WaitGroup
	)

	emit := func(r Record) bool {
		key := r.SessionID + "\x00" + r.Product.ID
		writeMu.Lock()
		defer writeMu.Unlock()
		if _, ok := seen[key]; ok {
			return false
		}
		seen[key] = struct{}{}
		// One Encode per record keeps the output JSON Lines even when
		// several pages publish concurrently.
		return json.NewEncoder(w).Encode(r) == nil
	}

	recordFailure := func(f PageFailure) {
		failMu.Lock()
		fails = append(fails, f)
		failMu.Unlock()
	}

	for _, page := range pages {
		wg.Add(1)
		go func(page PageConfig) {
			defer wg.Done()

			if failure := collectPage(ctx, parent, page, opts, emit); failure != nil {
				recordFailure(*failure)
			}
		}(page)
	}
	wg.Wait()

	return fails, nil
}

// collectPage runs the full lifecycle of one page session. It only
// publishes records after the dual (outer and inner) reply of
// Runtime.evaluate has fully succeeded, so an inner answer that was merely
// parked behind a rejected outer request can never leak into the output.
func collectPage(ctx context.Context, parent *rpcc.Conn, page PageConfig, opts Options, emit func(Record) bool) *PageFailure {
	// NewSession neither attaches nor opens a physical connection; the
	// session was attached by the caller beforehand.
	child, err := parent.NewSession(page.SessionID)
	if err != nil {
		return &PageFailure{
			SessionID: page.SessionID, Source: page.Source,
			Op: "NewSession", Message: err.Error(), Err: err,
		}
	}
	// Closing the child ends only its own commands and streams, no
	// Target.detachFromTarget is sent and the parent Conn stays open.
	defer child.Close()

	client := runtime.NewClient(child)

	evalReply, err := client.Evaluate(ctx,
		runtime.NewEvaluateArgs(opts.Expression).SetReturnByValue(true))
	if err != nil {
		return &PageFailure{
			SessionID: page.SessionID, Source: page.Source,
			Op: "Runtime.evaluate", Message: err.Error(), Err: err,
		}
	}

	var products []Product
	if err := json.Unmarshal(evalReply.Result.Value, &products); err != nil {
		return &PageFailure{
			SessionID: page.SessionID, Source: page.Source,
			Op: "decode", Message: err.Error(), Err: err,
		}
	}
	for _, p := range products {
		emit(Record{SessionID: page.SessionID, Source: page.Source, Product: p})
	}

	// The stream is bound to the per-page deadline so that RecvMsg
	// unblocks on its own once the update window has elapsed.
	updateCtx, cancel := context.WithTimeout(ctx, opts.UpdateTimeout)
	defer cancel()
	stream, err := rpcc.NewStream(updateCtx, consoleAPICalledMethod, child)
	if err != nil {
		return &PageFailure{
			SessionID: page.SessionID, Source: page.Source,
			Op: "NewStream", Message: err.Error(), Err: err,
		}
	}
	defer stream.Close()

	// Drain update notifications until the per-page deadline. A missing
	// update is not an error; a closed session (detach, bad message)
	// ends the loop with an error.
	for {
		var event consoleEvent
		err := stream.RecvMsg(&event)
		if err != nil {
			if updateCtx.Err() != nil {
				return nil
			}
			return &PageFailure{
				SessionID: page.SessionID, Source: page.Source,
				Op: "update", Message: err.Error(), Err: err,
			}
		}
		for _, arg := range event.Args {
			var p Product
			if err := json.Unmarshal(arg.Value, &p); err != nil {
				continue
			}
			emit(Record{SessionID: page.SessionID, Source: page.Source, Product: p})
		}
	}
}
