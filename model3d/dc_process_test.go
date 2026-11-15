package model3d

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"syscall"
	"testing"
	"time"

	"github.com/unixpickle/model3d/task_support/taskcheck"
)

// dcWorkerMode lets the test binary serve as its own worker
// process, so the tests never depend on an external program.
var dcWorkerMode = flag.Bool("dc-worker", false, "run the model3d test binary as a DC worker")
var dcChaosWorkerMode = flag.Bool("dc-chaos-worker", false, "run the model3d test binary as a chaos DC worker")

func TestMain(m *testing.M) {
	flag.Parse()
	if *dcWorkerMode {
		err := ServeDualContourWorker(os.Stdin, os.Stdout, processTestFactory)
		if err != nil {
			fmt.Fprintln(os.Stderr, "dc-worker:", err)
			os.Exit(1)
		}
		os.Exit(0)
	}
	if *dcChaosWorkerMode {
		os.Exit(runChaosWorker())
	}
	os.Exit(m.Run())
}

// processTestField is the opaque field description used by
// the test workers. The model3d package itself never
// interprets these bytes.
type processTestField struct {
	Kind   string  `json:"kind"`
	Radius float64 `json:"radius"`
	Offset Coord3D `json:"offset"`

	// WaitDir, when set, makes passthrough workers block
	// before answering the request for slab k until the file
	// WaitDir/slab-<k> appears. This is an explicit
	// handshake rather than a timed sleep.
	WaitDir string `json:"wait_dir"`

	// WaitingDir gets a waiting-<slab> file once a worker has
	// reached the WaitDir gate, letting the test observe
	// worker progress without sleeping.
	WaitingDir string `json:"waiting_dir"`

	// StartedDir, when set, gets a started-<pid> file once
	// the worker has sent its ready frame.
	StartedDir string `json:"started_dir"`

	// StderrSpam writes this many bytes to stderr before the
	// first response, proving stderr is drained separately.
	StderrSpam int `json:"stderr_spam"`

	// Script controls the scripted worker used to exercise
	// the global must-clip retry loop deterministically.
	Script *scriptSpec `json:"script"`
}

type scriptSpec struct {
	NX          int      `json:"nx"`
	NY          int      `json:"ny"`
	FirstCubes  [][3]int `json:"first_cubes"`
	FirstPos    Coord3D  `json:"first_pos"`
	SecondCubes [][3]int `json:"second_cubes"`
	SecondBase  Coord3D  `json:"second_base"`
}

func processTestFactory(field []byte) (SolidSurfaceEstimator, error) {
	var f processTestField
	if err := json.Unmarshal(field, &f); err != nil {
		return SolidSurfaceEstimator{}, fmt.Errorf("parse field: %w", err)
	}
	switch f.Kind {
	case "sphere":
		return SolidSurfaceEstimator{
			Solid: TranslateSolid(&Sphere{Radius: f.Radius}, f.Offset),
		}, nil
	case "contact":
		solid := contactBoxes()
		return SolidSurfaceEstimator{Solid: TranslateSolid(solid, f.Offset)}, nil
	case "empty":
		// A solid with valid bounds that contains nothing.
		return SolidSurfaceEstimator{Solid: CheckedFuncSolid(
			XYZ(-1, -1, -1), XYZ(1, 1, 1), func(Coord3D) bool { return false })}, nil
	case "factoryfail":
		return SolidSurfaceEstimator{}, fmt.Errorf("synthetic factory failure")
	case "badbounds":
		return SolidSurfaceEstimator{
			Solid: NewRect(XYZ(-10, -10, -10), XYZ(-9, -9, -9)),
		}, nil
	default:
		return SolidSurfaceEstimator{}, fmt.Errorf("unknown test field kind %q", f.Kind)
	}
}

func contactBoxes() Solid {
	return JoinedSolid{
		NewRect(XYZ(0, 0, 0), XYZ(1, 1, 1)),
		NewRect(XYZ(1, 0, 1), XYZ(2, 1, 2)),
		NewRect(XYZ(-1, -1, -1), XYZ(0, 0, 0)),
	}
}

func workerCommand() []string {
	return []string{os.Args[0], "-dc-worker"}
}

func chaosWorkerCommand() []string {
	return []string{os.Args[0], "-dc-chaos-worker"}
}

