//-----------------------------------------------------------------------------
/*

Marching Cubes Octree with an External Field Process

The octree traversal is driven by a batched, asynchronous dependency
scheduler.  Field evaluations are sent to a subprocess (see
process_sampler.go) in batches, while the octree is refined along a
controlled frontier.  Triangle batches are retired in the same depth first
order as NewMarchingCubesOctreeSingle, and the geometry (sampling grid,
bounding box expansion, emptiness test, corner interpolation and triangle
winding) is identical to that implementation.

*/
//-----------------------------------------------------------------------------

package render

import (
	"container/heap"
	"context"
	"errors"
	"fmt"
	"math"

	"github.com/deadsy/sdfx/sdf"
	"github.com/deadsy/sdfx/vec/conv"
	v3 "github.com/deadsy/sdfx/vec/v3"
	"github.com/deadsy/sdfx/vec/v3i"
)

//-----------------------------------------------------------------------------

// ProcessConfig configures RenderProcess.
type ProcessConfig struct {
	Command     []string // executable and arguments for the field process
	Bounds      sdf.Box3 // bounding box enclosing the surface to extract
	MeshCells   int      // cells on the longest axis, as for MarchingCubesOctreeSingle
	BatchSize   int      // maximum points per request
	MaxInFlight int      // maximum outstanding requests
	Window      int      // maximum completed-but-unwritten leaf batches
}

// ProcessStats reports what a RenderProcess call did.
type ProcessStats struct {
	Batches            int // requests sent to the field process
	Samples            int // points requested from the field process
	PeakPendingSamples int // peak points sent but not yet answered
	PeakReadyLeaves    int // peak leaf triangle batches finished but unwritten
}

//-----------------------------------------------------------------------------

// maxOctreeDepth bounds the leaf ordinal space (8^20 fits in a uint64).
const maxOctreeDepth = 20

// writeError carries a triangle writer error.  It is returned to the caller
// unchanged rather than decorated with process diagnostics.
type writeError struct{ err error }

func (e *writeError) Error() string { return e.err.Error() }
func (e *writeError) Unwrap() error { return e.err }

// procParams are the validated, derived process parameters.
type procParams struct {
	origin      v3.Vec    // low corner of the expanded bounding box
	resolution  float64   // size of a level-0 grid cell
	levels      uint      // number of octree levels, as in march3x
	leafDepth   int       // levels-2
	totalLeaves uint64    // 8^leafDepth, number of possible leaf positions
	hdiag       []float64 // half diagonal length per level
}

func validateProcessConfig(cfg ProcessConfig) (*procParams, error) {
	if len(cfg.Command) == 0 || cfg.Command[0] == "" {
		return nil, fmt.Errorf("render process: empty command")
	}
	size := cfg.Bounds.Size()
	for _, v := range []float64{cfg.Bounds.Min.X, cfg.Bounds.Min.Y, cfg.Bounds.Min.Z, size.X, size.Y, size.Z} {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return nil, fmt.Errorf("render process: bounds are not finite")
		}
	}
	if size.X <= 0 || size.Y <= 0 || size.Z <= 0 {
		return nil, fmt.Errorf("render process: bounds must have positive width on every axis")
	}
	if cfg.MeshCells <= 0 {
		return nil, fmt.Errorf("render process: MeshCells must be positive")
	}
	if cfg.BatchSize <= 0 {
		return nil, fmt.Errorf("render process: BatchSize must be positive")
	}
	if cfg.MaxInFlight <= 0 {
		return nil, fmt.Errorf("render process: MaxInFlight must be positive")
	}
	if cfg.Window < 8 {
		return nil, fmt.Errorf("render process: Window must be at least 8")
	}

	// The sampling grid matches marchingCubesOctree/marchingCubesOctree.
	bb := cfg.Bounds.ScaleAboutCenter(1.01)
	longAxis := bb.Size().MaxComponent()
	resolution := 0.5 * (cfg.Bounds.Size().MaxComponent() / float64(cfg.MeshCells))
	levels := uint(math.Ceil(math.Log2(longAxis/resolution))) + 1
	if levels < 2 {
		levels = 2
	}
	leafDepth := int(levels) - 2
	if leafDepth > maxOctreeDepth {
		return nil, fmt.Errorf("render process: mesh is too fine (octree depth %d)", leafDepth)
	}

	p := &procParams{
		origin:     bb.Min,
		resolution: resolution,
		levels:     levels,
		leafDepth:  leafDepth,
		hdiag:      make([]float64, levels),
	}
	p.totalLeaves = 1
	for i := 0; i < leafDepth; i++ {
		p.totalLeaves *= 8
	}
	for i := range p.hdiag {
		side := float64(uint64(1)<<uint(i)) * resolution
		p.hdiag[i] = 0.5 * math.Sqrt(3.0*side*side)
	}
	return p, nil
}

