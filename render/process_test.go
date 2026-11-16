//-----------------------------------------------------------------------------
/*

Out-of-process octree rendering tests.

The tests use the test binary itself as the field process.  When the binary
is started with a -worker argument it runs a protocol worker instead of the
tests (see TestMain).  Worker behaviour is selected with additional flags and,
where a test needs to observe progress, through handshake files.

*/
//-----------------------------------------------------------------------------

package render

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/deadsy/sdfx/sdf"
	"github.com/deadsy/sdfx/task_support/taskcheck"
	v3 "github.com/deadsy/sdfx/vec/v3"
)

//-----------------------------------------------------------------------------
// Test binary worker mode

// workerRequest mirrors the protocol request.
type workerRequest struct {
	ID     int          `json:"id"`
	Points [][3]float64 `json:"points"`
}

// workerResponse mirrors the protocol response.
type workerResponse struct {
	ID     int       `json:"id"`
	Values []float64 `json:"values"`
}

// workerField evaluates the test field.  Both fields are finite and
// 1-Lipschitz over any finite box.
func workerField(kind string, p v3.Vec) float64 {
	switch kind {
	case "offset":
		c := v3.Vec{X: 0.35, Y: -0.2, Z: 0.1}
		return p.Sub(c).Length() - 0.75
	case "empty":
		return 1000.0
	default:
		return p.Length() - 1.0
	}
}

// referenceSDF returns the SDF3 matching a worker field.
func referenceSDF(t *testing.T, kind string) sdf.SDF3 {
	t.Helper()
	switch kind {
	case "offset":
		s, err := sdf.Sphere3D(0.75)
		if err != nil {
			t.Fatal(err)
		}
		return sdf.Transform3D(s, sdf.Translate3d(v3.Vec{X: 0.35, Y: -0.2, Z: 0.1}))
	default:
		s, err := sdf.Sphere3D(1.0)
		if err != nil {
			t.Fatal(err)
		}
		return s
	}
}

// workerOptions are the behaviour switches understood by the worker.
type workerOptions struct {
	kind      string
	log       string // append every raw request line here
	handshake string // write "received=<n> peak=<n>" here
	holdFrom  int    // first 1-based batch index to withhold, 0 disables
	holdPeak  int    // release held batches when this many are unanswered
	reverse   bool   // answer released batches in reverse order
	badID     bool   // answer with an id that was never used
	dup       bool   // duplicate the first response
	badJSON   bool   // write a non-JSON line before the first response
	shortVal  bool   // truncate the first response value list
	bigVal    bool   // write an out of range value in the first response
	reorder   bool   // defer the first response until later
	halfLine  bool   // write half a line and exit before the second response
	stderr    bool   // continuously write to stderr
	crash     bool   // exit before answering anything
	cancel    bool   // block forever after the first batch
	failOnce  string // first session using this marker fails with bad ids
	pidFile   string // write the process id here
}

func parseWorkerOptions(args []string) workerOptions {
	fs := flag.NewFlagSet("worker", flag.ContinueOnError)
	o := workerOptions{}
	fs.Bool("worker", false, "")
	fs.StringVar(&o.kind, "kind", "sphere", "")
	fs.StringVar(&o.log, "log", "", "")
	fs.StringVar(&o.handshake, "handshake", "", "")
	fs.IntVar(&o.holdFrom, "holdfrom", 0, "")
	fs.IntVar(&o.holdPeak, "holdpeak", 0, "")
	fs.BoolVar(&o.reverse, "reverse", false, "")
	fs.BoolVar(&o.badID, "badid", false, "")
	fs.BoolVar(&o.dup, "dup", false, "")
	fs.BoolVar(&o.reorder, "reorder", false, "")
	fs.BoolVar(&o.badJSON, "badjson", false, "")
	fs.BoolVar(&o.shortVal, "shortval", false, "")
	fs.BoolVar(&o.bigVal, "bigval", false, "")
	fs.BoolVar(&o.halfLine, "halfline", false, "")
	fs.BoolVar(&o.stderr, "stderr", false, "")
	fs.BoolVar(&o.crash, "crash", false, "")
	fs.BoolVar(&o.cancel, "cancelblock", false, "")
	fs.StringVar(&o.failOnce, "failonce", "", "")
	fs.StringVar(&o.pidFile, "pidfile", "", "")
	fs.SetOutput(io.Discard)
	_ = fs.Parse(args)
	return o
}

