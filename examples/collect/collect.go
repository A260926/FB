// Package collect implements a parallel device-interface collection
// flow on top of a querier (typically a coap.Session): resources are
// read concurrently, successful responses are parsed into structured
// records, duplicates are dropped, and the committed records are
// written to a JSON Lines file.  Responses with 4.xx/5.xx codes and
// malformed payloads are reported in a failure list carrying the
// resource identity instead of being treated as measurements.
package collect

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
)

// Record is a single committed measurement.
type Record struct {
	Resource string  `json:"resource"`
	SampleID string  `json:"sample_id"`
	Value    float64 `json:"value"`
}

// Failure describes a resource that produced no measurement.
type Failure struct {
	Resource string `json:"resource"`
	Reason   string `json:"reason"`
}

// Response is the outcome of querying one resource.
type Response struct {
	// Code is the CoAP response code (class.detail, e.g. 69 for 2.05).
	Code    int
	Payload []byte
}

// Querier reads one resource.  It is implemented by adapters around a
// CoAP session; see QuerierFunc and the collect example command.
type Querier interface {
	Query(ctx context.Context, resource string) (Response, error)
}

// QuerierFunc adapts a function to the Querier interface.
type QuerierFunc func(ctx context.Context, resource string) (Response, error)

// Query implements Querier.
func (f QuerierFunc) Query(ctx context.Context, resource string) (Response, error) {
	return f(ctx, resource)
}

// ParseRecord parses a device payload of the form
// "sample_id=<id>;value=<number>" into a Record for resource.
func ParseRecord(resource string, payload []byte) (Record, error) {
	rec := Record{Resource: resource}
	var haveID, haveValue bool
	for _, field := range strings.Split(string(payload), ";") {
		kv := strings.SplitN(strings.TrimSpace(field), "=", 2)
		if len(kv) != 2 {
			return rec, fmt.Errorf("collect: malformed field %q in payload %q", field, payload)
		}
		switch strings.TrimSpace(kv[0]) {
		case "sample_id":
			rec.SampleID = strings.TrimSpace(kv[1])
			haveID = rec.SampleID != ""
		case "value":
			v, err := strconv.ParseFloat(strings.TrimSpace(kv[1]), 64)
			if err != nil {
				return rec, fmt.Errorf("collect: malformed value in payload %q: %w", payload, err)
			}
			rec.Value = v
			haveValue = true
		default:
			return rec, fmt.Errorf("collect: unknown field %q in payload %q", kv[0], payload)
		}
	}
	if !haveID || !haveValue {
		return rec, fmt.Errorf("collect: payload %q is missing sample_id or value", payload)
	}
	return rec, nil
}

// Result is the outcome of a collection run: the committed records
// (deduplicated by resource/sample_id), the failures with resource
// identity, and the number of duplicate deliveries that were dropped.
type Result struct {
	Records    []Record
	Failures   []Failure
	Duplicates int
}

// Collect queries every resource concurrently and aggregates the
// results.  A 2.xx response is parsed into a record; the first
// resource/sample_id pair is committed and later duplicates are
// counted, never committed twice.  4.xx/5.xx responses, malformed
// payloads and query errors (including cancellation of an individual
// resource) land in the failure list without affecting the other
// resources.
func Collect(ctx context.Context, q Querier, resources []string) *Result {
	res := &Result{}
	seen := make(map[string]bool)
	var mu sync.Mutex
	var wg sync.WaitGroup
	for _, resource := range resources {
		wg.Add(1)
		go func(resource string) {
			defer wg.Done()
			resp, err := q.Query(ctx, resource)
			mu.Lock()
			defer mu.Unlock()
			switch {
			case err != nil:
				res.Failures = append(res.Failures, Failure{resource, err.Error()})
			case resp.Code>>5 == 4 || resp.Code>>5 == 5:
				res.Failures = append(res.Failures, Failure{resource,
					fmt.Sprintf("device returned %d.%02d", resp.Code>>5, resp.Code&0x1f)})
			case resp.Code>>5 != 2:
				res.Failures = append(res.Failures, Failure{resource,
					fmt.Sprintf("unexpected response code %d.%02d", resp.Code>>5, resp.Code&0x1f)})
			default:
				rec, err := ParseRecord(resource, resp.Payload)
				if err != nil {
					res.Failures = append(res.Failures, Failure{resource, err.Error()})
					return
				}
				key := resource + "\x00" + rec.SampleID
				if seen[key] {
					res.Duplicates++
					return
				}
				seen[key] = true
				res.Records = append(res.Records, rec)
			}
		}(resource)
	}
	wg.Wait()
	sort.Slice(res.Records, func(i, j int) bool {
		if res.Records[i].Resource != res.Records[j].Resource {
			return res.Records[i].Resource < res.Records[j].Resource
		}
		return res.Records[i].SampleID < res.Records[j].SampleID
	})
	sort.Slice(res.Failures, func(i, j int) bool {
		return res.Failures[i].Resource < res.Failures[j].Resource
	})
	return res
}

// WriteJSONL commits records to path, one complete JSON object per
// line.
func WriteJSONL(path string, records []Record) error {
	var buf bytes.Buffer
	for _, r := range records {
		line, err := json.Marshal(r)
		if err != nil {
			return err
		}
		buf.Write(line)
		buf.WriteByte('\n')
	}
	return os.WriteFile(path, buf.Bytes(), 0o644)
}

// ReadJSONL reads back a JSON Lines file of records.
func ReadJSONL(path string) ([]Record, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var out []Record
	for _, line := range strings.Split(string(data), "\n") {
		if line == "" {
			continue
		}
		var r Record
		if err := json.Unmarshal([]byte(line), &r); err != nil {
			return nil, fmt.Errorf("collect: %s: %w", path, err)
		}
		out = append(out, r)
	}
	return out, nil
}