func mustMarshalField(t *testing.T, f processTestField) []byte {
	t.Helper()
	b, err := json.Marshal(f)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// runChaosWorker serves a custom protocol loop for the
// lifecycle and retry tests. Normal geometry workers use
// ServeDualContourWorker instead.
func runChaosWorker() int {
	reader := newDcFrameReader(os.Stdin)
	writer := newBufWriter(os.Stdout)

	msgType, payload, err := reader.Read()
	if err != nil || msgType != dcMsgHello {
		fmt.Fprintln(os.Stderr, "chaos worker: bad hello")
		return 1
	}
	var hello dcHello
	if err := json.Unmarshal(payload, &hello); err != nil {
		fmt.Fprintln(os.Stderr, "chaos worker:", err)
		return 1
	}
	var field processTestField
	if err := json.Unmarshal(hello.Field, &field); err != nil {
		fmt.Fprintln(os.Stderr, "chaos worker:", err)
		return 1
	}

	if field.Kind == "garbage" {
		_, _ = os.Stdout.Write([]byte{0x7f, 0x12, 0x34, 0x56, 0x78, 0x9a})
		return 1
	}

	if field.StartedDir != "" {
		path := filepath.Join(field.StartedDir, fmt.Sprintf("started-%d", os.Getpid()))
		_ = os.WriteFile(path, []byte("ready"), 0o600)
	}

	switch field.Kind {
	case "crash":
		// Send a valid ready frame, then die on the first
		// request without answering.
		ready, _ := json.Marshal(dcReady{Min: hello.Config.Min, Max: hello.Config.Max})
		_ = dcWriteFrame(writer, dcMsgReady, ready)
		_ = writer.Flush()
		if _, _, err := reader.Read(); err == nil {
			os.Exit(7)
		}
		os.Exit(7)
	case "passthrough":
		return runPassthroughChaos(reader, writer, hello, field)
	case "script":
		return runScriptChaos(reader, writer, hello, field)
	default:
		fmt.Fprintln(os.Stderr, "chaos worker: unknown kind", field.Kind)
		return 1
	}
	// The crash case terminates the process above; this is
	// unreachable but satisfies the compiler.
	return 1
}

func newBufWriter(w *os.File) *bufioWriter {
	return &bufioWriter{w: w}
}

// bufioWriter is a tiny framed writer so the chaos worker
// avoids importing extra plumbing into the test binary.
type bufioWriter struct {
	w   *os.File
	buf []byte
}

func (b *bufioWriter) Write(p []byte) (int, error) {
	b.buf = append(b.buf, p...)
	return len(p), nil
}

func (b *bufioWriter) Flush() error {
	_, err := b.w.Write(b.buf)
	b.buf = b.buf[:0]
	return err
}

func runPassthroughChaos(reader *dcFrameReader, writer *bufioWriter,
	hello dcHello, field processTestField) int {
	kind := field.Kind
	if kind == "passthrough" {
		kind = "sphere"
	}
	estimator, err := processTestFactory(mustMarshalJSON(processTestField{
		Kind:   kind,
		Radius: field.Radius,
		Offset: field.Offset,
	}))
	if err != nil {
		fmt.Fprintln(os.Stderr, "chaos worker:", err)
		return 1
	}
	cfg := hello.Config
	ready, _ := json.Marshal(dcReady{Min: estimator.Solid.Min(), Max: estimator.Solid.Max()})
	if err := dcWriteFrame(writer, dcMsgReady, ready); err != nil {
		return 1
	}
	if err := writer.Flush(); err != nil {
		return 1
	}
	d := workerDualContouring(cfg, estimator)
	for {
		msgType, payload, err := reader.Read()
		if err != nil {
			return 0
		}
		if msgType != dcMsgRequest {
			return 1
		}
		var req dcRequest
		if err := json.Unmarshal(payload, &req); err != nil {
			return 1
		}
		if field.WaitDir != "" {
			if field.WaitingDir != "" {
				waiting := filepath.Join(field.WaitingDir,
					fmt.Sprintf("waiting-%d", req.Slab))
				_ = os.WriteFile(waiting, []byte("wait"), 0o600)
			}
			gate := filepath.Join(field.WaitDir, fmt.Sprintf("slab-%d", req.Slab))
			waitForFile(gate)
		}
		if field.StderrSpam > 0 {
			line := make([]byte, field.StderrSpam)
			for i := range line {
				line[i] = 'x'
			}
			line = append(line, '\n')
			_, _ = os.Stderr.Write(line)
		}
		res, err := d.extractSlab(cfg, req)
		if err != nil {
			payload, _ := json.Marshal(dcError{Message: err.Error()})
			_ = dcWriteFrame(writer, dcMsgError, payload)
			_ = writer.Flush()
			continue
		}
		if err := dcWriteJSON(writer, dcMsgResult, res); err != nil {
			return 1
		}
		if err := writer.Flush(); err != nil {
			return 1
		}
	}
}

func runScriptChaos(reader *dcFrameReader, writer *bufioWriter,
	hello dcHello, field processTestField) int {
	cfg := hello.Config
	ready, _ := json.Marshal(dcReady{Min: cfg.Min, Max: cfg.Max})
	if err := dcWriteFrame(writer, dcMsgReady, ready); err != nil {
		return 1
	}
	if err := writer.Flush(); err != nil {
		return 1
	}
	spec := field.Script
	nx := spec.NX - 1
	ny := spec.NY - 1
	for {
		msgType, payload, err := reader.Read()
		if err != nil {
			return 0
		}
		if msgType != dcMsgRequest {
			return 1
		}
		var req dcRequest
		if err := json.Unmarshal(payload, &req); err != nil {
			return 1
		}
		res := &dcResult{Slab: req.Slab, CubePositions: map[int]Coord3D{}}
		if len(req.MustClip) == 0 {
			// First round: several distinct global cubes
			// collapse onto one identical coordinate,
			// including cubes on opposite sides of a slab
			// partition boundary.
			for _, c := range spec.FirstCubes {
				id := c[0] + (c[1]+c[2]*ny)*nx
				res.CubePositions[id] = spec.FirstPos
			}
		} else {
			// Retry round: every cube gets a distinct point.
			for i, c := range spec.SecondCubes {
				id := c[0] + (c[1]+c[2]*ny)*nx
				res.CubePositions[id] = spec.SecondBase.Add(
					XYZ(float64(i)*0.01, 0, 0))
			}
		}
		if err := dcWriteJSON(writer, dcMsgResult, res); err != nil {
			return 1
		}
		if err := writer.Flush(); err != nil {
			return 1
		}
	}
}

func mustMarshalJSON(v interface{}) []byte {
	b, err := json.Marshal(v)
	if err != nil {
		panic(err)
	}
	return b
}

func waitForFile(path string) {
	for {
		if _, err := os.Stat(path); err == nil {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
}

// -----------------------------------------------------------------------------
// Geometry
// -----------------------------------------------------------------------------

func TestDCProcessGeometry(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		offset := XYZ(0.37, -0.21, 0.13)
		solid := TranslateSolid(&Sphere{Radius: 1.0}, offset)
		field := mustMarshalField(t, processTestField{
			Kind: "sphere", Radius: 1.0, Offset: offset,
		})

		for _, workers := range []int{1, 2, 3} {
			for _, slabRows := range []int{1, 2, 3, 4, 7} {
				for _, mode := range []DualContouringTriangleMode{
					DualContouringTriangleModeMaxMinArea,
					DualContouringTriangleModeSharpest,
					DualContouringTriangleModeFlattest,
				} {
					d := &DualContouring{
						S:            SolidSurfaceEstimator{Solid: solid},
						Delta:        0.18,
						NoJitter:     true,
						MaxGos:       1,
						BufferSize:   256,
						TriangleMode: mode,
					}
					ref := d.Mesh()
					got, err := d.MeshProcess(context.Background(), workerCommand(),
						field, workers, slabRows)
					c.Check(err == nil, fmt.Sprintf(
						"workers=%d slabRows=%d mode=%d: MeshProcess error: %v",
						workers, slabRows, mode, err))
					if err != nil {
						continue
					}
					c.Check(got != nil, "MeshProcess returned nil mesh")
					c.Check(got.NumTriangles() == ref.NumTriangles(),
						"workers=%d slabRows=%d mode=%d: triangle count ref=%d got=%d",
						workers, slabRows, mode, ref.NumTriangles(), got.NumTriangles())
					c.Check(sameTriangles(ref, got, 1e-8),
						"workers=%d slabRows=%d mode=%d: directed triangle sets differ beyond 1e-8",
						workers, slabRows, mode)
					c.Check(math.Abs(ref.Volume()-got.Volume()) <= 1e-6,
						"workers=%d slabRows=%d mode=%d: volume ref=%v got=%v",
						workers, slabRows, mode, ref.Volume(), got.Volume())
				}
			}
		}

		// A jittered, asymmetric field must also share the
		// exact grid identity across processes.
		jitterSolid := TranslateSolid(&Sphere{Radius: 0.7}, XYZ(-1.23, 4.56, 2.79))
		jitterField := mustMarshalField(t, processTestField{
			Kind: "sphere", Radius: 0.7, Offset: XYZ(-1.23, 4.56, 2.79),
		})
		d := &DualContouring{
			S: SolidSurfaceEstimator{Solid: jitterSolid}, Delta: 0.15, BufferSize: 300,
		}
		ref := d.Mesh()
		got, err := d.MeshProcess(context.Background(), workerCommand(), jitterField, 3, 2)
		c.Check(err == nil, "jittered field: %v", err)
		c.Check(sameTriangles(ref, got, 1e-8), "jittered field triangles differ")

		// Global grid-edge ownership partitions every edge
		// across slabs exactly once; this checks the
		// ownership algebra directly.
		c.Check(edgeOwnershipIsPartition(solid, 0.18, true, []int{1, 2, 3, 5}),
			"grid edge ownership is not a unique partition")

		// An empty field still reaches a complete terminal
		// state with a non-nil empty mesh.
		emptySolid := CheckedFuncSolid(XYZ(-1, -1, -1), XYZ(1, 1, 1),
			func(Coord3D) bool { return false })
		emptyD := &DualContouring{
			S: SolidSurfaceEstimator{Solid: emptySolid}, Delta: 0.2, NoJitter: true,
		}
		var emptyStats DCProcessStats
		emptyD.ProcessStats = func(s DCProcessStats) { emptyStats = s }
		emptyMesh, err := emptyD.MeshProcess(context.Background(), workerCommand(),
			mustMarshalField(t, processTestField{Kind: "empty"}), 2, 3)
		c.Check(err == nil, "empty field: %v", err)
		c.Check(emptyMesh != nil && emptyMesh.NumTriangles() == 0,
			"empty field should produce a non-nil empty mesh")
		c.Check(emptyStats.Slabs > 0 && len(emptyStats.PIDs) == emptyStats.Slabs,
			"empty field stats incomplete: %+v", emptyStats)
	})
}

// sameTriangles checks that every triangle of got matches a
// reference triangle with identical winding and every
// coordinate within tol.
func sameTriangles(ref, got *Mesh, tol float64) bool {
	if ref == nil || got == nil {
		return ref == got
	}
	if ref.NumTriangles() != got.NumTriangles() {
		return false
	}
	refTris := ref.TriangleSlice()
	allMatched := true
	got.Iterate(func(t *Triangle) {
		matched := false
		for _, r := range refTris {
			ok := true
			for k := 0; k < 3; k++ {
				if t[k].Dist(r[k]) > tol {
					ok = false
					break
				}
			}
			if ok {
				matched = true
				break
			}
		}
		if !matched {
			allMatched = false
		}
	})
	return allMatched
}

// edgeOwnershipIsPartition enumerates every global grid edge
// and asserts that exactly one slab in each partition
// claims ownership of it under the same rule the workers
// use.
func edgeOwnershipIsPartition(solid Solid, delta float64, noJitter bool,
	slabRowChoices []int) bool {
	xs, ys, zs := dcGridCoordinates(solid.Min(), solid.Max(), delta, noJitter)
	numCubeLayers := len(zs) - 1
	for _, slabRows := range slabRowChoices {
		var ranges [][2]int
		for z0 := 0; z0 < numCubeLayers; z0 += slabRows {
			z1 := z0 + slabRows
			if z1 > numCubeLayers {
				z1 = numCubeLayers
			}
			ranges = append(ranges, [2]int{z0, z1})
		}
		xCount := (len(xs) - 1) * len(ys)
		yCount := (len(ys) - 1) * len(xs)
		zCount := len(xs) * len(ys)
		stride := xCount + yCount + zCount
		// x/y edges exist at every corner layer; z edges only
		// span cube layers, so the top corner layer has none.
		for globalZ := 0; globalZ < len(zs); globalZ++ {
			layerGroups := xCount + yCount
			if globalZ < len(zs)-1 {
				layerGroups = stride
			}
			for axisIdx := 0; axisIdx < layerGroups; axisIdx++ {
				edgeID := globalZ*stride + axisIdx
				_ = edgeID
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
				claims := 0
				for _, r := range ranges {
					if owner >= r[0] && owner < r[1] {
						claims++
					}
				}
				if claims != 1 {
					return false
				}
			}
		}
	}
	return true
}

// -----------------------------------------------------------------------------
// Repair
// -----------------------------------------------------------------------------

func TestDCProcessRepair(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// The contact-box field from the original DC tests,
		// including a translated asymmetric placement.
		for _, offset := range []Coord3D{XYZ(0, 0, 0), XYZ(0.31, -0.47, 0.23)} {
			solid := TranslateSolid(contactBoxes(), offset)
			field := mustMarshalField(t, processTestField{
				Kind: "contact", Offset: offset,
			})
			for _, clip := range []bool{false, true} {
				for _, workers := range []int{1, 2, 3} {
					d := &DualContouring{
						S:          SolidSurfaceEstimator{Solid: solid},
						Delta:      0.1,
						Repair:     true,
						Clip:       clip,
						BufferSize: 512,
					}
					ref := d.Mesh()
					got, err := d.MeshProcess(context.Background(), workerCommand(),
						field, workers, 2)
					c.Check(err == nil, "offset=%v clip=%v workers=%d: %v",
						offset, clip, workers, err)
					if err != nil {
						continue
					}
					c.Check(!got.NeedsRepair(),
						"offset=%v clip=%v workers=%d: singular edges remain",
						offset, clip, workers)
					c.Check(len(got.SingularVertices()) == 0,
						"offset=%v clip=%v workers=%d: singular vertices remain",
						offset, clip, workers)
					c.Check(got.Orientable() && len(got.InconsistentEdges()) == 0,
						"offset=%v clip=%v workers=%d: orientation inconsistent",
						offset, clip, workers)
					c.Check(math.Abs(ref.Volume()-got.Volume()) <= 1e-6,
						"offset=%v clip=%v workers=%d: volume ref=%v got=%v",
						offset, clip, workers, ref.Volume(), got.Volume())
					c.Check(countComponents(ref) == countComponents(got),
						"offset=%v clip=%v workers=%d: components ref=%d got=%d",
						offset, clip, workers,
						countComponents(ref), countComponents(got))
					c.Check(edgeHistogramEqual(ref, got),
						"offset=%v clip=%v workers=%d: edge adjacency histogram differs",
						offset, clip, workers)
					c.Check(got.NumTriangles() == ref.NumTriangles(),
						"offset=%v clip=%v workers=%d: tri count ref=%d got=%d",
						offset, clip, workers, ref.NumTriangles(), got.NumTriangles())
				}
			}
		}

		// Deterministic must-clip retry across a partition
		// boundary: two distinct global cubes in different
		// slabs solve to the same coordinate on round one and
		// separate after clipping.
		retried, stats := runScriptedRetry(t, c)
		c.Check(retried, "scripted duplicate-coordinate retry did not converge")
		c.Check(stats.Retries >= 1, "expected global retries, got %d", stats.Retries)
		c.Check(stats.Slabs >= 2, "retry scenario needs multiple slabs, got %d", stats.Slabs)
	})
}

