package model3d

import (
	"bufio"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"os/exec"
	"sort"
	"sync"

	"github.com/unixpickle/essentials"
)

// DCProcessStats summarizes one MeshProcess call.
type DCProcessStats struct {
	// Slabs is the number of z-axis slab jobs dispatched.
	Slabs int

	// Retries is the number of global must-clip retry
	// rounds performed.
	Retries int

	// PeakActiveSlabs is the largest number of worker
	// processes that were running at the same time.
	PeakActiveSlabs int

	// PIDs are the worker process IDs, sorted ascending and
	// deduplicated.
	PIDs []int
}

// Wire message types exchanged by the parent process and
// the worker processes. Every frame is:
//
//	1 byte  type
//	4 bytes big-endian uint32 payload length
//	N bytes payload (type-specific JSON)
//
// Protocol frames only ever travel over stdout; stderr is
// drained separately and can never be parsed as a response.
const (
	dcMsgHello   byte = 1
	dcMsgRequest byte = 2
	dcMsgResult  byte = 3
	dcMsgError   byte = 4
	dcMsgReady   byte = 5
)

// dcMaxFramePayload bounds the accepted frame size so a
// corrupted length header cannot trigger a huge allocation.
const dcMaxFramePayload = 256 << 20

type dcHello struct {
	Config dcConfig `json:"config"`
	Field  []byte   `json:"field"`
}

type dcConfig struct {
	Min                  Coord3D                    `json:"min"`
	Max                  Coord3D                    `json:"max"`
	Delta                float64                    `json:"delta"`
	NoJitter             bool                       `json:"no_jitter"`
	MaxGos               int                        `json:"max_gos"`
	BufferSize           int                        `json:"buffer_size"`
	Repair               bool                       `json:"repair"`
	Clip                 bool                       `json:"clip"`
	CubeMargin           float64                    `json:"cube_margin"`
	RepairEpsilon        float64                    `json:"repair_epsilon"`
	SingularValueEpsilon float64                    `json:"singular_value_epsilon"`
	L2Penalty            float64                    `json:"l2_penalty"`
	TriangleMode         DualContouringTriangleMode `json:"triangle_mode"`

	// SolidSurfaceEstimator tuning. The parent owns these
	// parameters; the factory only supplies the Solid.
	BisectCount         int     `json:"bisect_count"`
	NormalSamples       int     `json:"normal_samples"`
	NormalBisectEpsilon float64 `json:"normal_bisect_epsilon"`
	NormalNoiseEpsilon  float64 `json:"normal_noise_epsilon"`

	GridXs []float64 `json:"grid_xs"`
	GridYs []float64 `json:"grid_ys"`
	GridZs []float64 `json:"grid_zs"`
}

type dcRequest struct {
	Slab     int       `json:"slab"`
	Z0       int       `json:"z0"`
	Z1       int       `json:"z1"`
	SlabRows int       `json:"slab_rows"`
	MustClip []Coord3D `json:"must_clip"`
}

type dcResult struct {
	// Slab identifies the slab that produced the result.
	Slab int `json:"slab"`

	// CubePositions maps the global cube identity to the
	// solved vertex position of every cube owned by the
	// slab. Halo copies are never reported, so the parent
	// sees each global cube exactly once.
	CubePositions map[int]Coord3D `json:"cube_positions"`

	// Triangles are the faces generated from grid edges
	// owned by the slab.
	Triangles []dcTriangle `json:"triangles"`
}

type dcTriangle struct {
	P0 Coord3D `json:"p0"`
	P1 Coord3D `json:"p1"`
	P2 Coord3D `json:"p2"`
}

type dcReady struct {
	Min Coord3D `json:"min"`
	Max Coord3D `json:"max"`
}

type dcError struct {
	Message string `json:"message"`
}

// dcFrameReader reads length-prefixed binary frames.
type dcFrameReader struct {
	r   *bufio.Reader
	buf []byte
}

