## Getting Started

Here's an example for creating a 128x128 array of OpenSimplex2 noise

```cpp
// Create and configure FastNoise object
FastNoiseLite noise;
noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);

// Gather noise data
std::vector<float> noiseData(128 * 128);
int index = 0;

for (int y = 0; y < 128; y++)
{
    for (int x = 0; x < 128; x++)
    {
        noiseData[index++] = noise.GetNoise((float)x, (float)y);
    }
}

// Do something with this data...
```

## Exact cellular (Voronoi stone) area bake

Point sampling the cellular noise flickers on thin seams and low-resolution
bakes. `BakeCellular2D` instead integrates the real geometry: for every pixel
rectangle it returns the exact stone coverage and the area-weighted mean
greyscale, with the seam counted as black. No multi-sampling is involved.

```cpp
FastNoiseLite noise(1337);
noise.SetFrequency(0.1f);
noise.SetCellularJitter(1.0f);   // only seed, frequency and jitter matter

const unsigned width = 256, height = 256;
std::vector<FastNoiseLite::CellularPixel2D> pixels(width * height);

// Pixel (i, j) covers
// [x0 + i*dx, x0 + (i+1)*dx] x [y0 + j*dy, y0 + (j+1)*dy],
// x varying fastest. seam is the gap width in noise coordinates.
const double seam = 0.1;
noise.BakeCellular2D(/*x0*/ 0.0, /*y0*/ 0.0,
                     /*dx*/ 0.25, /*dy*/ 0.25,
                     width, height, seam, pixels.data());

for (const auto& pixel : pixels)
{
    // pixel.coverage: stone area / pixel area, in [0, 1]
    // pixel.value:    mean greyscale over the whole pixel
    //                 (the black seam is included, not divided out)
}
```

A stone is the Euclidean Voronoi cell of a feature eroded by `seam / 2`.
For feature `a` and every other feature `b`, a point `p` belongs to the
stone of `a` when

```
|p - b|^2 - |p - a|^2 >= seam * |b - a|
```

Each inequality is a half-plane, so a stone is the convex polygon obtained
by intersecting them; the polygon is clipped against each pixel rectangle
and integrated with the signed-area formula. Features that land on the same
position keep the lexicographically smallest lattice cell identity.

### Candidate set bound

The original 3x3 neighbourhood only serves scalar sampling and cannot
guarantee full coverage for high jitter or large pixels. The bake derives a
finite, conservative candidate range instead of a fixed radius:

1. A feature can move at most `J = 0.43701595 * |jitter|` from its integer
   lattice point. Every point of the plane has an integer lattice point
   within `sqrt(0.5)`, hence some feature within `D = sqrt(0.5) + J`.
2. A feature `a` whose stone intersects the request rectangle is the
   closest feature at that point, so it is within `D` of the rectangle.
3. For a competitor `b` to clip `a`'s eroded boundary, the eroded
   bisector line must reach the disk `B(a, D)`. The original bisector
   passes through the midpoint of `a` and `b`; erosion moves it towards
   `a` by `seam / 2`, so its distance from `a` is
   `(|a - b| - seam) / 2`. Reaching the disk requires
   `(|a - b| - seam) / 2 <= D`, i.e.
   `|a - b| <= 2D + seam <= 2D + 2`.
4. Every relevant feature is consequently within
   `|b - request| <= |b - a| + |a - request| <= 3D + 2` of the request.
   Lattice cells (not features) are enumerated over the rectangle expanded
   by `E = 3D + 2 + J`, which is finite for any finite request and covers
   both the cells entering the rectangle and every competitor that can
   clip their eroded boundaries.

The stone polygons are built once per bake from this shared candidate set
and then projected onto the pixels they intersect, so candidate pairs are
not re-traversed per texel. Baking a rectangle and baking the same region
as tiled sub-rectangles produce identical pixels, and scaling world
coordinates inversely to the frequency cancels out exactly.