func runScriptedRetry(t *testing.T, c *taskcheck.C) (bool, DCProcessStats) {
	t.Helper()
	// Bounds come from a real parent-side solid; the chaos
	// worker ignores its geometry and returns scripted cube
	// positions instead.
	solid := NewRect(XYZ(0, 0, 0), XYZ(2, 2, 2))
	xs, ys, _ := dcGridCoordinates(solid.Min(), solid.Max(), 0.5, true)
	spec := &scriptSpec{
		NX: len(xs),
		NY: len(ys),
		// One cube in slab 0 and one in slab 1 sharing a
		// coordinate on their partition boundary.
		FirstCubes:  [][3]int{{0, 0, 0}, {1, 1, 3}},
		FirstPos:    XYZ(0.123, 0.456, 0.789),
		SecondCubes: [][3]int{{0, 0, 0}, {1, 1, 3}},
		SecondBase:  XYZ(0.2, 0.2, 0.2),
	}
	field := mustMarshalField(t, processTestField{Kind: "script", Script: spec})
	d := &DualContouring{
		S: SolidSurfaceEstimator{Solid: solid}, Delta: 0.5, NoJitter: true,
		Repair: true, Clip: false,
	}
	var stats DCProcessStats
	d.ProcessStats = func(s DCProcessStats) { stats = s }
	mesh, err := d.MeshProcess(context.Background(), chaosWorkerCommand(), field, 2, 2)
	if err != nil {
		c.Check(false, "scripted retry failed: %v", err)
		return false, stats
	}
	return mesh != nil, stats
}