func newDcFrameReader(r io.Reader) *dcFrameReader {
	return &dcFrameReader{r: bufio.NewReaderSize(r, 1<<20)}
}

func (f *dcFrameReader) Read() (byte, []byte, error) {
	var header [5]byte
	if _, err := io.ReadFull(f.r, header[:]); err != nil {
		return 0, nil, err
	}
	msgType := header[0]
	n := binary.BigEndian.Uint32(header[1:])
	if uint64(n) > dcMaxFramePayload {
		return 0, nil, fmt.Errorf("dc process: frame too large (%d bytes)", n)
	}
	if cap(f.buf) < int(n) {
		f.buf = make([]byte, n)
	}
	payload := f.buf[:n]
	if _, err := io.ReadFull(f.r, payload); err != nil {
		return 0, nil, err
	}
	f.buf = payload
	return msgType, payload, nil
}

func dcWriteFrame(w io.Writer, msgType byte, payload []byte) error {
	if len(payload) > math.MaxUint32 {
		return fmt.Errorf("dc process: frame too large (%d bytes)", len(payload))
	}
	var header [5]byte
	header[0] = msgType
	binary.BigEndian.PutUint32(header[1:], uint32(len(payload)))
	if _, err := w.Write(header[:]); err != nil {
		return err
	}
	if len(payload) > 0 {
		if _, err := w.Write(payload); err != nil {
			return err
		}
	}
	return nil
}

func dcWriteJSON(w io.Writer, msgType byte, v interface{}) error {
	payload, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return dcWriteFrame(w, msgType, payload)
}

// dcBuildFrame serializes one complete framed message.
func dcBuildFrame(msgType byte, payload []byte) ([]byte, error) {
	if len(payload) > math.MaxUint32 {
		return nil, fmt.Errorf("dc process: frame too large (%d bytes)", len(payload))
	}
	frame := make([]byte, 5+len(payload))
	frame[0] = msgType
	binary.BigEndian.PutUint32(frame[1:5], uint32(len(payload)))
	copy(frame[5:], payload)
	return frame, nil
}

// MeshProcess runs the dual contouring extraction across
// multiple worker processes.
//
// command is the fully-specified command line of a worker
// program (see ServeDualContourWorker), field is an opaque
// description handed to the worker's factory, workers
// limits the number of concurrently running processes and
// slabRows is the number of global z-axis cube layers owned
// by each slab.
//
// The parent process only reads the Solid's bounds; all
// geometry sampling, Hermite data and per-cube QEF solves
// happen inside the workers. Repair of singular edges and
// vertices is performed globally in the parent once every
// slab has reported its unrepaired faces.
func (d *DualContouring) MeshProcess(ctx context.Context, command []string, field []byte,
	workers, slabRows int) (mesh *Mesh, err error) {
	// The callback runs exactly once before every return,
	// including validation and startup failures.
	stats := &DCProcessStats{}
	defer func() {
		d.deliverStats(*stats)
	}()

	if len(command) == 0 {
		return nil, errors.New("dc process: empty worker command")
	}
	if workers <= 0 {
		return nil, fmt.Errorf("dc process: workers must be positive, got %d", workers)
	}
	if slabRows <= 0 {
		return nil, fmt.Errorf("dc process: slabRows must be positive, got %d", slabRows)
	}
	if d.S.RandomSearchNormals {
		return nil, errors.New("dc process: RandomSearchNormals is not supported across processes")
	}
	validateBounds(d.S.Solid)
	if ctx == nil {
		ctx = context.Background()
	}

	xs, ys, zs := dcGridCoordinates(d.S.Solid.Min(), d.S.Solid.Max(), d.Delta, d.NoJitter)
	numCubeLayers := len(zs) - 1
	if numCubeLayers < 2 {
		panic("invalid number of z values")
	}

	// Slabs own contiguous ranges of global cube layers.
	var ranges [][2]int
	for z0 := 0; z0 < numCubeLayers; z0 += slabRows {
		z1 := essentials.MinInt(z0+slabRows, numCubeLayers)
		ranges = append(ranges, [2]int{z0, z1})
	}
	stats.Slabs = len(ranges)

	cfg := d.processConfig(d.S.Solid.Min(), d.S.Solid.Max(), xs, ys, zs)
	hello, err := json.Marshal(dcHello{
		Config: cfg,
		Field:  append([]byte(nil), field...),
	})
	if err != nil {
		return nil, err
	}

	helloFrame, err := dcBuildFrame(dcMsgHello, hello)
	if err != nil {
		return nil, err
	}

	p := &dcProcessParent{
		ctx:         ctx,
		command:     command,
		hello:       helloFrame,
		cfg:         cfg,
		expectedMin: d.S.Solid.Min(),
		expectedMax: d.S.Solid.Max(),
		ranges:      ranges,
		workers:     workers,
		dc:          d,
	}
	mesh, finalStats, err := p.run()
	*stats = finalStats
	if err != nil {
		return nil, err
	}
	return mesh, nil
}