//-----------------------------------------------------------------------------
// Scheduler state

// procWaiter records that a node is waiting for a sample.
// corner == -1 means the node's center sample.
type procWaiter struct {
	n      *procNode
	corner int
}

// procSample is one pending or resolved field evaluation, shared by every
// node that depends on the same integer grid coordinate.
type procSample struct {
	coord      v3i.Vec
	pos        v3.Vec
	waiters    []procWaiter
	order      uint64 // min ordinal of the dependent nodes
	dispatched bool
	resolved   bool
	value      float64
}

// procNode is one octree cube being processed.
type procNode struct {
	c          cube
	lo, hi     uint64 // range of leaf ordinals below this node
	span       uint64 // leaf ordinals per child
	phase      uint8  // 0: center pending, 1: center known non-empty
	leaf       bool
	done       bool
	center     *procSample
	corners    [8]*procSample
	cornerWait int
	queued     bool
	cornersReq bool // leaf corner samples have been requested
}

type procState struct {
	params *procParams
	cfg    ProcessConfig

	out    sdf.Triangle3Writer
	sendCh chan *sendBatch
	outbox []*sendBatch
	stats  ProcessStats

	settled uint64 // leaf positions strictly before this are decided

	samples        map[v3i.Vec]*procSample
	pending        map[int][]*procSample // outstanding batches by request id
	nextID         int
	inFlightPoints int
	unresolved     int // samples requested but not yet answered

	dirty    dirtyHeap    // nodes whose center/corners are ready, by lo
	expand   expandHeap   // internal nodes with children left to create, by next child lo
	dispatch dispatchHeap // samples waiting to be sent, by order
	ready    readyHeap    // completed leaves waiting to be written, by ord
	empty    rangeHeap    // decided empty leaf ranges, by lo
	parked   parkedHeap   // non-empty leaves waiting for a window slot, by lo
}

func newProcState(cfg ProcessConfig, params *procParams, out sdf.Triangle3Writer, sendCh chan *sendBatch) *procState {
	return &procState{
		params:  params,
		cfg:     cfg,
		out:     out,
		sendCh:  sendCh,
		samples: make(map[v3i.Vec]*procSample),
		pending: make(map[int][]*procSample),
	}
}

func (s *procState) windowEnd() uint64 {
	return s.settled + uint64(s.cfg.Window)
}

//-----------------------------------------------------------------------------

// RenderProcess renders the surface enclosed by cfg.Bounds by evaluating its
// field in a subprocess.  It does not call out.Close; the caller owns the
// writer after a successful call.
func RenderProcess(ctx context.Context, cfg ProcessConfig, out sdf.Triangle3Writer) (ProcessStats, error) {
	var stats ProcessStats
	params, err := validateProcessConfig(cfg)
	if err != nil {
		return stats, err
	}

	sampler, err := startProcessSampler(cfg.Command, cfg.MaxInFlight)
	if err != nil {
		return stats, err
	}

	s := newProcState(cfg, params, out, sampler.sendCh)
	err = s.run(ctx, sampler)
	if err != nil {
		sampler.abort()
		var we *writeError
		if errors.As(err, &we) {
			// Return the writer's own error untouched.
			return s.stats, we.err
		}
		if ctx.Err() != nil {
			return s.stats, ctx.Err()
		}
		return s.stats, sampler.withTail(err)
	}

	if ferr := sampler.finish(ctx); ferr != nil {
		if ctx.Err() != nil {
			return s.stats, ctx.Err()
		}
		return s.stats, sampler.processExitError(ferr)
	}
	return s.stats, nil
}