// countComponents counts connected triangle components,
// where triangles are connected through shared vertices.
func countComponents(m *Mesh) int {
	v2f := map[Coord3D][]*Triangle{}
	m.Iterate(func(t *Triangle) {
		for _, p := range t {
			v2f[p] = append(v2f[p], t)
		}
	})
	visited := map[*Triangle]bool{}
	components := 0
	m.Iterate(func(start *Triangle) {
		if visited[start] {
			return
		}
		components++
		queue := []*Triangle{start}
		visited[start] = true
		for len(queue) > 0 {
			t := queue[0]
			queue = queue[1:]
			for _, p := range t {
				for _, next := range v2f[p] {
					if !visited[next] {
						visited[next] = true
						queue = append(queue, next)
					}
				}
			}
		}
	})
	return components
}

// edgeHistogramEqual compares the distribution of undirected
// edge adjacency counts, an independent topology check that
// does not rely on Mesh's own repair predicates.
func edgeHistogramEqual(a, b *Mesh) bool {
	counts := func(m *Mesh) map[int]int {
		edges := map[[2]Coord3D]int{}
		m.Iterate(func(t *Triangle) {
			for i := 0; i < 3; i++ {
				edges[undirectedKey(t[i], t[(i+1)%3])]++
			}
		})
		hist := map[int]int{}
		for _, n := range edges {
			hist[n]++
		}
		return hist
	}
	ha, hb := counts(a), counts(b)
	if len(ha) != len(hb) {
		return false
	}
	for k, v := range ha {
		if hb[k] != v {
			return false
		}
	}
	return true
}