// deliverStats invokes the optional stats callback with a
// value that never aliases internal slices.
func (d *DualContouring) deliverStats(stats DCProcessStats) {
	if d.ProcessStats == nil {
		return
	}
	stats.PIDs = append([]int(nil), stats.PIDs...)
	d.ProcessStats(stats)
}

func (d *DualContouring) processConfig(min, max Coord3D, xs, ys, zs []float64) dcConfig {
	return dcConfig{
		Min:                  min,
		Max:                  max,
		Delta:                d.Delta,
		NoJitter:             d.NoJitter,
		MaxGos:               d.MaxGos,
		BufferSize:           d.BufferSize,
		Repair:               d.Repair,
		Clip:                 d.Clip,
		CubeMargin:           d.CubeMargin,
		RepairEpsilon:        d.RepairEpsilon,
		SingularValueEpsilon: d.SingularValueEpsilon,
		L2Penalty:            d.L2Penalty,
		TriangleMode:         d.TriangleMode,
		BisectCount:          d.S.BisectCount,
		NormalSamples:        d.S.NormalSamples,
		NormalBisectEpsilon:  d.S.NormalBisectEpsilon,
		NormalNoiseEpsilon:   d.S.NormalNoiseEpsilon,
		GridXs:               xs,
		GridYs:               ys,
		GridZs:               zs,
	}
}

// dcProcessParent owns the lifecycle of every worker
// process for a single MeshProcess call. No state is shared
// between independent calls.
type dcProcessParent struct {
	ctx         context.Context
	command     []string
	hello       []byte
	cfg         dcConfig
	expectedMin Coord3D
	expectedMax Coord3D
	ranges      [][2]int
	workers     int
	dc          *DualContouring

	lock       sync.Mutex
	pidSet     map[int]bool
	active     int
	peakActive int
	cancel     context.CancelFunc
}

type dcWorkerHandle struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	stdout io.Reader
	done   chan struct{}
	closed bool
}