// run drives the scheduler until the octree is resolved, an error occurs or
// the context is canceled.
func (s *procState) run(ctx context.Context, sampler *processSampler) error {
	root := &procNode{
		c:    cube{v3i.Vec{0, 0, 0}, s.params.levels - 1},
		lo:   0,
		hi:   s.params.totalLeaves - 1,
		leaf: s.params.levels == 2,
	}
	root.span = s.params.totalLeaves
	s.waitCenter(root)

	for {
		if err := s.pump(); err != nil {
			return err
		}
		if s.complete() {
			return nil
		}
		if ctx.Err() != nil {
			return ctx.Err()
		}
		if err := s.flushOutbox(ctx, sampler); err != nil {
			return err
		}
		select {
		case ev := <-sampler.events:
			if ev.err != nil {
				return ev.err
			}
			if ev.eof {
				return sampler.withTail(fmt.Errorf("field process: exited before the octree was complete"))
			}
			if err := s.handleResponse(ev.id, ev.values); err != nil {
				return err
			}
		case err := <-sampler.errCh:
			return sampler.withTail(err)
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}

func (s *procState) complete() bool {
	return s.settled >= s.params.totalLeaves && len(s.pending) == 0
}

//-----------------------------------------------------------------------------

// pump performs every currently possible state transition.  It repeats until
// a full pass makes no progress (typically because it is waiting for field
// process responses or for the in-flight limit).
func (s *procState) pump() error {
	for {
		progress := false

		changed, err := s.settle()
		if err != nil {
			return err
		}
		progress = progress || changed

		for s.dirty.Len() > 0 {
			n := heap.Pop(&s.dirty).(*procNode)
			n.queued = false
			if err := s.advanceNode(n); err != nil {
				return err
			}
			progress = true
		}

		// Non-empty leaves that were parked outside the window may now be
		// inside it: request their corners.
		for s.parked.Len() > 0 && s.parked[0].lo < s.windowEnd() {
			n := heap.Pop(&s.parked).(*procNode)
			s.requestCorners(n)
			progress = true
		}

		// Expand the frontier until enough samples are outstanding to fill
		// every in-flight slot, but never past a short lookahead beyond the
		// output window.  This keeps the traversal bounded by the window and
		// the in-flight limit instead of materialising the whole grid.
		target := s.cfg.BatchSize * s.cfg.MaxInFlight
		frontier := s.windowEnd() + uint64(s.cfg.Window)
		for s.expand.Len() > 0 && s.unresolved < target {
			n := s.expand[0]
			if n.lo >= frontier {
				break
			}
			heap.Pop(&s.expand)
			s.createChildren(n)
			progress = true
		}

		if s.dispatchSamples() {
			progress = true
		}

		if !progress {
			return nil
		}
	}
}

// settle decides empty leaf ranges and writes ready leaves in DFS order.
func (s *procState) settle() (bool, error) {
	progress := false
	for {
		advanced := false
		for s.empty.Len() > 0 && s.empty[0].lo <= s.settled {
			r := heap.Pop(&s.empty).(rangeItem)
			if r.hi < s.settled {
				continue // already covered
			}
			if r.lo > s.settled {
				heap.Push(&s.empty, r)
				break
			}
			s.settled = r.hi + 1
			advanced = true
		}
		if s.ready.Len() > 0 && s.ready[0].ord == s.settled {
			r := heap.Pop(&s.ready).(*leafResult)
			if len(r.tris) > 0 {
				if err := s.out.Write(r.tris); err != nil {
					return progress, &writeError{err: err}
				}
			}
			s.settled++
			advanced = true
		}
		if !advanced {
			break
		}
		progress = true
	}
	return progress, nil
}

// advanceNode reacts to a node whose center sample has been resolved.
func (s *procState) advanceNode(n *procNode) error {
	if n.done || n.phase != 0 || n.center == nil || !n.center.resolved {
		return nil
	}
	if math.Abs(n.center.value) >= s.params.hdiag[n.c.n] {
		// The cube cannot contain the surface.
		n.done = true
		heap.Push(&s.empty, rangeItem{n.lo, n.hi})
		return nil
	}
	n.phase = 1
	if n.leaf {
		// Only leaves inside the output window ask for their corners, so the
		// number of finished-but-unwritten leaves can never exceed it.
		if n.lo >= s.windowEnd() {
			heap.Push(&s.parked, n)
			return nil
		}
		s.requestCorners(n)
		return nil
	}
	heap.Push(&s.expand, n)
	return nil
}

// childSpan returns the number of leaf positions covered by a child.
// A cube of level n contains 8^(n-1) leaf positions, and a child is one
// level down, so each child covers an eighth of the parent's positions.
func (n *procNode) childSpan() uint64 {
	return n.span / 8
}

// createChildren creates the eight children of a non-empty internal node and
// requests their centre evaluations.  Creating them together is what lets
// independent subtrees fill a batch; the original recursive walk visits the
// same cubes in the same order.
func (s *procState) createChildren(n *procNode) {
	childSpan := n.childSpan()
	leaf := n.c.n-1 == 1
	offsets := mcProcessChildOffsets(1 << (n.c.n - 1))
	for i := 0; i < 8; i++ {
		child := &procNode{
			c:    cube{n.c.v.Add(offsets[i]), n.c.n - 1},
			lo:   n.lo + uint64(i)*childSpan,
			span: childSpan,
			leaf: leaf,
		}
		child.hi = child.lo + child.span - 1
		s.waitCenter(child)
	}
}

func (s *procState) waitCenter(n *procNode) {
	smp, done := s.requestSample(n.c.v.AddScalar(1<<(n.c.n-1)), procWaiter{n: n, corner: -1}, n.lo)
	n.center = smp
	if done {
		s.queueNode(n)
	}
}

// requestSample returns a sample for a grid coordinate, sharing an existing
// sample (pending or resolved) if there is one.  The second result reports
// whether the value is already known.
func (s *procState) requestSample(coord v3i.Vec, w procWaiter, order uint64) (*procSample, bool) {
	smp := s.samples[coord]
	if smp == nil {
		smp = &procSample{
			coord:   coord,
			pos:     s.params.origin.Add(conv.V3iToV3(coord).MulScalar(s.params.resolution)),
			order:   order * 8,
			waiters: []procWaiter{w},
		}
		s.samples[coord] = smp
		s.unresolved++
		heap.Push(&s.dispatch, dispatchItem{s: smp, key: smp.order})
		return smp, false
	}
	smp.waiters = append(smp.waiters, w)
	if smp.resolved {
		return smp, true
	}
	if order*8 < smp.order {
		smp.order = order * 8
		if !smp.dispatched {
			heap.Push(&s.dispatch, dispatchItem{s: smp, key: smp.order})
		}
	}
	return smp, false
}

// requestCorners asks for the eight corner samples of a non-empty leaf.
func (s *procState) requestCorners(n *procNode) {
	if n.cornersReq || n.done {
		return
	}
	n.cornersReq = true
	for i, off := range mcProcessCornerOffsets {
		w := procWaiter{n: n, corner: i}
		smp, done := s.requestSample(n.c.v.Add(off), w, n.lo)
		n.corners[i] = smp
		if done {
			n.cornerWait++
		}
	}
	s.tryFinishLeaf(n)
}

// tryFinishLeaf finishes a leaf once every corner sample is known.
func (s *procState) tryFinishLeaf(n *procNode) {
	if n.done || n.phase != 1 || n.cornerWait < 8 {
		return
	}
	s.finishLeaf(n)
}

// finishLeaf converts a resolved leaf into triangles and queues them for
// ordered retirement.
func (s *procState) finishLeaf(n *procNode) {
	var corners [8]v3.Vec
	var values [8]float64
	for i := range n.corners {
		corners[i] = n.corners[i].pos
		values[i] = n.corners[i].value
	}
	tris := mcToTriangles(corners, values, 0)
	n.done = true
	heap.Push(&s.ready, &leafResult{ord: n.lo, tris: tris})
	if s.ready.Len() > s.stats.PeakReadyLeaves {
		s.stats.PeakReadyLeaves = s.ready.Len()
	}
}

//-----------------------------------------------------------------------------

// dispatchSamples sends ready samples in as few requests as possible.
func (s *procState) dispatchSamples() bool {
	progress := false
	for len(s.pending) < s.cfg.MaxInFlight {
		batch := make([]*procSample, 0, s.cfg.BatchSize)
		for len(batch) < s.cfg.BatchSize && s.dispatch.Len() > 0 {
			it := heap.Pop(&s.dispatch).(dispatchItem)
			smp := it.s
			if smp.dispatched || smp.resolved || len(smp.waiters) == 0 || it.key != smp.order {
				continue
			}
			smp.dispatched = true
			batch = append(batch, smp)
		}
		if len(batch) == 0 {
			break
		}
		s.sendBatch(batch)
		progress = true
	}
	return progress
}

func (s *procState) sendBatch(batch []*procSample) {
	id := s.nextID
	s.nextID++
	b := &sendBatch{id: id, points: make([][3]float64, len(batch))}
	for i, smp := range batch {
		b.points[i] = [3]float64{smp.pos.X, smp.pos.Y, smp.pos.Z}
	}
	s.pending[id] = batch
	s.outbox = append(s.outbox, b)
}

// recordSent accounts for a request that has actually been written to the
// subprocess.
func (s *procState) recordSent(b *sendBatch) {
	s.stats.Batches++
	s.stats.Samples += len(b.points)
	s.inFlightPoints += len(b.points)
	if s.inFlightPoints > s.stats.PeakPendingSamples {
		s.stats.PeakPendingSamples = s.inFlightPoints
	}
}

// flushOutbox hands queued requests to the stdin writer.  The number of
// outstanding requests is bounded by MaxInFlight, and the writer goroutine
// is always able to drain them (it does not wait for responses), so this
// never blocks while the process is healthy.
func (s *procState) flushOutbox(ctx context.Context, sampler *processSampler) error {
	for len(s.outbox) > 0 {
		select {
		case s.sendCh <- s.outbox[0]:
			s.recordSent(s.outbox[0])
			s.outbox = s.outbox[1:]
		case err := <-sampler.errCh:
			return sampler.withTail(err)
		case <-ctx.Done():
			return ctx.Err()
		}
	}
	// No request is blocked on a full stdin pipe: the pipe can only fill
	// while the field process is not reading, and in that case the process
	// has been started and stdin writes complete as soon as the child reads.
	// Still, blocked writes are unrecoverable on cancellation, so the writer
	// goroutine reports them through errCh.
	return nil
}

//-----------------------------------------------------------------------------

// handleResponse resolves every sample of a completed request.
func (s *procState) handleResponse(id int, values []float64) error {
	batch, ok := s.pending[id]
	if !ok {
		return fmt.Errorf("field process: protocol: unknown or duplicate response id %d", id)
	}
	if len(values) != len(batch) {
		return fmt.Errorf("field process: protocol: response %d has %d values, expected %d", id, len(values), len(batch))
	}
	delete(s.pending, id)
	s.inFlightPoints -= len(batch)
	for i, smp := range batch {
		smp.resolved = true
		smp.value = values[i]
		s.unresolved--
		for _, w := range smp.waiters {
			if w.corner < 0 {
				w.n.center = smp
				s.queueNode(w.n)
			} else {
				w.n.corners[w.corner] = smp
				w.n.cornerWait++
				s.tryFinishLeaf(w.n)
			}
		}
		// The value stays available for nodes that share this grid point,
		// but the waiters (and the nodes they reference) can be released.
		smp.waiters = nil
	}
	return nil
}

func (s *procState) queueNode(n *procNode) {
	if n.queued {
		return
	}
	n.queued = true
	heap.Push(&s.dirty, n)
}

//-----------------------------------------------------------------------------
// Heap implementations

type rangeItem struct{ lo, hi uint64 }

type rangeHeap []rangeItem

func (h rangeHeap) Len() int            { return len(h) }
func (h rangeHeap) Less(i, j int) bool  { return h[i].lo < h[j].lo }
func (h rangeHeap) Swap(i, j int)       { h[i], h[j] = h[j], h[i] }
func (h *rangeHeap) Push(x interface{}) { *h = append(*h, x.(rangeItem)) }
func (h *rangeHeap) Pop() interface{} {
	old := *h
	n := len(old)
	x := old[n-1]
	*h = old[:n-1]
	return x
}

type leafResult struct {
	ord  uint64
	tris []*sdf.Triangle3
}

type readyHeap []*leafResult

func (h readyHeap) Len() int            { return len(h) }
func (h readyHeap) Less(i, j int) bool  { return h[i].ord < h[j].ord }
func (h readyHeap) Swap(i, j int)       { h[i], h[j] = h[j], h[i] }
func (h *readyHeap) Push(x interface{}) { *h = append(*h, x.(*leafResult)) }
func (h *readyHeap) Pop() interface{} {
	old := *h
	n := len(old)
	x := old[n-1]
	*h = old[:n-1]
	return x
}

type parkedHeap []*procNode

func (h parkedHeap) Len() int           { return len(h) }
func (h parkedHeap) Less(i, j int) bool { return h[i].lo < h[j].lo }
func (h parkedHeap) Swap(i, j int)      { h[i], h[j] = h[j], h[i] }
func (h *parkedHeap) Push(x interface{}) {
	*h = append(*h, x.(*procNode))
}
func (h *parkedHeap) Pop() interface{} {
	old := *h
	n := len(old)
	x := old[n-1]
	*h = old[:n-1]
	return x
}

type dirtyHeap []*procNode

func (h dirtyHeap) Len() int           { return len(h) }
func (h dirtyHeap) Less(i, j int) bool { return h[i].lo < h[j].lo }
func (h dirtyHeap) Swap(i, j int)      { h[i], h[j] = h[j], h[i] }
func (h *dirtyHeap) Push(x interface{}) {
	*h = append(*h, x.(*procNode))
}
func (h *dirtyHeap) Pop() interface{} {
	old := *h
	n := len(old)
	x := old[n-1]
	*h = old[:n-1]
	return x
}

type expandHeap []*procNode

func (h expandHeap) Len() int           { return len(h) }
func (h expandHeap) Less(i, j int) bool { return h[i].lo < h[j].lo }
func (h expandHeap) Swap(i, j int)      { h[i], h[j] = h[j], h[i] }
func (h *expandHeap) Push(x interface{}) {
	*h = append(*h, x.(*procNode))
}
func (h *expandHeap) Pop() interface{} {
	old := *h
	n := len(old)
	x := old[n-1]
	*h = old[:n-1]
	return x
}

type dispatchItem struct {
	s   *procSample
	key uint64
}

type dispatchHeap []dispatchItem

func (h dispatchHeap) Len() int           { return len(h) }
func (h dispatchHeap) Less(i, j int) bool { return h[i].key < h[j].key }
func (h dispatchHeap) Swap(i, j int)      { h[i], h[j] = h[j], h[i] }
func (h *dispatchHeap) Push(x interface{}) {
	*h = append(*h, x.(dispatchItem))
}
func (h *dispatchHeap) Pop() interface{} {
	old := *h
	n := len(old)
	x := old[n-1]
	*h = old[:n-1]
	return x
}

//-----------------------------------------------------------------------------

// mcProcessCornerOffsets are the leaf corner offsets used by the original
// octree implementation (the level-0 cube is at half resolution).
var mcProcessCornerOffsets = [8]v3i.Vec{
	{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {0, 2, 0},
	{0, 0, 2}, {2, 0, 2}, {2, 2, 2}, {0, 2, 2},
}

// mcProcessChildOffsets returns the child cube offsets in the same DFS order
// used by the original octree implementation.
func mcProcessChildOffsets(s int) [8]v3i.Vec {
	return [8]v3i.Vec{
		{0, 0, 0}, {s, 0, 0}, {s, s, 0}, {0, s, 0},
		{0, 0, s}, {s, 0, s}, {s, s, s}, {0, s, s},
	}
}

//-----------------------------------------------------------------------------
