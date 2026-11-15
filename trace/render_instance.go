package trace

import (
	"image"
	"io/ioutil"
	"log"
	"math/rand"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"time"

	"github.com/peterstace/grayt/scene"
	"github.com/peterstace/grayt/xmath"
)

type Stats struct {
	LoadState   string
	Workers     int
	Completed   int
	Passes      int
	TraceRateHz int
	Generation  uint64
}

type loadState int

const (
	unloaded loadState = iota
	loading
	loaded
	loadError
)

type Instance struct {
	// Read only variables
	sceneFn       func() scene.Scene
	accumFilename string
	dim           xmath.Dimensions

	// Access controlled by cond variable
	cond             *sync.Cond
	requestedWorkers int
	loadState        loadState
	current          *generation
	nextGenID        uint64

	// Atomic access
	actualWorkers int64
	traceRate     int64
}

func NewInstance(dim xmath.Dimensions, sceneFn func() scene.Scene, filename string) *Instance {
	inst := &Instance{
		sceneFn:       sceneFn,
		accumFilename: filename,
		dim:           dim,
		cond:          sync.NewCond(new(sync.Mutex)),
		nextGenID:     1, // Generation 0 is reserved for the startup load.
	}
	go inst.loadScene()
	go inst.dispatchWork()
	go inst.monitorTraceRate()
	return inst
}

func (in *Instance) loadScene() {
	in.cond.L.Lock()
	for in.requestedWorkers == 0 || in.loadState != unloaded {
		in.cond.Wait()
	}
	in.loadState = loading
	in.cond.L.Unlock()

	cam, objs := buildScene(in.sceneFn())
	accel := accelForObjects(objs)
	accum := newAccumulator(in.dim)

	// If a live scene was committed while the initial scene was being built,
	// it takes precedence: generation 0 is never published and the old
	// accumulator file is not read back in this instance.
	in.cond.L.Lock()
	superseded := in.current != nil
	in.cond.L.Unlock()

	if !superseded {
		f, err := os.Open(in.accumFilename)
		if err == nil {
			defer f.Close()
			if _, err := accum.ReadFrom(f); err != nil {
				log.Printf("could not read from accum state file: %v", err)
				in.cond.L.Lock()
				if in.current == nil {
					in.loadState = loadError
				} else {
					in.loadState = loaded
				}
				in.cond.Broadcast()
				in.cond.L.Unlock()
				return
			}
		}
	}

	completed := int64(accum.dim.High * accum.dim.Wide * accum.getPasses())

	in.cond.L.Lock()
	if in.current == nil {
		in.current = &generation{
			id:    0,
			cam:   cam,
			accel: accel,
			accum: accum,
		}
		atomic.StoreInt64(&in.current.completed, completed)
	}
	in.loadState = loaded
	in.cond.Broadcast()
	in.cond.L.Unlock()
}

func (in *Instance) SetWorkers(workers int) {
	in.cond.L.Lock()
	in.requestedWorkers = workers
	in.cond.L.Unlock()
	in.cond.Broadcast()
}

func (in *Instance) dispatchWork() {
	var lastSave time.Time
	for {
		in.cond.L.Lock()
		for in.requestedWorkers == 0 || in.current == nil {
			atomic.StoreInt64(&in.actualWorkers, 0)
			in.cond.Wait()
		}

		// Bind this pass to the current generation. Every path traced in
		// the pass uses only this generation's camera, acceleration
		// structure and staging pixels, and the completed pass is merged
		// back into this same generation (if it has been superseded, the
		// merge lands in the retired generation and is never seen again).
		gen := in.current
		var ctx workContext
		ctx.gen = gen
		atomic.StoreInt64(&in.actualWorkers, int64(in.requestedWorkers))
		ctx.wg.Add(in.requestedWorkers)
		for i := 0; i < in.requestedWorkers; i++ {
			go in.work(&ctx)
		}
		in.cond.L.Unlock()

		ctx.wg.Wait()
		gen.accum.merge(1)
		if time.Since(lastSave) > time.Minute {
			in.saveAccum()
			lastSave = time.Now()
		}
	}
}

// saveAccumTestHook, when non-nil, is called by saveAccum after the
// accumulator snapshot has been written to its temporary file but before the
// rename decision. It lets tests interleave a save with a scene update.
var saveAccumTestHook struct {
	fn atomic.Pointer[func()]
}