// runTestWorker implements the field process protocol.
func runTestWorker() {
	o := parseWorkerOptions(os.Args[1:])

	if o.pidFile != "" {
		_ = os.WriteFile(o.pidFile, []byte(strconv.Itoa(os.Getpid())), 0o644)
	}
	if o.stderr {
		// A continuously noisy stderr must be drained by the parent or the
		// pipe fills and the worker blocks.
		go func() {
			line := strings.Repeat("worker-stderr-", 8) + "\n"
			for {
				if _, err := os.Stderr.WriteString(line); err != nil {
					return
				}
				time.Sleep(time.Millisecond)
			}
		}()
	}
	if o.crash {
		os.Exit(3)
	}

	// The session marker makes the first invocation fail and later ones
	// work; every call uses a fresh process and therefore a fresh session.
	if o.failOnce != "" {
		if _, err := os.Stat(o.failOnce); err != nil {
			_ = os.WriteFile(o.failOnce, []byte("used"), 0o644)
			o.badID = true
		}
	}

	in := bufio.NewReader(os.Stdin)
	out := bufio.NewWriter(os.Stdout)

	received := 0    // requests read
	answered := 0    // responses written
	flushed := 0     // responses written, for one-shot injections
	peak := 0        // peak unanswered requests seen by the worker
	released := true // false while a hold is active
	var held []workerResponse
	var parked [][]byte
	parkedAt := -1

	if o.holdFrom > 0 {
		released = false
	}

	writeHandshake := func() {
		if o.handshake == "" {
			return
		}
		_ = os.WriteFile(o.handshake,
			[]byte(fmt.Sprintf("received=%d peak=%d\n", received, peak)), 0o644)
	}

	writeResponse := func(resp workerResponse) bool {
		values := resp.Values
		if o.shortVal && flushed == 0 {
			// Add an unexpected trailing value: the parent must reject a
			// response whose value count does not match the request.
			values = append(append([]float64(nil), values...), 0)
		}
		id := resp.ID
		if o.badID {
			id += 1 << 20
		}
		if o.bigVal && flushed == 0 && len(values) > 0 {
			line := fmt.Sprintf(`{"id":%d,"values":[1e999`, id)
			for _, v := range values[1:] {
				line += "," + strconv.FormatFloat(v, 'g', -1, 64)
			}
			line += "]}\n"
			if _, err := out.WriteString(line); err != nil {
				return false
			}
			flushed++
			answered++
			return true
		}
		data, err := json.Marshal(workerResponse{ID: id, Values: values})
		if err != nil {
			return false
		}
		if o.badJSON && flushed == 0 {
			if _, err := out.WriteString("this is not json\n"); err != nil {
				return false
			}
		}
		if o.halfLine && flushed == 1 {
			// Truncate the second response: the parent must report a partial
			// line at EOF rather than accepting it.
			_, _ = out.Write(data[:len(data)/2])
			_ = out.Flush()
			os.Exit(1)
		}
		if o.reorder && flushed == 1 {
			// Park this response and emit it only after a later batch has
			// been answered, so responses are genuinely out of order.
			parked = append(parked, data)
			parkedAt = answered
			flushed++
			answered++
			return true
		}
		if _, err := out.Write(append(data, '\n')); err != nil {
			return false
		}
		if o.dup && flushed == 0 {
			_, _ = out.Write(append(data, '\n'))
		}
		flushed++
		answered++
		return true
	}

	flushHeld := func() bool {
		batch := held
		held = nil
		if o.reverse {
			for i, j := 0, len(batch)-1; i < j; i, j = i+1, j-1 {
				batch[i], batch[j] = batch[j], batch[i]
			}
		}
		for _, resp := range batch {
			if !writeResponse(resp) {
				return false
			}
		}
		_ = out.Flush()
		return true
	}

	for {
		line, err := in.ReadBytes('\n')
		if err != nil {
			if len(line) > 0 {
				os.Exit(1)
			}
			if !flushHeld() {
				os.Exit(1)
			}
			for _, data := range parked {
				if _, err := out.Write(append(data, '\n')); err != nil {
					os.Exit(1)
				}
			}
			_ = out.Flush()
			os.Exit(0)
		}
		received++
		if o.log != "" {
			if f, err := os.OpenFile(o.log, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644); err == nil {
				_, _ = f.Write(line)
				_ = f.Close()
			}
		}
		var req workerRequest
		if err := json.Unmarshal(line, &req); err != nil {
			os.Exit(1)
		}
		if o.cancel {
			writeHandshake()
			// Read and ignore everything from now on.  The parent must
			// cancel rather than wait for responses.
			for {
				if _, err := in.ReadBytes('\n'); err != nil {
					os.Exit(0)
				}
			}
		}
		values := make([]float64, len(req.Points))
		for i, p := range req.Points {
			values[i] = workerField(o.kind, v3.Vec{X: p[0], Y: p[1], Z: p[2]})
		}
		resp := workerResponse{ID: req.ID, Values: values}

		unanswered := received - answered
		if unanswered > peak {
			peak = unanswered
		}
		if !released && received >= o.holdFrom {
			held = append(held, resp)
			if o.holdPeak > 0 && unanswered >= o.holdPeak {
				released = true
			}
		} else {
			flushHeld() // any leftovers are ready to go
			if !writeResponse(resp) {
				os.Exit(1)
			}
			_ = out.Flush()
		}
		if released && len(held) > 0 {
			if !flushHeld() {
				os.Exit(1)
			}
		}
		if len(parked) > 0 && answered > parkedAt+1 {
			// A later response has been written: now release the parked one.
			data := parked[0]
			parked = parked[1:]
			if _, err := out.Write(append(data, '\n')); err != nil {
				os.Exit(1)
			}
			_ = out.Flush()
		}
		writeHandshake()
	}
}

