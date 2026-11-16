//-----------------------------------------------------------------------------
/*

processmesh renders a sphere with the out-of-process field evaluator.

The program has two modes:

  processmesh -output mesh.stl
      Starts itself in worker mode, renders a sphere through RenderProcess and
      writes the result as an STL file.

  processmesh -worker
      Reads evaluation requests as newline delimited JSON on stdin and writes
      responses on stdout.  This is the field process protocol used by
      RenderProcess.

*/
//-----------------------------------------------------------------------------

package main

import (
	"bufio"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/deadsy/sdfx/render"
	"github.com/deadsy/sdfx/sdf"
	v3 "github.com/deadsy/sdfx/vec/v3"
)

//-----------------------------------------------------------------------------

const sphereRadius = 1.0

// field is the scalar field evaluated by the worker.  It must be finite and
// 1-Lipschitz over the bounds passed to RenderProcess.
func field(p v3.Vec) float64 {
	return p.Length() - sphereRadius
}

//-----------------------------------------------------------------------------

type request struct {
	ID     int          `json:"id"`
	Points [][3]float64 `json:"points"`
}

type response struct {
	ID     int       `json:"id"`
	Values []float64 `json:"values"`
}

// runWorker speaks the line delimited JSON protocol on stdin/stdout.
func runWorker() error {
	in := bufio.NewReader(os.Stdin)
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	for {
		line, err := in.ReadBytes('\n')
		if err != nil {
			if len(line) > 0 {
				return fmt.Errorf("worker: partial line at EOF")
			}
			return nil
		}
		var req request
		if err := json.Unmarshal(line, &req); err != nil {
			return fmt.Errorf("worker: bad request: %w", err)
		}
		values := make([]float64, len(req.Points))
		for i, p := range req.Points {
			values[i] = field(v3.Vec{X: p[0], Y: p[1], Z: p[2]})
		}
		data, err := json.Marshal(response{ID: req.ID, Values: values})
		if err != nil {
			return err
		}
		if _, err := out.Write(append(data, '\n')); err != nil {
			return err
		}
		if err := out.Flush(); err != nil {
			return err
		}
	}
}

//-----------------------------------------------------------------------------

// sliceWriter collects triangles in memory.
type sliceWriter struct {
	triangles []*sdf.Triangle3
}

func (w *sliceWriter) Write(in []*sdf.Triangle3) error {
	w.triangles = append(w.triangles, in...)
	return nil
}

func (w *sliceWriter) Close() error { return nil }

//-----------------------------------------------------------------------------

func main() {
	worker := flag.Bool("worker", false, "run as the field evaluation worker")
	output := flag.String("output", "mesh.stl", "path of the STL file to write")
	meshCells := flag.Int("cells", 24, "mesh cells on the longest axis")
	flag.Parse()

	if *worker {
		if err := runWorker(); err != nil {
			fmt.Fprintf(os.Stderr, "%s\n", err)
			os.Exit(1)
		}
		return
	}

	span := 1.2 * sphereRadius
	bounds := sdf.Box3{
		Min: v3.Vec{X: -span, Y: -span, Z: -span},
		Max: v3.Vec{X: span, Y: span, Z: span},
	}
	cfg := render.ProcessConfig{
		Command:     []string{os.Args[0], "-worker"},
		Bounds:      bounds,
		MeshCells:   *meshCells,
		BatchSize:   64,
		MaxInFlight: 4,
		Window:      16,
	}

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()

	w := &sliceWriter{}
	stats, err := render.RenderProcess(ctx, cfg, w)
	if err != nil {
		fmt.Fprintf(os.Stderr, "processmesh: %s\n", err)
		os.Exit(1)
	}
	if len(w.triangles) == 0 {
		fmt.Fprintf(os.Stderr, "processmesh: empty mesh\n")
		os.Exit(1)
	}
	if err := render.SaveSTL(*output, w.triangles); err != nil {
		fmt.Fprintf(os.Stderr, "processmesh: %s\n", err)
		os.Exit(1)
	}
	fmt.Printf("wrote %s: %d triangles, %d batches, %d samples\n",
		*output, len(w.triangles), stats.Batches, stats.Samples)
}

//-----------------------------------------------------------------------------