func (in *Instance) saveAccum() {
	in.cond.L.Lock()
	gen := in.current
	in.cond.L.Unlock()
	if gen == nil {
		return
	}

	log.Printf("saving accumulator state")
	dir := filepath.Dir(in.accumFilename)
	tmpF, err := ioutil.TempFile(dir, "*.data")
	if err != nil {
		log.Printf("could not create tmp file: %v", err)
		return
	}
	defer os.Remove(tmpF.Name())
	if _, err := gen.accum.WriteTo(tmpF); err != nil {
		log.Printf("could not write to accumulator state file: %v", err)
		tmpF.Close()
		return
	}
	if err := tmpF.Close(); err != nil {
		log.Printf("could not close tmp file: %v", err)
		return
	}

	if hook := saveAccumTestHook.fn.Load(); hook != nil {
		(*hook)()
	}

	// Only publish the snapshot if its generation is still current: a save
	// belonging to a retired generation must not rename its stale image over
	// the accumulator file once a newer generation has been published.
	in.cond.L.Lock()
	if in.current == gen {
		if err := os.Rename(tmpF.Name(), in.accumFilename); err != nil {
			log.Printf("could not rename accumulator state file: %v", err)
		}
	}
	in.cond.L.Unlock()
}

func (in *Instance) GetStats() Stats {
	in.cond.L.Lock()
	state := in.loadState
	gen := in.current
	in.cond.L.Unlock()

	loadStateStr := map[loadState]string{
		unloaded:  "unloaded",
		loading:   "loading",
		loaded:    "loaded",
		loadError: "error",
	}[state]

	var passes, completed int64
	var generationID uint64
	if gen != nil {
		// Once any generation is published the instance has a usable
		// scene, regardless of how the startup load is progressing.
		loadStateStr = "loaded"
		passes = int64(gen.accum.getPasses())
		completed = atomic.LoadInt64(&gen.completed)
		generationID = gen.id
	}

	return Stats{
		LoadState:   loadStateStr,
		Workers:     int(atomic.LoadInt64(&in.actualWorkers)),
		Completed:   int(completed),
		Passes:      int(passes),
		TraceRateHz: int(atomic.LoadInt64(&in.traceRate)),
		Generation:  generationID,
	}
}

type workContext struct {
	gen *generation
	idx int64
	wg  sync.WaitGroup
}

func (in *Instance) work(ctx *workContext) {
	gen := ctx.gen
	rng := rand.New(rand.NewSource(time.Now().UnixNano()))
	tr := newTracer(gen.accel, rng)
	wide := in.dim.Wide
	high := in.dim.High
	pxPitch := 2.0 / float64(wide)
	for {
		idx := int(atomic.AddInt64(&ctx.idx, 1)) - 1
		if idx >= wide*high {
			break
		}
		pxY := idx / wide
		pxX := idx % wide
		x := (float64(pxX-wide/2) + rng.Float64()) * pxPitch
		y := (float64(pxY-high/2) + rng.Float64()) * pxPitch * -1.0
		cr := gen.cam.makeRay(x, y, rng)
		cr.Dir = cr.Dir.Unit()
		c := tr.tracePath(cr)
		gen.accum.set(pxX, pxY, c)
		atomic.AddInt64(&gen.completed, 1)
	}
	ctx.wg.Done()
}

func (in *Instance) monitorTraceRate() {
	const samplePeriod = 5 * time.Second
	var lastCompleted int64
	ticker := time.NewTicker(samplePeriod)
	for {
		<-ticker.C
		var completed int64
		in.cond.L.Lock()
		if in.current != nil {
			completed = atomic.LoadInt64(&in.current.completed)
		}
		in.cond.L.Unlock()
		if lastCompleted != 0 && completed >= lastCompleted {
			sample := (completed - lastCompleted) * int64(time.Second) / int64(samplePeriod)
			atomic.StoreInt64(&in.traceRate, sample)
		}
		lastCompleted = completed
	}
}

func (in *Instance) Image() image.Image {
	in.cond.L.Lock()
	gen := in.current
	in.cond.L.Unlock()
	if gen == nil {
		return newAccumulator(in.dim).toImage(1.0)
	}
	return gen.accum.toImage(1.0)
}