func (p *dcProcessParent) run() (_ *Mesh, stats DCProcessStats, err error) {
	stats.Slabs = len(p.ranges)
	p.pidSet = map[int]bool{}

	ctx, cancel := context.WithCancel(p.ctx)
	p.cancel = cancel
	defer cancel()

	// Global must-clip coordinates, accumulated across
	// retry rounds exactly like mustClipMap in mesh().
	mustClip := NewCoordMap[struct{}]()
	var roundResults []dcResult

	for {
		results, runErr := p.runRound(ctx, mustClip)
		if runErr != nil {
			// Report the partial lifecycle statistics even on
			// failure: every launched worker has already been
			// reaped by runRound's handle.close().
			stats.PeakActiveSlabs = p.peakActive
			stats.PIDs = p.sortedPIDs()
			if errors.Is(runErr, context.Canceled) && p.ctx.Err() != nil {
				return nil, stats, p.ctx.Err()
			}
			return nil, stats, runErr
		}
		roundResults = results

		if !p.cfg.Repair || p.cfg.Clip {
			break
		}

		// Apply the global used-coordinate semantics of
		// populateCubes: when two distinct global cubes solve
		// to the same coordinate, both must be clipped and
		// the whole extraction retried. Halo copies collapse
		// because only owned cubes report positions, keyed by
		// global cube identity.
		usedCoords := NewCoordToNumber[int]()
		for i := range results {
			for _, pos := range results[i].CubePositions {
				usedCoords.Add(pos, 1)
			}
		}
		grew := false
		usedCoords.Range(func(c Coord3D, count int) bool {
			if count > 1 {
				if _, ok := mustClip.Load(c); !ok {
					mustClip.Store(c, struct{}{})
					grew = true
				}
			}
			return true
		})
		if !grew {
			break
		}
		stats.Retries++
	}

	mesh := NewMesh()
	for i := range roundResults {
		for _, tri := range roundResults[i].Triangles {
			mesh.Add(&Triangle{tri.P0, tri.P1, tri.P2})
		}
	}

	if p.cfg.Repair {
		// Constraint-only layout: grid axes for
		// PointCubeMinMax, without any cached corners or
		// Hermite data in the parent.
		layout := newDcConstraintLayout(p.cfg.GridXs, p.cfg.GridYs, p.cfg.GridZs)
		p.dc.meshRepair(mesh, layout)
	}

	stats.PeakActiveSlabs = p.peakActive
	stats.PIDs = p.sortedPIDs()
	return mesh, stats, nil
}

// runRound dispatches every slab concurrently, bounded by
// the worker limit. Responses may arrive in any order; the
// returned slice is indexed by slab number.
func (p *dcProcessParent) runRound(ctx context.Context,
	mustClip *CoordMap[struct{}]) ([]dcResult, error) {
	results := make([]dcResult, len(p.ranges))

	// Token bucket guarantees at most workers active slabs.
	tokens := make(chan struct{}, p.workers)
	for i := 0; i < p.workers; i++ {
		tokens <- struct{}{}
	}

	var wg sync.WaitGroup
	var errMu sync.Mutex
	var firstErr error
	setErr := func(e error) {
		errMu.Lock()
		if firstErr == nil && e != nil {
			firstErr = e
			p.cancel()
		}
		errMu.Unlock()
	}

	for slab := range p.ranges {
		select {
		case <-tokens:
		case <-ctx.Done():
		}
		if ctx.Err() != nil {
			break
		}
		wg.Add(1)
		go func(slab int) {
			defer wg.Done()
			defer func() { tokens <- struct{}{} }()
			req := dcRequest{
				Slab:     slab,
				Z0:       p.ranges[slab][0],
				Z1:       p.ranges[slab][1],
				SlabRows: p.ranges[slab][1] - p.ranges[slab][0],
			}
			mustClip.KeyRange(func(c Coord3D) bool {
				req.MustClip = append(req.MustClip, c)
				return true
			})
			res, e := p.runSlab(ctx, req)
			if e != nil {
				setErr(e)
				return
			}
			results[slab] = *res
		}(slab)
	}
	wg.Wait()

	if firstErr != nil {
		return nil, firstErr
	}
	return results, nil
}

