# go-trace

Implementing a Path Tracer so that I can teach myself the Go language.

Now with more concurrency! :sparkles:

Following the E-book by Peter Shirley ['Ray Tracing in One Weekend'](http://www.amazon.com/Ray-Tracing-Weekend-Peter-Shirley-ebook/dp/B01B5AODD8), but in Go instead of C++.

I also wrote a series of posts chronicling the process on [my blog](https://www.markphelps.me/2016/03/15/writing-a-ray-tracer-in-go/).

!['Scene'](https://s3.amazonaws.com/markphelps.me/2016/out.png)

[![Software License](https://img.shields.io/badge/license-MIT-brightgreen.svg?style=flat-square)](LICENSE)
[![Go Doc](https://img.shields.io/badge/godoc-reference-blue.svg?style=flat-square)](http://godoc.org/github.com/markphelps/go-trace)
[![Go Report Card](https://goreportcard.com/badge/github.com/markphelps/example?style=flat-square)](https://goreportcard.com/report/github.com/markphelps/go-trace)
[![SayThanks.io](https://img.shields.io/badge/SayThanks.io-%E2%98%BC-1EAEDB.svg?style=flat-square)](https://saythanks.io/to/markphelps)

## Options

```
$ ./bin/go-trace -help
Usage of ./bin/go-trace:
  -a value
        camera aperture (default 0.010000)
  -cpus value
        number of CPUs to use (default 8)
  -fov value
        vertical field of view (degrees) (default 75.000000)
  -h value
        height of image (pixels) (default 500)
  -n value
        number of samples per pixel for AA (default 100)
  -o value
        output filename (default out.png)
  -version
        show version and exit
  -w value
        width of image (pixels) (default 600)
  -x float
        look from X (default 10)
  -y float
        look from Y (default 4)
  -z float
        look from Z (default 6)
```

## Running

```
$ make
go build -o bin/go-trace github.com/markphelps/go-trace/cmd/go-trace
```

```
$ ./bin/go-trace

Rendering 600 x 500 pixel scene with 486 objects:
[4 cpus, 100 samples/pixel, 75.00° fov, 0.01 aperture]

[==========----------------------------------------------------------------------] 12.80%
```

## Tests

```
$ make test
```

## Output

The default output format is [PNG](https://en.wikipedia.org/wiki/Portable_Network_Graphics) and the default filename is **out.png**.

This can be changed by using the `-o` flag as described in the help output.

Ex: `$ ./bin/go-trace -o output.jpeg`

Current supported file extensions:

* .png
* .jpg
* .jpeg

## Reconnect rendering (optional)

`render/instance_scene.go` and `render/reconnect.go` add an optional two-hop
reconnection path for reusing the surface-sampling distribution of an old pose
to light the current frame. It is pure Go and uses only the standard library;
no window or GPU is required.

### InstanceScene

`render.InstanceScene` is an immutable collection of diffusely shaded spheres
implementing `primitive.Hitable`. Construction copies every prototype and
instance description, so mutating the caller's slices afterwards does not
affect an existing scene, and an old scene may be queried concurrently with the
current frame.

```go
type SpherePrototype struct {
    Center primitive.Vector
    Radius float64           // finite and > 0
    Color  primitive.Color   // Lambertian albedo, each component in [0,1]
}

type InstanceDesc struct {
    ID        uint64         // unique within a scene
    Prototype int            // index into the prototype slice
    Matrix    [3][4]float64  // row-major world = A*local + t
}
```

The 3x3 part may contain shear, non-uniform scale and mirroring, but must be
finite and invertible. Prototypes may be shared by several instances.

Identity across frames is the instance ID plus the unit-sphere direction, not
the prototype index or a material pointer:

```go
type SurfaceRef struct {
    Instance    uint64
    LocalNormal primitive.Vector // unit direction; local point = Center + Radius*LocalNormal
}
```

Entry points:

```go
func NewInstanceScene(prototypes []SpherePrototype, instances []InstanceDesc) (*InstanceScene, error)
func (s *InstanceScene) HitRef(r primitive.Ray, tMin, tMax float64) (bool, primitive.Hit, SurfaceRef)
func (s *InstanceScene) Hit(r primitive.Ray, tMin, tMax float64) (bool, primitive.Hit) // primitive.Hitable
func (s *InstanceScene) Surface(ref SurfaceRef) (Surface, error)
```

Rays need not be unit length: `HitRef` keeps the `O + tD` parameterization,
and outward normals are normalized by the inverse transpose so mirroring does
not flip sphere inside/outside. An empty scene misses every ray and renders as
sky.

### EstimateReconnect

```go
type ProposalSource interface {
    ReconnectScene() *InstanceScene
    ReconnectSurface() SurfaceRef
}

type Source struct {
    Scene   *InstanceScene
    Surface SurfaceRef
}

type ReconnectStats struct {
    M                 int
    Positive          int
    SumWeight         float64
    Selected          SurfaceRef
    QMix              float64
    Visible           bool
    ContinuationCalls int
}

func EstimateReconnect(scene *InstanceScene, target SurfaceRef,
    proposals []ProposalSource, n int, rnd *rand.Rand) (primitive.Color, ReconnectStats, error)
```

`EstimateReconnect` draws `n` true cosine-hemisphere rays from each source
(and always appends the current target as an extra source), maps each first
hit to the current frame by `(ID, LocalNormal)`, and streams a weighted
selection by `K/qmix`, where `K` is the target cosine kernel and `qmix` is the
equal average of every source's back-recomputed density. Candidate counts are
fixed at `M = n * (len(proposals) + 1)`; misses, vanished IDs and zero-kernel
samples count toward `M` and are not resampled. Density back-recomputation uses
the same `tMin = 0.001` self-intersection rule and converts source-surface area
to current-surface area with `radius^2 * |det A| * |A^-T n|`. The returned
statistics are read-only. Invalid arguments (nil scene, nil RNG, non-positive
`n`, invalid references) return an error.

`RenderReconnect` renders a whole frame along real `Camera.RayAt` first hits
and returns a row-major linear RGB frame:

```go
func RenderReconnect(scene *InstanceScene, camera *primitive.Camera,
    proposals []ProposalSource, width, height, samples, candidates, workers int,
    seed int64) ([][][3]float64, error)
```

The same inputs produce the same frame for any worker count.

Run the example, which draws the before and after poses:

```
$ go run ./examples/reconnect /tmp/reconnect-output
```