func TestMain(m *testing.M) {
	for _, arg := range os.Args[1:] {
		if arg == "-worker" || strings.HasPrefix(arg, "-worker=") {
			runTestWorker()
			return
		}
	}
	os.Exit(m.Run())
}

//-----------------------------------------------------------------------------
// Test helpers

// workerArgs builds the command line for a test field process.
func workerArgs(opts map[string]string) []string {
	exe, err := os.Executable()
	if err != nil {
		exe = os.Args[0]
	}
	args := []string{exe, "-worker"}
	keys := make([]string, 0, len(opts))
	for k := range opts {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		args = append(args, "-"+k+"="+opts[k])
	}
	return args
}

// collectWriter collects triangles and can inject a write error.
type collectWriter struct {
	mu     sync.Mutex
	tris   []sdf.Triangle3
	writes int
	failAt int   // number of successful writes before failing, <0 never fails
	err    error // error to return once failing
	failed bool
	after  int // write attempts after the injected failure
	closed bool
}

func (w *collectWriter) Write(in []*sdf.Triangle3) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.failed {
		w.after++
		return w.err
	}
	if w.failAt >= 0 && w.writes >= w.failAt {
		w.failed = true
		return w.err
	}
	w.writes++
	for _, t := range in {
		w.tris = append(w.tris, *t)
	}
	return nil
}

func (w *collectWriter) Close() error {
	w.mu.Lock()
	w.closed = true
	w.mu.Unlock()
	return nil
}

func (w *collectWriter) snapshot() []sdf.Triangle3 {
	w.mu.Lock()
	defer w.mu.Unlock()
	return append([]sdf.Triangle3(nil), w.tris...)
}

func (w *collectWriter) wasClosed() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.closed
}

// blockingWriter blocks the first write until release is closed.
type blockingWriter struct {
	release chan struct{}
	once    sync.Once
	inner   collectWriter
	blocked chan struct{}
	gate    sync.Once
}

func (w *blockingWriter) Write(in []*sdf.Triangle3) error {
	w.once.Do(func() {
		w.gate.Do(func() { close(w.blocked) })
		<-w.release
	})
	return w.inner.Write(in)
}

func (w *blockingWriter) Close() error { return w.inner.Close() }

// compareToReference checks triangle ordering and 1e-9 coordinate agreement.
func compareToReference(dst, ref []sdf.Triangle3) (int, float64) {
	if len(dst) != len(ref) {
		return len(dst), math.Inf(1)
	}
	worst := 0.0
	for i := range ref {
		for j := 0; j < 3; j++ {
			d := ref[i][j].Sub(dst[i][j]).Length()
			if d > worst {
				worst = d
			}
		}
	}
	return len(dst), worst
}

// checkGeometry verifies finiteness, normals and that the mesh is non-empty.
func checkGeometry(t *testing.T, tris []sdf.Triangle3) bool {
	t.Helper()
	if len(tris) == 0 {
		t.Error("mesh is empty")
		return false
	}
	for i, tri := range tris {
		for j := 0; j < 3; j++ {
			for _, v := range []float64{tri[j].X, tri[j].Y, tri[j].Z} {
				if math.IsNaN(v) || math.IsInf(v, 0) {
					t.Errorf("triangle %d vertex %d is not finite", i, j)
					return false
				}
			}
		}
		if tri.Normal().Length() < 0.5 {
			t.Errorf("triangle %d has a null normal", i)
			return false
		}
	}
	return true
}

// readRequestLog parses the raw request lines recorded by a worker.
func readRequestLog(t *testing.T, path string) []workerRequest {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Errorf("cannot read worker log: %s", err)
		return nil
	}
	defer f.Close()
	var reqs []workerRequest
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 1<<20), 1<<22)
	for sc.Scan() {
		var req workerRequest
		if err := json.Unmarshal(sc.Bytes(), &req); err != nil {
			t.Errorf("bad request line in worker log: %s", err)
			return nil
		}
		reqs = append(reqs, req)
	}
	return reqs
}