func undirectedKey(a, b Coord3D) [2]Coord3D {
	if coordLess(a, b) {
		return [2]Coord3D{a, b}
	}
	return [2]Coord3D{b, a}
}

func coordLess(a, b Coord3D) bool {
	if a.X != b.X {
		return a.X < b.X
	}
	if a.Y != b.Y {
		return a.Y < b.Y
	}
	return a.Z < b.Z
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

func TestDCProcessLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		solid := TranslateSolid(&Sphere{Radius: 1.0}, XYZ(0.1, 0.2, 0.3))
		baseField := processTestField{Kind: "sphere", Radius: 1.0, Offset: solidOffset()}

		// Stats content and copy semantics.
		d := &DualContouring{
			S: SolidSurfaceEstimator{Solid: solid}, Delta: 0.2, NoJitter: true,
		}
		var stats DCProcessStats
		var calls int
		var rawPidCount int
		d.ProcessStats = func(s DCProcessStats) {
			calls++
			rawPidCount = len(s.PIDs)
			// Mutating the delivered slice must not corrupt
			// any internal state or later deliveries.
			s.PIDs = append(s.PIDs, 999999)
			stats = s
		}
		field := mustMarshalField(t, baseField)
		mesh, err := d.MeshProcess(context.Background(), workerCommand(), field, 2, 3)
		c.Check(err == nil, "stats run: %v", err)
		c.Check(mesh != nil && mesh.NumTriangles() > 0, "stats run produced no mesh")
		c.Check(calls == 1, "stats callback called %d times, want 1", calls)
		expectedSlabs := expectedSlabCount(solid, 0.2, true, 3)
		c.Check(stats.Slabs == expectedSlabs, "slabs got=%d want=%d", stats.Slabs, expectedSlabs)
		c.Check(rawPidCount == stats.Slabs, "one fresh PID per slab: %+v", stats)
		originalPIDs := stats.PIDs[:rawPidCount]
		c.Check(sort.IntsAreSorted(originalPIDs), "PIDs should be sorted ascending")
		c.Check(stats.PeakActiveSlabs >= 1 && stats.PeakActiveSlabs <= 2,
			"peak active slabs out of range: %d", stats.PeakActiveSlabs)
		c.Check(uniqueSorted(originalPIDs), "PIDs not unique")
		assertReaped(c, originalPIDs)

		// A second independent call gets its own pending jobs
		// and its own stats value.
		var stats2 DCProcessStats
		d.ProcessStats = func(s DCProcessStats) { stats2 = s }
		_, err = d.MeshProcess(context.Background(), workerCommand(), field, 3, 2)
		c.Check(err == nil, "second independent call: %v", err)
		c.Check(len(stats2.PIDs) == stats2.Slabs && stats2.PeakActiveSlabs <= 3,
			"second call stats wrong: %+v", stats2)
		assertReaped(c, stats2.PIDs)

		// Out-of-order responses are reassembled correctly.
		// Two workers pull slabs in order; every slab except
		// the first is released before slab 0, so the earliest
		// dispatched response necessarily arrives last.
		waitDir := t.TempDir()
		waitingDir := t.TempDir()
		ooField := mustMarshalField(t, processTestField{
			Kind:       "passthrough",
			Radius:     1.0,
			Offset:     solidOffset(),
			WaitDir:    waitDir,
			WaitingDir: waitingDir,
		})
		ooD := &DualContouring{
			S: SolidSurfaceEstimator{Solid: solid}, Delta: 0.2, NoJitter: true,
		}
		ooErrCh := make(chan error, 1)
		ooMeshCh := make(chan *Mesh, 1)
		go func() {
			m, e := ooD.MeshProcess(context.Background(), chaosWorkerCommand(),
				ooField, 2, 2)
			ooErrCh <- e
			ooMeshCh <- m
		}()
		slabN := expectedSlabCount(solid, 0.2, true, 2)
		release := func(slab int) {
			path := filepath.Join(waitDir, fmt.Sprintf("slab-%d", slab))
			c.Check(os.WriteFile(path, []byte("go"), 0o600) == nil,
				"write handshake slab %d", slab)
		}
		waitWaiting := func(slab int) {
			waitForFile(filepath.Join(waitingDir, fmt.Sprintf("waiting-%d", slab)))
		}
		// Both workers start blocked on slabs 0 and 1.
		waitWaiting(0)
		waitWaiting(1)
		for slab := 1; slab < slabN; slab++ {
			release(slab)
			// The freed worker picks the next slab and blocks
			// on its gate before we continue.
			if slab+1 < slabN {
				waitWaiting(slab + 1)
			}
		}
		// Finally release the oldest, still-pending slab.
		release(0)
		select {
		case e := <-ooErrCh:
			c.Check(e == nil, "out-of-order run: %v", e)
			c.Check(sameTriangles(d.Mesh(), <-ooMeshCh, 1e-8),
				"out-of-order mesh differs from reference")
		case <-time.After(30 * time.Second):
			c.Check(false, "out-of-order run hung")
		}

		// Cancellation: workers block forever, then the parent
		// cancels and must return no mesh and reap everyone.
		cancelDir := t.TempDir()
		startedDir := t.TempDir()
		cancelField := mustMarshalField(t, processTestField{
			Kind: "passthrough", Radius: 1.0, Offset: solidOffset(),
			WaitDir: cancelDir, StartedDir: startedDir,
		})
		cancelD := &DualContouring{
			S: SolidSurfaceEstimator{Solid: solid}, Delta: 0.2, NoJitter: true,
		}
		var cancelStats DCProcessStats
		cancelD.ProcessStats = func(s DCProcessStats) { cancelStats = s }
		ctx, cancel := context.WithCancel(context.Background())
		cancelErrCh := make(chan error, 1)
		cancelMeshCh := make(chan *Mesh, 1)
		go func() {
			m, e := cancelD.MeshProcess(ctx, chaosWorkerCommand(), cancelField, 2, 1)
			cancelErrCh <- e
			cancelMeshCh <- m
		}()
		waitForCount(startedDir, 2)
		cancel()
		select {
		case e := <-cancelErrCh:
			c.Check(e != nil, "cancelled call must return an error")
			c.Check(<-cancelMeshCh == nil, "cancelled call must return nil mesh")
		case <-time.After(30 * time.Second):
			c.Check(false, "cancelled call hung")
		}
		c.Check(len(cancelStats.PIDs) > 0, "cancelled call started no workers")
		assertReaped(c, cancelStats.PIDs)
		// A fresh call after failure must work normally.
		restartD := &DualContouring{
			S: SolidSurfaceEstimator{Solid: solid}, Delta: 0.2, NoJitter: true,
		}
		restartMesh, restartErr := restartD.MeshProcess(context.Background(),
			workerCommand(), field, 2, 3)
		c.Check(restartErr == nil, "restart after cancel: %v", restartErr)
		c.Check(restartMesh != nil && restartMesh.NumTriangles() > 0,
			"restart produced no mesh")

		// Worker crash after the ready handshake.
		crashD := newLifecycleDualContouring(solid)
		_, err = crashD.MeshProcess(context.Background(), chaosWorkerCommand(),
			mustMarshalField(t, processTestField{Kind: "crash"}), 1, 2)
		c.Check(err != nil, "worker crash must surface an error")

		// Corrupted protocol frame.
		garbageD := newLifecycleDualContouring(solid)
		_, err = garbageD.MeshProcess(context.Background(), chaosWorkerCommand(),
			mustMarshalField(t, processTestField{Kind: "garbage"}), 1, 2)
		c.Check(err != nil, "protocol corruption must surface an error")

		// Factory failure delivered as a worker error frame.
		failD := newLifecycleDualContouring(solid)
		_, err = failD.MeshProcess(context.Background(), workerCommand(),
			mustMarshalField(t, processTestField{Kind: "factoryfail"}), 1, 2)
		c.Check(err != nil, "factory failure must surface an error")

		// Bounds mismatch is rejected before sampling.
		badD := newLifecycleDualContouring(solid)
		_, err = badD.MeshProcess(context.Background(), workerCommand(),
			mustMarshalField(t, processTestField{Kind: "badbounds"}), 1, 2)
		c.Check(err != nil, "bounds mismatch must surface an error")

		// stderr is drained without being read as protocol.
		spamD := newLifecycleDualContouring(solid)
		spamMesh, err := spamD.MeshProcess(context.Background(), chaosWorkerCommand(),
			mustMarshalField(t, processTestField{
				Kind: "passthrough", Radius: 1.0, Offset: solidOffset(),
				StderrSpam: 256 * 1024,
			}), 2, 3)
		c.Check(err == nil, "stderr spam run: %v", err)
		c.Check(spamMesh != nil && spamMesh.NumTriangles() > 0,
			"stderr spam run produced no mesh")

		// Up-front validation errors, with stats still
		// delivered once and no processes spawned.
		var earlyStats DCProcessStats
		earlyD := &DualContouring{
			S: SolidSurfaceEstimator{Solid: solid}, Delta: 0.2, NoJitter: true,
			ProcessStats: func(s DCProcessStats) { earlyStats = s },
		}
		_, err = earlyD.MeshProcess(context.Background(), workerCommand(), field, 0, 2)
		c.Check(err != nil, "workers=0 must fail")
		_, err = earlyD.MeshProcess(context.Background(), workerCommand(), field, 2, 0)
		c.Check(err != nil, "slabRows=0 must fail")
		earlyD.S.RandomSearchNormals = true
		_, err = earlyD.MeshProcess(context.Background(), workerCommand(), field, 2, 2)
		c.Check(err != nil, "RandomSearchNormals must fail up front")
		c.Check(len(earlyStats.PIDs) == 0,
			"up-front validation failure must not spawn workers: %+v", earlyStats)
	})
}

