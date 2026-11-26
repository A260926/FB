# Tile passes (ordered intermediate tile stages)

Tile passes let you run a programmable, whole-tile stage **between** geometry
dispatches inside a single `commit()`. A pass operates directly on the tile's
existing color and depth buffers — it does not allocate a screen-sized color or
depth buffer and it is not emulated with a full-screen triangle.

Typical uses are tile-local reductions (e.g. minimum visible depth), adjusting
color based on the reduced result, or rewriting depth before later geometry is
drawn.

## Submitting a pass

```cpp
device.dispatch_pso(first_geometry);
device.dispatch_tile_pass(MyPass{...});   // runs after first_geometry, before later geometry
device.dispatch_pso(later_geometry);
device.commit();
```

The entry point is `graphics_device::dispatch_tile_pass(const Pass&)`.

### Pass requirements

A pass is a **trivially copyable** object that provides:

```cpp
struct MyPass
{
  static constexpr bool writes_depth = false;          // compile-time flag
  void exec(egl::rasterizer_tile_pass_context &ctx) const;
};
```

- `void exec(rasterizer_tile_pass_context&) const` is the per-tile callback.
- `static constexpr bool writes_depth` declares whether the pass rewrites the
  depth buffer. Set it `true` only when the callback writes `ctx.depth.data`.

Like PSOs, the pass object is **copied by value** into the per-commit frame
(PSO) store at dispatch time, so the frame store keeps its own copy. Any
external object reached through a pointer stored in the pass is **not** copied;
the caller must keep such objects alive until `commit()` returns. The callback
is **not** executed at dispatch time — it only runs during `commit()`.

### Ordering and frequency

Within one tile, geometry dispatches and tile passes execute strictly in
submission order: geometry, pass, geometry, pass, ... Consecutive passes are
neither merged nor dropped. Tiles are independent; there is no requirement that
different tiles pause at the same stage, and callbacks must not recursively
submit draws.

Every pass runs **exactly once per valid tile** in each commit, including tiles
that received no geometry clusters. Right and bottom edge tiles use the clipped
valid dimensions, so a pass never reads or writes outside the render target.

A commit that contains only tile passes still produces output, and the final
tile shader still runs after all stages to convert/transfer the tile. When the
same tile backing buffers are reused by the next commit, no color or depth data
written by a pass leaks across commits.

## The context: `rasterizer_tile_pass_context`

Defined in `egl_rasterizer_tile_pass.h`, in namespace `egl`:

| Member | Meaning |
|---|---|
| `uint16_t x, y` | Screen-space origin of the tile. |
| `uint16_t width, height` | Valid (clipped) tile size in pixels. Index local coordinates within `[0,width)` × `[0,height)`. |
| `uint16_t pitch` | Allocated tile row stride in pixels (the full tile width, which can exceed `width` on edge tiles). |
| `void *rts[4]` | Tile-local render target pointers; null for unconfigured attachments. |
| `uint8_t rt_pixel_sizes[4]` | Byte size of one pixel for each attachment (0 when unconfigured). |
| `rasterizer_depth_target depth` | Tile-local depth target; `depth.data` is null when no depth is configured. |

All pointers already point at the tile's local origin, so index them directly
with local coordinates:

```cpp
Pixel *rt = static_cast<Pixel*>(ctx.rts[0]);
rt[x + y*ctx.pitch].c.r = 255;
```

Configured color attachments may be read and written in place and are
interpreted in the format already configured for that attachment. Supported
depth storage formats are `depthfmt_uint8`, `depthfmt_uint16` and
`depthfmt_float32`, plus `depthfmt_none` (no depth).

When `writes_depth` is `true`, the callback may rewrite `depth.data` within the
valid tile region, honoring `depth.format`. The Hi-Z buffer (`depth.hiz_data`)
is owned by the rasterizer and the callback must not touch it.

## Depth and Hi-Z behavior

- Read-only passes (`writes_depth == false`) and the plain geometry-only path
  keep the existing Hi-Z acceleration.
- When a pass writes depth, the rasterizer invalidates that tile's Hi-Z bounds
  before any subsequent geometry dispatch. Later `LESS`/`LEQUAL` (and other)
  depth tests therefore produce exactly the same result as running with Hi-Z
  disabled; the per-pixel depth test stays authoritative. Subsequent geometry
  rebuilds tight Hi-Z bounds from the depth it writes.

## Capacity and configuration

Tile passes share the existing per-commit fixed-capacity stores from
`rasterizer_memory`:

- Each dispatch (geometry or tile pass) consumes one entry of `max_dispatches`.
- Each pass value consumes space in `pso_store_size`, with the same alignment
  and capacity checks used for PSOs.

No extra screen-sized storage is required. Both linear and Morton tile orders
are supported, and the existing `dispatch_pso`, `commit` (with or without a
final tile shader) and tile-shader interfaces are unchanged.

## Minimal example

See [`examples/09_TilePass/tile_pass_example.cpp`](../examples/09_TilePass/tile_pass_example.cpp)
for a complete windowless program that reduces a tile's visible depth and
adjusts its color between two geometry draws.