// readHandshake parses "received=<n> peak=<n>".
func readHandshake(t *testing.T, path string) (received, peak int) {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Errorf("cannot read handshake: %s", err)
		return 0, 0
	}
	_, err = fmt.Sscanf(strings.TrimSpace(string(data)), "received=%d peak=%d", &received, &peak)
	if err != nil {
		t.Errorf("bad handshake %q: %s", string(data), err)
	}
	return received, peak
}

// waitForFile waits until a file exists.
func waitForFile(path string, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(path); err == nil {
			return true
		}
		time.Sleep(2 * time.Millisecond)
	}
	return false
}

// baseConfig returns a process configuration with small controlled limits.
func baseConfig(command []string, bounds sdf.Box3, cells int) ProcessConfig {
	return ProcessConfig{
		Command:     command,
		Bounds:      bounds,
		MeshCells:   cells,
		BatchSize:   3,
		MaxInFlight: 4,
		Window:      8,
	}
}

// coordKey is an integral grid position used to prove sample sharing.
type coordKey struct{ x, y, z float64 }

func checkRequestLog(t *testing.T, reqs []workerRequest, stats ProcessStats, cfg ProcessConfig) bool {
	t.Helper()
	ok := true
	if len(reqs) != stats.Batches {
		t.Errorf("request log has %d batches, stats say %d", len(reqs), stats.Batches)
		ok = false
	}
	samples := 0
	ids := make(map[int]bool, len(reqs))
	seen := make(map[coordKey]int, len(reqs)*cfg.BatchSize)
	for _, req := range reqs {
		if ids[req.ID] {
			t.Errorf("request id %d was used more than once", req.ID)
			ok = false
		}
		ids[req.ID] = true
		samples += len(req.Points)
		if len(req.Points) > cfg.BatchSize {
			t.Errorf("request %d has %d points, BatchSize is %d", req.ID, len(req.Points), cfg.BatchSize)
			ok = false
		}
		if len(req.Points) == 0 {
			t.Errorf("request %d is empty", req.ID)
			ok = false
		}
		for _, p := range req.Points {
			k := coordKey{p[0], p[1], p[2]}
			seen[k]++
		}
	}
	if samples != stats.Samples {
		t.Errorf("request log has %d samples, stats say %d", samples, stats.Samples)
		ok = false
	}
	dups := 0
	for _, n := range seen {
		if n > 1 {
			dups += n - 1
		}
	}
	if dups != 0 {
		t.Errorf("shared pending coordinates were requested more than once: %d duplicates", dups)
		ok = false
	}
	return ok
}

func sumTriangles(tris []sdf.Triangle3) int { return len(tris) }

//-----------------------------------------------------------------------------
// Geometry