func (p *dcProcessParent) runSlab(ctx context.Context, req dcRequest) (*dcResult, error) {
	handle, err := p.startWorker(ctx)
	if err != nil {
		return nil, err
	}
	defer handle.close()

	reader := newDcFrameReader(handle.stdout)
	writer := bufio.NewWriter(handle.stdin)

	if _, err := handle.stdin.Write(p.hello); err != nil {
		return nil, fmt.Errorf("dc process: write hello: %w", err)
	}
	msgType, payload, err := reader.Read()
	if err != nil {
		return nil, fmt.Errorf("dc process: read ready: %w", err)
	}
	if msgType != dcMsgReady {
		return nil, p.unexpectedMsg(msgType, payload, "ready")
	}
	var ready dcReady
	if err := json.Unmarshal(payload, &ready); err != nil {
		return nil, fmt.Errorf("dc process: decode ready: %w", err)
	}
	if ready.Min != p.expectedMin || ready.Max != p.expectedMax {
		return nil, fmt.Errorf(
			"dc process: worker field bounds %v..%v do not match parent bounds %v..%v",
			ready.Min, ready.Max, p.expectedMin, p.expectedMax)
	}

	if err := dcWriteJSON(writer, dcMsgRequest, req); err != nil {
		return nil, fmt.Errorf("dc process: write request: %w", err)
	}
	if err := writer.Flush(); err != nil {
		return nil, fmt.Errorf("dc process: flush request: %w", err)
	}

	msgType, payload, err = reader.Read()
	if err != nil {
		return nil, fmt.Errorf("dc process: read result: %w", err)
	}
	switch msgType {
	case dcMsgResult:
		var res dcResult
		if err := json.Unmarshal(payload, &res); err != nil {
			return nil, fmt.Errorf("dc process: decode result: %w", err)
		}
		if res.Slab != req.Slab {
			return nil, fmt.Errorf("dc process: worker returned slab %d, expected %d",
				res.Slab, req.Slab)
		}
		return &res, nil
	case dcMsgError:
		var workerErr dcError
		_ = json.Unmarshal(payload, &workerErr)
		return nil, fmt.Errorf("dc process: worker error: %s", workerErr.Message)
	default:
		return nil, p.unexpectedMsg(msgType, payload, "result")
	}
}

func (p *dcProcessParent) unexpectedMsg(msgType byte, payload []byte, want string) error {
	if msgType == dcMsgError {
		var workerErr dcError
		_ = json.Unmarshal(payload, &workerErr)
		return fmt.Errorf("dc process: worker error while waiting for %s: %s",
			want, workerErr.Message)
	}
	return fmt.Errorf("dc process: unexpected message type %d while waiting for %s",
		msgType, want)
}

func (p *dcProcessParent) startWorker(ctx context.Context) (*dcWorkerHandle, error) {
	cmd := exec.CommandContext(ctx, p.command[0], p.command[1:]...)
	cmd.Env = os.Environ()
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		_ = stdin.Close()
		return nil, err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		_ = stdin.Close()
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		_ = stdin.Close()
		return nil, fmt.Errorf("dc process: start worker: %w", err)
	}

	pid := cmd.Process.Pid
	p.lock.Lock()
	p.pidSet[pid] = true
	p.active++
	if p.active > p.peakActive {
		p.peakActive = p.active
	}
	p.lock.Unlock()

	// stderr is continuously drained for the lifetime of the
	// process and never shares a stream with the protocol.
	stderrDone := make(chan struct{})
	go func() {
		defer close(stderrDone)
		_, _ = io.Copy(os.Stderr, stderr)
	}()

	done := make(chan struct{})
	h := &dcWorkerHandle{cmd: cmd, stdin: stdin, stdout: stdout, done: done}
	go func() {
		_ = cmd.Wait()
		<-stderrDone
		p.lock.Lock()
		p.active--
		p.lock.Unlock()
		close(done)
	}()

	return h, nil
}

// close shuts the worker pipes and blocks until the process
// has exited and been reaped.
func (h *dcWorkerHandle) close() {
	if h.closed {
		return
	}
	h.closed = true
	_ = h.stdin.Close()
	<-h.done
}

func (p *dcProcessParent) sortedPIDs() []int {
	pids := make([]int, 0, len(p.pidSet))
	for pid := range p.pidSet {
		pids = append(pids, pid)
	}
	sort.Ints(pids)
	return pids
}