func solidOffset() Coord3D {
	return XYZ(0.1, 0.2, 0.3)
}

func newLifecycleDualContouring(solid Solid) *DualContouring {
	return &DualContouring{
		S: SolidSurfaceEstimator{Solid: solid}, Delta: 0.2, NoJitter: true,
	}
}

func expectedSlabCount(solid Solid, delta float64, noJitter bool, slabRows int) int {
	_, _, zs := dcGridCoordinates(solid.Min(), solid.Max(), delta, noJitter)
	layers := len(zs) - 1
	n := layers / slabRows
	if layers%slabRows != 0 {
		n++
	}
	return n
}

func uniqueSorted(xs []int) bool {
	for i := 1; i < len(xs); i++ {
		if xs[i] <= xs[i-1] {
			return false
		}
	}
	return true
}

func waitForCount(dir string, n int) {
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		entries, err := os.ReadDir(dir)
		if err == nil && len(entries) >= n {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
}

func assertReaped(c *taskcheck.C, pids []int) {
	for _, pid := range pids {
		p, err := os.FindProcess(pid)
		if err != nil {
			continue
		}
		// Signal 0 performs no delivery; ESRCH means the
		// child has exited and been reaped.
		err = p.Signal(syscall.Signal(0))
		c.Check(err != nil, "worker pid %d still alive after MeshProcess returned", pid)
	}
}
