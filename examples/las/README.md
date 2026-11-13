# Simple LAS(LiDAR) particle rendering example with GUI support(bullet3's OpenGLWindow + ImGui).

![](../../images/las.png)

## Coordinates

Right-handed coorinate, Y up, counter clock-wise normal definition.

## Requirements

* cmake(PDAL) or premake5(liblas)
* OpenGL 2.x
* pdal(Point Data Abstraction layer. recommended) or liblas
* lastools(laszip) (optional)

## TODO

* [x] Color

## Build on Linux

### PDAL

Install pdal(you can use apt for Ubuntu).

Then,

```
$ mkdir build
$ cd build
$ cmake ..
$ make
```

### liblas

liblas support is deprecated, since PDAL is now recommended library to load las.
(And you'll face some Boost problem if you build liblas from source)

Install liblas https://www.liblas.org/

Then,

    $ premake5 gmake
    $ make

## Build on MacOSX

Install liblas using brew

    $ brew install liblas
Then,

    $ premake5 gmake
    $ make

Please note that `libas` installed with brew does not support compreession(LAZ), thus if you want to use laz data, you must first decompress laz using laszip by building http://www.laszip.org/

## Usage

Edit `config.json`, then run `lasrender`

### Mouse operation

* left mouse = rotate
* shift + left mouse = translate
* tab + left mouse = dolly(Z axis)

## Licenses

* btgui3 : zlib license.
* glew : Modified BSD, MIT license.
* picojson : 2-clause BSD license. See picojson.h for more details.
* ImGui : MIT license.
* stb : Public domain. See stb_*.h for more details.

