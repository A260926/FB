# Voxel Space

![web demonstration](images/webdemo.gif)

# **[Web Demo of the Voxel Space Engine][project demo]**

## History

Let us go back to the year 1992. The CPUs were 1000 times slower than today and the acceleration via a GPU was unknown or unaffordable. 3D games were calculated exclusively on the CPU and the rendering engine rendered filled polygons with a single color.

![Game Gunship 2000 in 1991](images/gunship2000-1991.gif)
*Game Gunship 2000 published by MicroProse in 1991*

It was during that year [NovaLogic](http://www.novalogic.com/) published the game [Comanche](https://en.wikipedia.org/wiki/Comanche_(video_game_series)).

![Game Comanche in 1992](images/comanche-1992.gif)
*Game Comanche published by NovaLogic in 1992*

The graphics were breathtaking for the time being and in my opinion 3 years ahead of its time. You see many more details such as textures on mountains and valleys, and for the first time a neat shading and even shadows. Sure, it's pixelated, but all games in those years were pixelated.

## Render algorithm

[Comanche](https://en.wikipedia.org/wiki/Comanche_(video_game_series)) uses a technique called [Voxel Space](https://en.wikipedia.org/wiki/Voxel_Space), which is based on the same ideas like [ray casting](https://en.wikipedia.org/wiki/Ray_casting). Hence the Voxel Space engine is a 2.5D engine, it doesn't have all the levels of freedom that a regular 3D engine offers.

### Height map and color map

The easiest way to represent a terrain is through a height map and color map. For the game Comanche a 1024 \* 1024 one byte height map and a 1024 \* 1024 one byte color map is used which you can download on this site. These maps are periodic:

![periodic map](images/periodicmap.gif)

Such maps limit the terrain to "one height per position on the map" - Complex geometries such as buildings or trees are not possible to represent. However, a great advantage of the colormap is, that it already contains the shading and shadows. The Voxel Space engine just takes the color and doesn't have to compute illumination during the render process.

### Basic algorithm
For a 3D engine the rendering algorithm is amazingly simple. The Voxel Space engine rasters the height and color map and draws vertical lines. The following figure demonstrate this technique.

![Line by line](images/linebyline.gif)

 * Clear Screen.
 * To guarantee occlusion start from the back and render to the front. This is called painter algorithm.
 * Determine the line on the map, which corresponds to the same optical distance from the observer. Consider the field of view and the [perspective projection](https://en.wikipedia.org/wiki/3D_projection) (Objects are smaller farther away)
 * Raster the line so that it matches the number of columns of the screen.
 * Retrieve the height and color from the 2D maps corresponding of the segment of the line.
 * Perform the [perspective projection](https://en.wikipedia.org/wiki/3D_projection) for the height coordinate.
 * Draw a vertical line with the corresponding color with the height retrieved from the perspective projection.

The core algorithm contains in its simplest form only a few lines of code (python syntax):

```python
def Render(p, height, horizon, scale_height, distance, screen_width, screen_height):
    # Draw from back to the front (high z coordinate to low z coordinate)
    for z in range(distance, 1, -1):
        # Find line on map. This calculation corresponds to a field of view of 90°
        pleft  = Point(-z + p.x, -z + p.y)
        pright = Point( z + p.x, -z + p.y)
        # segment the line
        dx = (pright.x - pleft.x) / screen_width
        # Raster line and draw a vertical line for each segment
        for i in range(0, screen_width):
            height_on_screen = (height - heightmap[pleft.x, pleft.y]) / z * scale_height. + horizon
            DrawVerticalLine(i, height_on_screen, screen_height, colormap[pleft.x, pleft.y])
            pleft.x += dx

# Call the render function with the camera parameters:
# position, height, horizon line position,
# scaling factor for the height, the largest distance, 
# screen width and the screen height parameter
Render( Point(0, 0), 50, 120, 120, 300, 800, 600 )
```

### Add rotation

With the algorithm above we can only view to the north. A different angle needs a few more lines of code to rotate the coordinates.

![rotation](images/rotate.gif)

```python
def Render(p, phi, height, horizon, scale_height, distance, screen_width, screen_height):
    # precalculate viewing angle parameters
    var sinphi = math.sin(phi);
    var cosphi = math.cos(phi);

    # Draw from back to the front (high z coordinate to low z coordinate)
    for z in range(distance, 1, -1):

        # Find line on map. This calculation corresponds to a field of view of 90°
        pleft = Point(
            (-cosphi*z - sinphi*z) + p.x,
            ( sinphi*z - cosphi*z) + p.y)
        pright = Point(
            ( cosphi*z - sinphi*z) + p.x,
            (-sinphi*z - cosphi*z) + p.y)
        
        # segment the line
        dx = (pright.x - pleft.x) / screen_width
        dy = (pright.y - pleft.y) / screen_width

        # Raster line and draw a vertical line for each segment
        for i in range(0, screen_width):
            height_on_screen = (height - heightmap[pleft.x, pleft.y]) / z * scale_height. + horizon
            DrawVerticalLine(i, height_on_screen, screen_height, colormap[pleft.x, pleft.y])
            pleft.x += dx
            pleft.y += dy

# Call the render function with the camera parameters:
# position, viewing angle, height, horizon line position, 
# scaling factor for the height, the largest distance, 
# screen width and the screen height parameter
Render( Point(0, 0), 0, 50, 120, 120, 300, 800, 600 )
```

### More performance

There are of course a lot of tricks to achieve higher performance.

* Instead of drawing from back to the front we can draw from front to back. The advantage is, the we don't have to draw lines to the bottom of the screen every time because of occlusion. However, to guarantee occlusion we need an additional y-buffer. For every column, the highest y position is stored. Because we are drawing from the front to back, the visible part of the next line can only be larger then the highest line previously drawn.
* Level of Detail. Render more details in front but less details far away. 

![front to back rendering](images/fronttoback.gif)

```python
def Render(p, phi, height, horizon, scale_height, distance, screen_width, screen_height):
    # precalculate viewing angle parameters
    var sinphi = math.sin(phi);
    var cosphi = math.cos(phi);
    
    # initialize visibility array. Y position for each column on screen 
    ybuffer = np.zeros(screen_width)
    for i in range(0, screen_width):
        ybuffer[i] = screen_height

    # Draw from front to the back (low z coordinate to high z coordinate)
    dz = 1.
    z = 1.
    while z < distance
        # Find line on map. This calculation corresponds to a field of view of 90°
        pleft = Point(
            (-cosphi*z - sinphi*z) + p.x,
            ( sinphi*z - cosphi*z) + p.y)
        pright = Point(
            ( cosphi*z - sinphi*z) + p.x,
            (-sinphi*z - cosphi*z) + p.y)

        # segment the line
        dx = (pright.x - pleft.x) / screen_width
        dy = (pright.y - pleft.y) / screen_width

        # Raster line and draw a vertical line for each segment
        for i in range(0, screen_width):
            height_on_screen = (height - heightmap[pleft.x, pleft.y]) / z * scale_height. + horizon
            DrawVerticalLine(i, height_on_screen, ybuffer[i], colormap[pleft.x, pleft.y])
            if height_on_screen < ybuffer[i]:
                ybuffer[i] = height_on_screen
            pleft.x += dx
            pleft.y += dy

        # Go to next line and increase step size when you are far away
        z += dz
        dz += 0.2

# Call the render function with the camera parameters:
# position, viewing angle, height, horizon line position, 
# scaling factor for the height, the largest distance, 
# screen width and the screen height parameter
Render( Point(0, 0), 0, 50, 120, 120, 300, 800, 600 )
```

## Procedural clipmap terrain

`terrain-clipmap.js` adds an editable, streaming terrain that never allocates
a whole-world array. It is plain JavaScript builtins only and loads both as a
browser classic script (globals `TerrainSource` / `TerrainClipmap`) and as a
Node CommonJS module.

### TerrainSource and override coordinates

`new TerrainSource(sampleBase)` takes a deterministic `sampleBase(x, y)`. It
is only ever called with integer world coordinates, each returning
`{height, color}` where `height` is finite and `color` is a `uint32` in the
existing `0xAABBGGRR` layout. The base function is invoked only for grid
points that actually have to fill a cache cell.

`source.edit([{x, y, height, color}, ...])` is a sparse override cover:

* The whole batch is validated first; an illegal item throws `RangeError` and
  neither the source nor any subscribed view changes.
* An empty batch changes nothing. Duplicate coordinates keep the last item.
* The cover is keyed by the **raw, unwrapped world coordinates**. When the
  image maps are adapted as a base, only the read-only base wraps with
  `& (map.width-1)`; an edit at `(x, y)` always means world point `(x, y)` even
  past the map period, so it is still there after you leave and come back.
* One source can be subscribed to by many views.

### Point-sampled levels of detail

`new TerrainClipmap(source, {side, levels})` keeps exactly `side × side`
resident samples per level (`side` an odd integer ≥ 9, `levels` in 1..16).
Level `l` has grid spacing `2^l` and stores the source value directly at that
absolute world grid point — there is no regional averaging.

`moveTo(cx, cy)` centers every level on the camera. For spacing `s` and
`R = (side-1)/2`, the window's lower-left grid node is
`(floor(cx/s)-R, floor(cy/s)-R)` and spans `side` nodes. All level windows are
validated before anything is written; a window leaving `[-2^30, 2^30]` throws
`RangeError` and leaves the view untouched.

`sample(x, y)` reconstructs around the last `moveTo` center using the
Chebyshev radius `r = max(|x-cx|, |y-cy|)` and the boundary distances
`b_l = (R-2)·2^l`:

* `r ≤ b_0/2` uses level 0 alone.
* `b_l/2 < r < b_l` blends the bilinear value of level `l` with level `l+1`,
  weight `w = 2r/b_l - 1`. At `r = b_l` the coarser level is used directly
  (`w = 1`), which keeps the level boundary continuous.
* The last level has no coarser neighbour and is used directly.
* `r > b_last` returns `null` (nothing is drawn there).

Each level bilinearly interpolates `floor(x/s)`, `floor(y/s)` and their four
neighbours. Height and the four RGBA channels are interpolated as floats; only
the final colour is rounded per channel and clamped to `0..255`. Sampling never
calls the source.

### Ring-buffer cache lifecycle

* Every level is a fixed ring of typed arrays — `(side+1)²` slots — that is
  never moved or re-copied. Each slot stores the absolute grid identity
  (`gridX`, `gridY`) of the sample it holds. The one-cell "ghost" margin means
  an incoming strip wraps into a free slot instead of evicting the strip that
  just left, so walking back into a previous window performs **zero** source
  reads. Resident memory stays `O(side² × levels)`; no off-window history or
  full pyramid is retained.
* A move reads only the new/old window set difference (a subset when ghost
  slots already hold some difference points). The corner where two incoming
  strips intersect is read once. A single-axis span of `side` fills each
  resident slot exactly once.
* After `source.edit(...)`, before the next sample/render, every still
  subscribed view refreshes only resident slots whose absolute coordinate lies
  on that level's grid. Non-resident edits trigger no reads; they surface when
  the point later enters a window.
* `destroy()` unsubscribes a view; using it afterwards throws, while other
  views on the same source keep working. `source.subscriberCount()` reports
  live views, and `view.stats()` reports cumulative `reads`,
  `fillsByLevel`, `resident`, plus the `lastReads` / `lastFillsByLevel` of the
  most recent move or edit notification.

### Viewer integration

`VoxelSpace.html` keeps the original full-array mode and adds an optional
**Clipmap terrain** checkbox plus a **Raise ground under camera** button. The
production `Render` keeps its depth stepping, screen projection,
`DrawVerticalLine` and `hiddeny` occlusion; only the terrain fetch is replaced
by `view.sample`, and `null` samples are skipped so out-of-range columns keep
their sky. `UpdateCamera`'s ground clearance uses the same view. The periodic
image maps are wrapped as a read-only base, and switching maps destroys the old
view and source before building new ones. `installClipmap(source)` is the
public entry used to attach a custom procedural source (the headless example
and tests drive it this way).

Headless checks and the example need no window or downloaded images:

```
node task_support/run.js baseline   # original array Render
node task_support/run.js target     # module tests + clipmap example
node examples/clipmap.js out.ppm    # three P6 frames: initial, moved, edited
```

## Links

[Web Project demo][project demo] page

[Voxel terrain engine - an introduction](https://web.archive.org/web/20131113094653/http://www.codermind.com/articles/Voxel-terrain-engine-building-the-terrain.html)

[Personal website](http://www.simulationcorner.net)



## Maps
[color](maps/C1W.png),
[height](maps/D1.png)

![C1W.png](images/thumbnails/C1W.png)
![D1.png](images/thumbnails/D1.png)

[color](maps/C2W.png),
[height](maps/D2.png)

![C2W.png](images/thumbnails/C2W.png)
![D2.png](images/thumbnails/D2.png)

[color](maps/C3.png),
[height](maps/D3.png)

![C3.png](images/thumbnails/C3.png)
![D3.png](images/thumbnails/D3.png)

[color](maps/C4.png),
[height](maps/D4.png)

![C4.png](images/thumbnails/C4.png)
![D4.png](images/thumbnails/D4.png)

[color](maps/C5W.png),
[height](maps/D5.png)

![C5W.png](images/thumbnails/C5W.png)
![D5.png](images/thumbnails/D5.png)

[color](maps/C6W.png),
[height](maps/D6.png)

![C6W.png](images/thumbnails/C6W.png)
![D6.png](images/thumbnails/D6.png)

[color](maps/C7W.png),
[height](maps/D7.png)

![C7W.png](images/thumbnails/C7W.png)
![D7.png](images/thumbnails/D7.png)

[color](maps/C8.png),
[height](maps/D6.png)

![C8.png](images/thumbnails/C8.png)
![D6.png](images/thumbnails/D6.png)

[color](maps/C9W.png),
[height](maps/D9.png)

![C9W.png](images/thumbnails/C9W.png)
![D9.png](images/thumbnails/D9.png)

[color](maps/C10W.png),
[height](maps/D10.png)

![C10W.png](images/thumbnails/C10W.png)
![D10.png](images/thumbnails/D10.png)

[color](maps/C11W.png),
[height](maps/D11.png)

![C11W.png](images/thumbnails/C11W.png)
![D11.png](images/thumbnails/D11.png)

[color](maps/C12W.png),
[height](maps/D11.png)

![C12W.png](images/thumbnails/C12W.png)
![D11.png](images/thumbnails/D11.png)

[color](maps/C13.png),
[height](maps/D13.png)

![C13.png](images/thumbnails/C13.png)
![D13.png](images/thumbnails/D13.png)

[color](maps/C14.png),
[height](maps/D14.png)

![C14.png](images/thumbnails/C14.png)
![D14.png](images/thumbnails/D14.png)

[color](maps/C14W.png),
[height](maps/D14.png)

![C14W.png](images/thumbnails/C14W.png)
![D14.png](images/thumbnails/D14.png)

[color](maps/C15.png),
[height](maps/D15.png)

![C15.png](images/thumbnails/C15.png)
![D15.png](images/thumbnails/D15.png)

[color](maps/C16W.png),
[height](maps/D16.png)

![C16W.png](images/thumbnails/C16W.png)
![D16.png](images/thumbnails/D16.png)

[color](maps/C17W.png),
[height](maps/D17.png)

![C17W.png](images/thumbnails/C17W.png)
![D17.png](images/thumbnails/D17.png)

[color](maps/C18W.png),
[height](maps/D18.png)

![C18W.png](images/thumbnails/C18W.png)
![D18.png](images/thumbnails/D18.png)

[color](maps/C19W.png),
[height](maps/D19.png)

![C19W.png](images/thumbnails/C19W.png)
![D19.png](images/thumbnails/D19.png)

[color](maps/C20W.png),
[height](maps/D20.png)

![C20W.png](images/thumbnails/C20W.png)
![D20.png](images/thumbnails/D20.png)

[color](maps/C21.png),
[height](maps/D21.png)

![C21.png](images/thumbnails/C21.png)
![D21.png](images/thumbnails/D21.png)

[color](maps/C22W.png),
[height](maps/D22.png)

![C22W.png](images/thumbnails/C22W.png)
![D22.png](images/thumbnails/D22.png)

[color](maps/C23W.png),
[height](maps/D21.png)

![C23W.png](images/thumbnails/C23W.png)
![D21.png](images/thumbnails/D21.png)

[color](maps/C24W.png),
[height](maps/D24.png)

![C24W.png](images/thumbnails/C24W.png)
![D24.png](images/thumbnails/D24.png)

[color](maps/C25W.png),
[height](maps/D25.png)

![C25W.png](images/thumbnails/C25W.png)
![D25.png](images/thumbnails/D25.png)

[color](maps/C26W.png),
[height](maps/D18.png)

![C26W.png](images/thumbnails/C26W.png)
![D18.png](images/thumbnails/D18.png)

[color](maps/C27W.png),
[height](maps/D15.png)

![C27W.png](images/thumbnails/C27W.png)
![D15.png](images/thumbnails/D15.png)

[color](maps/C28W.png),
[height](maps/D25.png)

![C28W.png](images/thumbnails/C28W.png)
![D25.png](images/thumbnails/D25.png)

[color](maps/C29W.png),
[height](maps/D16.png)

![C29W.png](images/thumbnails/C29W.png)
![D16.png](images/thumbnails/D16.png)

## License

The software part of the repository is under the MIT license. Please read the license file for more information. Please keep in mind, that the Voxel Space technology might be still [patented](https://patents.justia.com/assignee/novalogic-inc) in some countries. The color and height maps are reverse engineered from the game Comanche and are therefore excluded from the license.

[project demo]: https://s-macke.github.io/VoxelSpace/VoxelSpace.html
