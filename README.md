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

## Two-bounce reconnect rendering

`render/instance_scene.go` and `render/reconnect.go` add an optional path that
reuses surface sampling distributions from an old pose to guide the indirect
light of the current frame. An `InstanceScene` is an immutable set of
Lambertian sphere instances (unique `uint64` IDs, row-major 3x4 affine
matrices with shear, non-uniform scale and mirroring allowed). The same
instance ID with the same unit-sphere direction is the same material point
across poses. The exact public API:

```go
func NewInstanceScene(protos []SphereProto, instances []Instance) (*InstanceScene, error)
func (s *InstanceScene) HitSurface(r primitive.Ray, tMin, tMax float64) (bool, SurfaceHit)
func (s *InstanceScene) Resolve(ref SurfaceRef) (SurfacePoint, error)
func (s *InstanceScene) AreaFactor(ref SurfaceRef) (float64, error)
func EstimateReconnect(current *InstanceScene, target SurfaceRef, sources []ProposalSource, candidates int, rnd *rand.Rand) (primitive.Color, ReconnectStats, error)
func RenderReconnect(current *InstanceScene, camera *primitive.Camera, oldScenes []*InstanceScene, width, height, samples, candidates, workers int, seed int64) ([]primitive.Color, error)
```

`EstimateReconnect` adds the current target to the proposal sources, emits
`candidates` cosine hemisphere rays per source, transports the first hits into
the current frame by ID and unit-sphere direction, and selects one candidate
with weight `K/qmix` (target kernel over the equal-weight mixture of all
transported source densities, including the old-to-new area ratio). The
surface term is `targetColor * S/M * visibility * L_y`, where `L_y` continues
with the original `Lambertian.Bounce` and the existing `color` recursion at
depth 2; an independent cosine ray estimates the direct sky term.
`RenderReconnect` returns the row-major linear RGB frame (row 0 is the top
scanline) and is deterministic in the seed for any worker count. See
`examples/reconnect` for a two-pose render.

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