// newDcConstraintLayout builds a layout carrying only the
// global grid axes. The singular-repair constraints need
// nothing more than PointCubeMinMax, so the parent never
// allocates or caches global corners, edges or Hermite data.
func newDcConstraintLayout(xs, ys, zs []float64) *dcCubeLayout {
	return &dcCubeLayout{
		Xs:      xs,
		Ys:      ys,
		Zs:      zs,
		BufRows: len(zs),
	}
}

// ServeDualContourWorker runs the worker side of the
// MeshProcess protocol on the given streams.
//
// factory rebuilds the Solid from the opaque field bytes
// supplied to MeshProcess; the worker validates that the
// rebuilt Solid's bounds agree with the parent process
// before any geometry is sampled.
func ServeDualContourWorker(input io.Reader, output io.Writer,
	factory func([]byte) (SolidSurfaceEstimator, error)) error {
	reader := newDcFrameReader(input)
	writer := bufio.NewWriter(output)

	msgType, payload, err := reader.Read()
	if err != nil {
		return fmt.Errorf("dc worker: read hello: %w", err)
	}
	if msgType != dcMsgHello {
		return fmt.Errorf("dc worker: expected hello frame, got type %d", msgType)
	}
	var hello dcHello
	if err := json.Unmarshal(payload, &hello); err != nil {
		return fmt.Errorf("dc worker: decode hello: %w", err)
	}
	cfg := hello.Config
	if err := validateWorkerGrid(cfg); err != nil {
		return err
	}

	estimator, err := factory(hello.Field)
	if err != nil {
		return workerFail(writer, fmt.Errorf("factory: %w", err))
	}
	if estimator.RandomSearchNormals {
		return workerFail(writer, errors.New(
			"RandomSearchNormals is not supported across processes"))
	}
	if estimator.Solid.Min() != cfg.Min || estimator.Solid.Max() != cfg.Max {
		return workerFail(writer, fmt.Errorf(
			"rebuilt field bounds %v..%v do not match configured bounds %v..%v",
			estimator.Solid.Min(), estimator.Solid.Max(), cfg.Min, cfg.Max))
	}

	ready, err := json.Marshal(dcReady{Min: estimator.Solid.Min(), Max: estimator.Solid.Max()})
	if err != nil {
		return err
	}
	if err := dcWriteFrame(writer, dcMsgReady, ready); err != nil {
		return err
	}
	if err := writer.Flush(); err != nil {
		return err
	}

	d := workerDualContouring(cfg, estimator)
	for {
		msgType, payload, err := reader.Read()
		if err != nil {
			if errors.Is(err, io.EOF) {
				return nil
			}
			return fmt.Errorf("dc worker: read request: %w", err)
		}
		if msgType != dcMsgRequest {
			return fmt.Errorf("dc worker: expected request frame, got type %d", msgType)
		}
		var req dcRequest
		if err := json.Unmarshal(payload, &req); err != nil {
			return workerFail(writer, fmt.Errorf("decode request: %w", err))
		}
		res, err := d.extractSlab(cfg, req)
		if err != nil {
			if failErr := workerFail(writer, err); failErr != nil {
				return failErr
			}
			if err := writer.Flush(); err != nil {
				return err
			}
			continue
		}
		if err := dcWriteJSON(writer, dcMsgResult, res); err != nil {
			return err
		}
		if err := writer.Flush(); err != nil {
			return err
		}
	}
}

func validateWorkerGrid(cfg dcConfig) error {
	if len(cfg.GridXs) < 2 || len(cfg.GridYs) < 2 || len(cfg.GridZs) < 3 {
		return errors.New("dc worker: invalid global grid dimensions")
	}
	if cfg.Delta <= 0 {
		return errors.New("dc worker: invalid grid delta")
	}
	return nil
}

