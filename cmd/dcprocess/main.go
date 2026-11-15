// Command dcprocess demonstrates multi-process dual
// contouring through model3d.MeshProcess and
// model3d.ServeDualContourWorker.
//
// In parent mode it spawns copies of itself in -worker
// mode, splits the global grid into z-axis slabs and writes
// the merged, globally repaired mesh as a binary STL. In
// worker mode it serves the framed worker protocol over
// stdin/stdout; stderr remains available for diagnostics.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"

	m "github.com/unixpickle/model3d/model3d"
)

// dcField is the opaque description exchanged through
// MeshProcess. The parent serializes it into the field
// argument and the worker's factory rebuilds the Solid from
// it; the model3d package never interprets these bytes.
type dcField struct {
	Kind   string  `json:"kind"`
	Radius float64 `json:"radius"`
}

func workerFactory(field []byte) (m.SolidSurfaceEstimator, error) {
	var desc dcField
	if err := json.Unmarshal(field, &desc); err != nil {
		return m.SolidSurfaceEstimator{}, fmt.Errorf("parse field: %w", err)
	}
	switch desc.Kind {
	case "sphere":
		return m.SolidSurfaceEstimator{Solid: &m.Sphere{Radius: desc.Radius}}, nil
	default:
		return m.SolidSurfaceEstimator{}, fmt.Errorf("unknown field kind %q", desc.Kind)
	}
}

func main() {
	worker := flag.Bool("worker", false, "run as a MeshProcess worker")
	output := flag.String("output", "dcprocess.stl", "output STL path in parent mode")
	workers := flag.Int("workers", 2, "number of concurrent worker processes")
	slabRows := flag.Int("slab-rows", 4, "z-axis cube layers per slab")
	flag.Parse()

	if *worker {
		if err := m.ServeDualContourWorker(os.Stdin, os.Stdout, workerFactory); err != nil {
			fmt.Fprintf(os.Stderr, "dcprocess worker: %v\n", err)
			os.Exit(1)
		}
		return
	}

	field, err := json.Marshal(dcField{Kind: "sphere", Radius: 1.0})
	if err != nil {
		fmt.Fprintf(os.Stderr, "dcprocess: %v\n", err)
		os.Exit(1)
	}

	dc := &m.DualContouring{
		S:          m.SolidSurfaceEstimator{Solid: &m.Sphere{Radius: 1.0}},
		Delta:      0.18,
		NoJitter:   true,
		MaxGos:     1,
		BufferSize: 256,
		Repair:     true,
		Clip:       true,
		ProcessStats: func(stats m.DCProcessStats) {
			fmt.Fprintf(os.Stderr,
				"dcprocess: slabs=%d retries=%d peak_active=%d pids=%v\n",
				stats.Slabs, stats.Retries, stats.PeakActiveSlabs, stats.PIDs)
		},
	}

	command := []string{os.Args[0], "-worker"}
	mesh, err := dc.MeshProcess(context.Background(), command, field, *workers, *slabRows)
	if err != nil {
		fmt.Fprintf(os.Stderr, "dcprocess: %v\n", err)
		os.Exit(1)
	}
	if mesh.NumTriangles() == 0 {
		fmt.Fprintln(os.Stderr, "dcprocess: produced empty mesh")
		os.Exit(1)
	}

	out, err := os.Create(*output)
	if err != nil {
		fmt.Fprintf(os.Stderr, "dcprocess: %v\n", err)
		os.Exit(1)
	}
	defer out.Close()
	if err := m.WriteSTL(out, mesh.TriangleSlice()); err != nil {
		fmt.Fprintf(os.Stderr, "dcprocess: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("dcprocess: wrote %d triangles to %s\n", mesh.NumTriangles(), *output)
}