func TestProcessGeometry(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		c.Check(true, "starting geometry checks")

		// Sphere: worker holds the second batch until a third request
		// arrives, so at least two requests are outstanding, and answers
		// released batches in reverse order.
		dir := t.TempDir()
		logPath := dir + "/requests.log"
		hsPath := dir + "/handshake"
		cfg := baseConfig(workerArgs(map[string]string{
			"kind":      "sphere",
			"log":       logPath,
			"handshake": hsPath,
			"holdfrom":  "2",
			"holdpeak":  "3",
			"reverse":   "true",
		}), referenceSDF(t, "sphere").BoundingBox(), 16)

		ctx, cancel := context.WithTimeout(context.Background(), 40*time.Second)
		defer cancel()
		w := &collectWriter{failAt: -1}
		stats, err := RenderProcess(ctx, cfg, w)
		c.Check(err == nil, "sphere render failed: %v", err)
		tris := w.snapshot()
		c.Check(checkGeometry(t, tris), "sphere geometry is invalid")
		c.Check(!w.wasClosed(), "RenderProcess must not close the caller's writer")

		ref := CollectTriangles(referenceSDF(t, "sphere"), NewMarchingCubesOctreeSingle(16))
		got, worst := compareToReference(tris, ref)
		c.Check(len(ref) > 0, "reference mesh is empty")
		c.Check(got == len(ref), "triangle count mismatch: got %d, reference %d", got, len(ref))
		c.Check(worst <= 1e-9, "triangle coordinate error %g exceeds 1e-9", worst)

		// A worker that parks its first response until stdin closes is a
		// pure out-of-order responder; the render must complete, which only
		// happens if responses are matched by id rather than arrival order.
		dirReorder := t.TempDir()
		logReorder := dirReorder + "/requests.log"
		cfgReorder := baseConfig(workerArgs(map[string]string{
			"kind":    "sphere",
			"log":     logReorder,
			"reorder": "true",
		}), referenceSDF(t, "sphere").BoundingBox(), 12)
		ctxR, cancelR := context.WithTimeout(context.Background(), 40*time.Second)
		wR := &collectWriter{failAt: -1}
		statsR, errR := RenderProcess(ctxR, cfgReorder, wR)
		cancelR()
		c.Check(errR == nil, "reordered response render failed: %v", errR)
		refR := CollectTriangles(referenceSDF(t, "sphere"), NewMarchingCubesOctreeSingle(12))
		gotR, worstR := compareToReference(wR.snapshot(), refR)
		c.Check(gotR == len(refR), "reordered triangle count mismatch: got %d, reference %d", gotR, len(refR))
		c.Check(worstR <= 1e-9, "reordered coordinate error %g exceeds 1e-9", worstR)
		c.Check(statsR.Batches > 1, "reordered render used %d batches", statsR.Batches)
		c.Check(len(readRequestLog(t, logReorder)) == statsR.Batches,
			"reordered request log does not match the statistics")

		// Out of order responses are accepted, and the handshake proves
		// that at least two batches were in flight before release.
		if waitForFile(hsPath, 10*time.Second) {
			received, peak := readHandshake(t, hsPath)
			c.Check(received >= 3, "worker received %d requests, expected at least 3", received)
			c.Check(peak >= 2, "worker peak concurrency %d, expected at least 2", peak)
		} else {
			c.Check(false, "worker never produced a handshake")
		}

		reqs := readRequestLog(t, logPath)
		checkRequestLog(t, reqs, stats, cfg)
		c.Check(stats.Batches > 1, "expected multiple batches, got %d", stats.Batches)
		c.Check(stats.Samples > stats.Batches, "expected batched sampling, got %d samples in %d batches",
			stats.Samples, stats.Batches)
		bound := cfg.BatchSize * cfg.MaxInFlight
		c.Check(stats.PeakPendingSamples <= bound,
			"peak pending samples %d exceeds BatchSize*MaxInFlight %d", stats.PeakPendingSamples, bound)
		c.Check(stats.PeakPendingSamples >= cfg.BatchSize,
			"peak pending samples %d never reached a batch", stats.PeakPendingSamples)
		c.Check(stats.PeakReadyLeaves <= cfg.Window,
			"peak ready leaves %d exceeds Window %d", stats.PeakReadyLeaves, cfg.Window)

		// Offset sphere: a non-symmetric field and bounds.
		dir2 := t.TempDir()
		log2 := dir2 + "/requests.log"
		hs2 := dir2 + "/handshake"
		cfg2 := baseConfig(workerArgs(map[string]string{
			"kind":      "offset",
			"log":       log2,
			"handshake": hs2,
			"holdfrom":  "2",
			"holdpeak":  "3",
			"reverse":   "true",
		}), referenceSDF(t, "offset").BoundingBox(), 14)
		ctx2, cancel2 := context.WithTimeout(context.Background(), 40*time.Second)
		defer cancel2()
		w2 := &collectWriter{failAt: -1}
		stats2, err2 := RenderProcess(ctx2, cfg2, w2)
		c.Check(err2 == nil, "offset sphere render failed: %v", err2)
		tris2 := w2.snapshot()
		c.Check(checkGeometry(t, tris2), "offset sphere geometry is invalid")
		ref2 := CollectTriangles(referenceSDF(t, "offset"), NewMarchingCubesOctreeSingle(14))
		got2, worst2 := compareToReference(tris2, ref2)
		c.Check(got2 == len(ref2), "offset triangle count mismatch: got %d, reference %d", got2, len(ref2))
		c.Check(worst2 <= 1e-9, "offset coordinate error %g exceeds 1e-9", worst2)
		c.Check(stats2.Samples > 0, "offset render sampled nothing")

		// An empty field must not expand the whole grid: only the root
		// centre is evaluated.
		dir3 := t.TempDir()
		log3 := dir3 + "/requests.log"
		cfg3 := baseConfig(workerArgs(map[string]string{"kind": "empty", "log": log3}),
			referenceSDF(t, "sphere").BoundingBox(), 25)
		ctx3, cancel3 := context.WithTimeout(context.Background(), 20*time.Second)
		defer cancel3()
		w3 := &collectWriter{failAt: -1}
		stats3, err3 := RenderProcess(ctx3, cfg3, w3)
		c.Check(err3 == nil, "empty field render failed: %v", err3)
		c.Check(len(w3.snapshot()) == 0, "empty field produced %d triangles", len(w3.snapshot()))
		c.Check(stats3.Samples <= 8, "empty field sampled %d points; the grid was expanded", stats3.Samples)
		c.Check(stats3.Batches <= 2, "empty field used %d batches", stats3.Batches)
	})
}

//-----------------------------------------------------------------------------
// Window and backpressure