func workerFail(writer *bufio.Writer, err error) error {
	payload, marshalErr := json.Marshal(dcError{Message: err.Error()})
	if marshalErr != nil {
		return marshalErr
	}
	if writeErr := dcWriteFrame(writer, dcMsgError, payload); writeErr != nil {
		return writeErr
	}
	return writer.Flush()
}

func workerDualContouring(cfg dcConfig, estimator SolidSurfaceEstimator) *DualContouring {
	// Boundary point and normal parameters follow the
	// parent's configuration; the factory only provides the
	// Solid itself.
	estimator.BisectCount = cfg.BisectCount
	estimator.NormalSamples = cfg.NormalSamples
	estimator.NormalBisectEpsilon = cfg.NormalBisectEpsilon
	estimator.NormalNoiseEpsilon = cfg.NormalNoiseEpsilon
	estimator.RandomSearchNormals = false
	return &DualContouring{
		S:                    estimator,
		Delta:                cfg.Delta,
		NoJitter:             cfg.NoJitter,
		MaxGos:               cfg.MaxGos,
		BufferSize:           cfg.BufferSize,
		Repair:               cfg.Repair,
		Clip:                 cfg.Clip,
		CubeMargin:           cfg.CubeMargin,
		RepairEpsilon:        cfg.RepairEpsilon,
		SingularValueEpsilon: cfg.SingularValueEpsilon,
		L2Penalty:            cfg.L2Penalty,
		TriangleMode:         cfg.TriangleMode,
	}
}

// extractSlab runs the shared populateCorners,
// populateEdges, populateCubes and appendMesh passes over a
// windowed layout covering only this slab's cube layers
// plus the halo layers needed to resolve boundary edges.
func (d *DualContouring) extractSlab(cfg dcConfig, req dcRequest) (*dcResult, error) {
	if req.Z0 < 0 || req.Z1 <= req.Z0 || req.Z1 > len(cfg.GridZs)-1 {
		return nil, fmt.Errorf("dc worker: invalid slab range [%d,%d)", req.Z0, req.Z1)
	}
	if req.SlabRows != req.Z1-req.Z0 {
		return nil, fmt.Errorf("dc worker: slab row count %d does not match range [%d,%d)",
			req.SlabRows, req.Z0, req.Z1)
	}

	mustClipMap := NewCoordMap[struct{}]()
	for _, c := range req.MustClip {
		mustClipMap.Store(c, struct{}{})
	}

	// Halo layers on both sides make every edge of every
	// owned cube resolvable; halo geometry is recomputed but
	// never emitted as faces. Each global cube still appears
	// exactly once inside this windowed layout.
	zLo := essentials.MaxInt(0, req.Z0-1)
	zHi := essentials.MinInt(len(cfg.GridZs)-1, req.Z1+1)
	windowCornerRows := zHi - zLo + 1

	// Keep the BufferSize soft limit and the minimum-four-
	// layer semantics, applied to this slab's window rather
	// than to the full global grid. The window always spans
	// at least three corner layers, so buffer shifts can
	// never stall on a zero-row advance.
	defaultRows := dcDefaultBufferRows(cfg.BufferSize, len(cfg.GridXs), len(cfg.GridYs),
		len(cfg.GridZs))
	bufRows := essentials.MinInt(windowCornerRows, defaultRows)

	layout := newDcCubeLayoutWithRows(cfg.GridXs, cfg.GridYs, cfg.GridZs[zLo:zHi+1], bufRows)
	mesh := NewMesh()
	res := &dcResult{
		Slab:          req.Slab,
		CubePositions: map[int]Coord3D{},
	}
	nx := len(cfg.GridXs) - 1
	ny := len(cfg.GridYs) - 1
	// A nil used-coordinate table disables local conflict
	// discovery: duplicate-coordinate detection is the
	// parent's global responsibility.
	for {
		d.populateCorners(layout)
		d.populateEdges(layout, nil)
		d.populateCubes(layout, mustClipMap, nil)
		d.appendOwnedMesh(layout, mesh, req.Z0, req.Z1, zLo)
		d.recordOwnedCubes(layout, res, zLo, req.Z0, req.Z1, nx, ny)
		if layout.Remaining() == 0 {
			break
		}
		layout.Shift()
	}

	mesh.Iterate(func(t *Triangle) {
		res.Triangles = append(res.Triangles, dcTriangle{P0: t[0], P1: t[1], P2: t[2]})
	})
	return res, nil
}

// recordOwnedCubes stores the solved vertex position of
// every active cube owned by the slab under its global cube
// identity. Inactive cubes carry no solved position and are
// excluded so they cannot create false duplicate
// coordinates in the parent. Halo cubes and buffer rows not
// yet reached are skipped. Repeated visits after a buffer
// shift overwrite with the identical position.
func (d *DualContouring) recordOwnedCubes(layout *dcCubeLayout, res *dcResult,
	zLo, z0, z1, nx, ny int) {
	for localIdx, cube := range layout.Cubes {
		if !cube.Populated || !layout.CubeActive(dcCubeIdx(localIdx)) {
			continue
		}
		x, y, localZ := layout.cubeCoord(dcCubeIdx(localIdx))
		globalZ := zLo + layout.ZOffset + localZ
		if globalZ < z0 || globalZ >= z1 {
			continue
		}
		globalID := x + (y+globalZ*ny)*nx
		res.CubePositions[globalID] = cube.VertexPosition
	}
}

// appendOwnedMesh is the slab analogue of appendMesh: it
// emits exactly the quads whose global grid edge is owned
// by the slab. Ownership is defined by the edge's global
// identity, never by floating-point coordinate welding.
func (d *DualContouring) appendOwnedMesh(layout *dcCubeLayout, mesh *Mesh,
	z0, z1, zLo int) {
	numEdges := layout.UsableEdges()
	essentials.ReduceConcurrentMap(d.MaxGos, numEdges, func() (func(int), func()) {
		subMesh := NewMesh()
		addEdge := func(idx int) {
			i := dcEdgeIdx(idx)
			e := layout.Edge(i)
			if e.Triangulated || !e.Active {
				return
			}
			if !d.ownsEdge(layout, i, z0, z1, zLo) {
				return
			}
			e.Triangulated = true
			var vs [4]Coord3D
			for j, c := range layout.EdgeCubes(i) {
				if c < 0 {
					panic("solid is true outside of bounds")
				}
				vs[j] = layout.Cube(c).VertexPosition
			}

			// Same orientation logic as appendMesh.
			cs := layout.EdgeCorners(i)
			if layout.Corner(cs[0]).Value {
				vs[0], vs[1], vs[2], vs[3] = vs[3], vs[2], vs[1], vs[0]
			}
			t1, t2 := d.triangulateQuad(vs)
			subMesh.Add(t1)
			subMesh.Add(t2)
		}
		reduce := func() {
			mesh.AddMesh(subMesh)
		}
		return addEdge, reduce
	})
}

// ownsEdge reports whether the slab owns the global grid
// edge i. x/y edges at corner layer gz touch cube layers
// gz-1 and gz and are owned by the lower one (clamped to
// layer 0); z edges at corner layer gz touch cube layer gz
// and are owned by it. Every active edge therefore has
// exactly one owning slab.
func (d *DualContouring) ownsEdge(layout *dcCubeLayout, i dcEdgeIdx, z0, z1, zLo int) bool {
	xCount, yCount, zCount := layout.edgeCounts()
	stride := xCount + yCount + zCount
	localZ := int(i) / stride
	axisIdx := int(i) % stride
	globalZ := zLo + layout.ZOffset + localZ

	var owner int
	switch {
	case axisIdx < xCount:
		owner = globalZ - 1
	case axisIdx < xCount+yCount:
		owner = globalZ - 1
	default:
		owner = globalZ
	}
	if owner < 0 {
		owner = 0
	}
	return owner >= z0 && owner < z1
}