func TestProcessWindow(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		c.Check(true, "starting window checks")

		const cells = 32
		cfg := baseConfig(workerArgs(map[string]string{"kind": "sphere"}), referenceSDF(t, "sphere").BoundingBox(), cells)
		cfg.Window = 8
		cfg.BatchSize = 4
		cfg.MaxInFlight = 4

		// Block the first write so the earliest leaf is a slow prefix.  The
		// scheduler must keep refining along the frontier, but completed
		// leaves may never exceed the window while the prefix is stuck.
		bw := &blockingWriter{release: make(chan struct{}), blocked: make(chan struct{})}
		bw.inner.failAt = -1
		// Only one goroutine may close the release channel; the cleanup is a
		// no-op after the normal release.
		var releaseOnce sync.Once
		release := func() { releaseOnce.Do(func() { close(bw.release) }) }
		t.Cleanup(release)
		done := make(chan struct{})
		var stats ProcessStats
		var err error
		go func() {
			defer close(done)
			ctx, cancel := context.WithTimeout(context.Background(), 40*time.Second)
			defer cancel()
			stats, err = RenderProcess(ctx, cfg, bw)
		}()

		select {
		case <-bw.blocked:
			// The writer is blocked on the prefix write.
		case <-time.After(20 * time.Second):
			c.Check(false, "renderer never reached the writer")
			release()
			<-done
			return
		}

		// The renderer is blocked inside the prefix write, so completed
		// leaves can only queue in memory.  Release the prefix; the window
		// bound is checked from the statistics below.
		release()
		<-done

		c.Check(err == nil, "blocked-prefix render failed: %v", err)
		tris := bw.inner.snapshot()
		c.Check(checkGeometry(t, tris), "blocked-prefix geometry is invalid")
		c.Check(stats.PeakReadyLeaves <= cfg.Window,
			"peak ready leaves %d exceeds Window %d", stats.PeakReadyLeaves, cfg.Window)
		c.Check(stats.PeakReadyLeaves >= 3,
			"peak ready leaves %d: the window never accumulated work", stats.PeakReadyLeaves)
		c.Check(!bw.inner.wasClosed(), "RenderProcess must not close the writer")

		ref := CollectTriangles(referenceSDF(t, "sphere"), NewMarchingCubesOctreeSingle(cells))
		got, worst := compareToReference(tris, ref)
		c.Check(got == len(ref), "blocked-prefix triangle count mismatch: got %d, reference %d", got, len(ref))
		c.Check(worst <= 1e-9, "blocked-prefix coordinate error %g exceeds 1e-9", worst)

		// A second run with a bigger window must still respect its bound.
		cfg2 := cfg
		cfg2.Window = 12
		w2 := &collectWriter{failAt: -1}
		ctx2, cancel2 := context.WithTimeout(context.Background(), 40*time.Second)
		defer cancel2()
		stats2, err2 := RenderProcess(ctx2, cfg2, w2)
		c.Check(err2 == nil, "window=12 render failed: %v", err2)
		c.Check(stats2.PeakReadyLeaves <= cfg2.Window,
			"peak ready leaves %d exceeds Window %d", stats2.PeakReadyLeaves, cfg2.Window)
		got2, worst2 := compareToReference(w2.snapshot(), ref)
		c.Check(got2 == len(ref), "window=12 triangle count mismatch: got %d, reference %d", got2, len(ref))
		c.Check(worst2 <= 1e-9, "window=12 coordinate error %g exceeds 1e-9", worst2)
		c.Check(stats2.Samples == stats.Samples, "samples changed with the window: %d vs %d",
			stats2.Samples, stats.Samples)
		c.Check(sumTriangles(w2.snapshot()) == sumTriangles(tris),
			"triangle count changed with the window")
	})
}

//-----------------------------------------------------------------------------
// Lifecycle: cancellation, protocol errors, stderr, writer errors and
// session isolation.

func TestProcessLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		sphere := referenceSDF(t, "sphere")
		bounds := sphere.BoundingBox()

		// Cancellation of a blocked worker must return the context error
		// promptly, without waiting on the pipes.
		dir := t.TempDir()
		hs := dir + "/handshake"
		cfg := baseConfig(workerArgs(map[string]string{"kind": "sphere", "handshake": hs, "cancelblock": "true"}),
			bounds, 12)
		ctx, cancel := context.WithCancel(context.Background())
		start := time.Now()
		// Wait for the worker to be running before cancelling; the handshake
		// is written from inside the worker, so this is not a timing guess.
		go func() {
			if waitForFile(hs, 10*time.Second) {
				cancel()
			}
		}()
		_, err := RenderProcess(ctx, cfg, &collectWriter{failAt: -1})
		cancel()
		elapsed := time.Since(start)
		c.Check(err != nil, "cancelled render returned no error")
		c.Check(errors.Is(err, context.Canceled),
			"cancelled render returned %v, expected context cancellation", err)
		c.Check(elapsed < 10*time.Second, "cancelled render took %s", elapsed)

		// Unknown id.
		_, err = runLifecycle(workerArgs(map[string]string{"kind": "sphere", "badid": "true"}), bounds, 10)
		c.Check(err != nil && strings.Contains(err.Error(), "unknown or duplicate"),
			"unknown id returned %v", err)

		// Duplicate response.
		_, err = runLifecycle(workerArgs(map[string]string{"kind": "sphere", "dup": "true"}), bounds, 10)
		c.Check(err != nil && strings.Contains(err.Error(), "unknown or duplicate"),
			"duplicate response returned %v", err)

		// Value count mismatch.
		_, err = runLifecycle(workerArgs(map[string]string{"kind": "sphere", "shortval": "true"}), bounds, 10)
		c.Check(err != nil && (strings.Contains(err.Error(), "expected") || strings.Contains(err.Error(), "values")),
			"count mismatch returned %v", err)

		// Invalid JSON on stdout.
		_, err = runLifecycle(workerArgs(map[string]string{"kind": "sphere", "badjson": "true"}), bounds, 10)
		c.Check(err != nil && strings.Contains(err.Error(), "invalid JSON"),
			"invalid JSON returned %v", err)

		// Non-finite / out of range value.
		_, err = runLifecycle(workerArgs(map[string]string{"kind": "sphere", "bigval": "true"}), bounds, 10)
		c.Check(err != nil, "out of range value was accepted")

		// Half line at EOF.
		_, err = runLifecycle(workerArgs(map[string]string{"kind": "sphere", "halfline": "true"}), bounds, 10)
		c.Check(err != nil, "half line EOF was accepted")

		// Abnormal process exit.
		_, err = runLifecycle(workerArgs(map[string]string{"kind": "sphere", "crash": "true"}), bounds, 10)
		c.Check(err != nil, "crashing worker was accepted")

		// stderr is drained continuously, so a noisy worker still renders.
		dir2 := t.TempDir()
		log2 := dir2 + "/requests.log"
		w2 := &collectWriter{failAt: -1}
		cfg2 := baseConfig(workerArgs(map[string]string{"kind": "sphere", "stderr": "true", "log": log2}), bounds, 12)
		ctx2, cancel2 := context.WithTimeout(context.Background(), 40*time.Second)
		stats2, err2 := RenderProcess(ctx2, cfg2, w2)
		cancel2()
		c.Check(err2 == nil, "noisy stderr render failed: %v", err2)
		c.Check(len(w2.snapshot()) > 0, "noisy stderr render produced no triangles")
		c.Check(stats2.Samples > 0, "noisy stderr render sampled nothing")

		// The retained stderr tail is reported with a failure and is capped
		// at 4 KiB.
		_, err = runLifecycle(workerArgs(map[string]string{"kind": "sphere", "stderr": "true", "badid": "true"}), bounds, 10)
		c.Check(err != nil, "noisy worker with bad ids returned no error")
		if err != nil {
			c.Check(strings.Contains(err.Error(), "worker-stderr-"),
				"error does not include the stderr tail: %v", err)
			c.Check(len(err.Error()) < 6*1024, "error carries %d bytes of stderr", len(err.Error()))
		}

		// Writer errors are returned unchanged, and nothing is written after
		// the failure.  Already delivered triangles stay delivered.
		injected := errors.New("writer failed")
		fw := &collectWriter{failAt: 2, err: injected}
		cfg3 := baseConfig(workerArgs(map[string]string{"kind": "sphere"}), bounds, 12)
		ctx3, cancel3 := context.WithTimeout(context.Background(), 40*time.Second)
		_, err3 := RenderProcess(ctx3, cfg3, fw)
		cancel3()
		c.Check(errors.Is(err3, injected), "writer error was not returned unchanged: %v", err3)
		c.Check(fw.after == 0, "%d writes happened after the writer failed", fw.after)
		c.Check(len(fw.snapshot()) > 0, "delivered prefix was rolled back")

		// A failed session does not poison the next call: a fresh session is
		// started with its own id table and cache.
		dir4 := t.TempDir()
		marker := dir4 + "/marker"
		log4 := dir4 + "/requests.log"
		cmd4 := workerArgs(map[string]string{"kind": "sphere", "failonce": marker, "log": log4})
		_, err = runLifecycle(cmd4, bounds, 12)
		c.Check(err != nil, "first (failing) session returned no error")
		cfg5 := baseConfig(cmd4, bounds, 12)
		w5 := &collectWriter{failAt: -1}
		ctx5, cancel5 := context.WithTimeout(context.Background(), 40*time.Second)
		stats5, err5 := RenderProcess(ctx5, cfg5, w5)
		cancel5()
		c.Check(err5 == nil, "second session after a failure failed: %v", err5)
		c.Check(stats5.Samples > 0, "second session sampled nothing")
		ref := CollectTriangles(sphere, NewMarchingCubesOctreeSingle(12))
		got5, worst5 := compareToReference(w5.snapshot(), ref)
		c.Check(got5 == len(ref), "second session triangle count mismatch: got %d, reference %d", got5, len(ref))
		c.Check(worst5 <= 1e-9, "second session coordinate error %g exceeds 1e-9", worst5)
		reqs := readRequestLog(t, log4)
		zeros := 0
		for _, r := range reqs {
			if r.ID == 0 {
				zeros++
			}
		}
		c.Check(zeros >= 2, "id table was not reset: %d requests used id 0", zeros)

		// Two concurrent instances keep separate ids, caches and outputs.
		var wg sync.WaitGroup
		var sA, sB ProcessStats
		var eA, eB error
		dirA := t.TempDir()
		dirB := t.TempDir()
		logA := dirA + "/requests.log"
		logB := dirB + "/requests.log"
		wA := &collectWriter{failAt: -1}
		wB := &collectWriter{failAt: -1}
		cfgA := baseConfig(workerArgs(map[string]string{"kind": "sphere", "log": logA}), sphere.BoundingBox(), 12)
		cfgB := baseConfig(workerArgs(map[string]string{"kind": "offset", "log": logB}),
			referenceSDF(t, "offset").BoundingBox(), 12)
		wg.Add(2)
		go func() {
			defer wg.Done()
			ctx, cancel := context.WithTimeout(context.Background(), 40*time.Second)
			defer cancel()
			sA, eA = RenderProcess(ctx, cfgA, wA)
		}()
		go func() {
			defer wg.Done()
			ctx, cancel := context.WithTimeout(context.Background(), 40*time.Second)
			defer cancel()
			sB, eB = RenderProcess(ctx, cfgB, wB)
		}()
		wg.Wait()
		c.Check(eA == nil, "instance A failed: %v", eA)
		c.Check(eB == nil, "instance B failed: %v", eB)
		refA := CollectTriangles(sphere, NewMarchingCubesOctreeSingle(12))
		refB := CollectTriangles(referenceSDF(t, "offset"), NewMarchingCubesOctreeSingle(12))
		gotA, worstA := compareToReference(wA.snapshot(), refA)
		gotB, worstB := compareToReference(wB.snapshot(), refB)
		c.Check(gotA == len(refA), "instance A triangle count mismatch")
		c.Check(worstA <= 1e-9, "instance A coordinate error %g", worstA)
		c.Check(gotB == len(refB), "instance B triangle count mismatch")
		c.Check(worstB <= 1e-9, "instance B coordinate error %g", worstB)
		c.Check(sA.Samples > 0 && sB.Samples > 0, "an instance sampled nothing")
		for _, path := range []string{logA, logB} {
			reqs := readRequestLog(t, path)
			zeros := 0
			for _, r := range reqs {
				if r.ID == 0 {
					zeros++
				}
			}
			c.Check(zeros == 1, "%s did not start a fresh id table (%d id 0 requests)", path, zeros)
		}

		// Invalid configurations fail before starting a process or writing.
		badWriter := &collectWriter{failAt: -1}
		cfgBad := baseConfig([]string{os.DevNull}, bounds, 10)
		cfgBad.Window = 4
		_, err = RenderProcess(context.Background(), cfgBad, badWriter)
		c.Check(err != nil, "Window < 8 was accepted")
		c.Check(len(badWriter.snapshot()) == 0, "invalid config wrote output")
		cfgBad = baseConfig([]string{os.DevNull}, bounds, 10)
		cfgBad.BatchSize = 0
		_, err = RenderProcess(context.Background(), cfgBad, badWriter)
		c.Check(err != nil, "BatchSize 0 was accepted")
		cfgBad = baseConfig([]string{}, bounds, 10)
		_, err = RenderProcess(context.Background(), cfgBad, badWriter)
		c.Check(err != nil, "empty command was accepted")
		cfgBad = baseConfig([]string{"/nonexistent/field-process-binary"}, bounds, 10)
		_, err = RenderProcess(context.Background(), cfgBad, badWriter)
		c.Check(err != nil, "missing executable was accepted")
		c.Check(len(badWriter.snapshot()) == 0, "a failed start wrote output")
	})
}

// runLifecycle runs one render expected to fail or succeed, returning the
// error and collected triangles.
func runLifecycle(command []string, bounds sdf.Box3, cells int) ([]sdf.Triangle3, error) {
	cfg := baseConfig(command, bounds, cells)
	w := &collectWriter{failAt: -1}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	_, err := RenderProcess(ctx, cfg, w)
	return w.snapshot(), err
}

//-----------------------------------------------------------------------------
