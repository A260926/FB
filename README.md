# PlayCanvas Engine

[![NPM Version](https://img.shields.io/npm/v/playcanvas)](https://www.npmjs.com/package/playcanvas)
[![NPM Downloads](https://img.shields.io/npm/dw/playcanvas)](https://npmtrends.com/playcanvas)
[![License](https://img.shields.io/npm/l/playcanvas)](https://github.com/playcanvas/engine/blob/main/LICENSE)
[![Discord](https://img.shields.io/badge/Discord-5865F2?style=flat&logo=discord&logoColor=white&color=black)](https://discord.gg/RSaMRzg)
[![Reddit](https://img.shields.io/badge/Reddit-FF4500?style=flat&logo=reddit&logoColor=white&color=black)](https://www.reddit.com/r/PlayCanvas)
[![X](https://img.shields.io/badge/X-000000?style=flat&logo=x&logoColor=white&color=black)](https://x.com/intent/follow?screen_name=playcanvas)

| [User Manual](https://developer.playcanvas.com/user-manual/engine/) | [API Reference](https://api.playcanvas.com/engine/) | [Examples](https://playcanvas.com/examples/) | [Blog](https://blog.playcanvas.com) | [Forum](https://forum.playcanvas.com) |

PlayCanvas is an open-source game engine built on WebGL2 and WebGPU. Use it to create interactive 3D apps, games and visualizations that run in any browser on any device.

[English](https://github.com/playcanvas/engine/blob/main/README.md)
[中文](https://github.com/playcanvas/engine/blob/main/README-zh.md)
[日本語](https://github.com/playcanvas/engine/blob/main/README-ja.md)
[한글](https://github.com/playcanvas/engine/blob/main/README-kr.md)

## Install

```sh
npm install playcanvas
```

Or scaffold a full project in seconds with [`create-playcanvas`](https://github.com/playcanvas/create-playcanvas):

```sh
npm create playcanvas@latest
```

## Usage

Here's a super-simple Hello World example - a spinning cube!

```js
import {
  Application,
  Color,
  Entity,
  FILLMODE_FILL_WINDOW,
  RESOLUTION_AUTO
} from 'playcanvas';

const canvas = document.createElement('canvas');
document.body.appendChild(canvas);

const app = new Application(canvas);

// fill the available space at full resolution
app.setCanvasFillMode(FILLMODE_FILL_WINDOW);
app.setCanvasResolution(RESOLUTION_AUTO);

// ensure canvas is resized when window changes size
window.addEventListener('resize', () => app.resizeCanvas());

// create box entity
const box = new Entity('cube');
box.addComponent('render', {
  type: 'box'
});
app.root.addChild(box);

// create camera entity
const camera = new Entity('camera');
camera.addComponent('camera', {
  clearColor: new Color(0.1, 0.2, 0.3)
});
app.root.addChild(camera);
camera.setPosition(0, 0, 3);

// create directional light entity
const light = new Entity('light');
light.addComponent('light');
app.root.addChild(light);
light.setEulerAngles(45, 0, 0);

// rotate the box according to the delta time since the last frame
app.on('update', dt => box.rotate(10 * dt, 20 * dt, 30 * dt));

app.start();
```

Want to play with the code yourself? Edit it on [CodePen](https://codepen.io/playcanvas/pen/NPbxMj).

A full guide to setting up a local development environment based on the PlayCanvas Engine can be found [here](https://developer.playcanvas.com/user-manual/engine/standalone/).

## Instanced skin

A single mesh shared by many characters can carry several live skeletons in one instanced draw
call, without baking the animation into a per-vertex texture. Each character samples its own
animation, runs its own IK and moves its own parent node every frame.

```js
import { MeshInstance } from 'playcanvas';

meshInstance.setInstancedSkin([
    { skinInstance: skinInstanceA, node: rootA },
    { skinInstance: skinInstanceB, node: rootB }
]);
```

- `MeshInstance#setInstancedSkin` takes the entries in draw order. Part of the public API
  since 2.25.0.
- `InstancedSkin` is the atlas and instance buffer the mode owns. It is created by
  `setInstancedSkin` and is not constructed directly.

### Interface

`entries` lists `{ skinInstance, node }` records in draw order:

- `skinInstance` is a `SkinInstance` whose `bones` are already bound.
- `node` is the coordinate root of that copy of the character mesh.
- The same `skinInstance` may appear more than once, even with different nodes.
- The record array is copied. It can be released or changed after the call; only the borrowed
  nodes, bones and skin are read afterwards.

`setInstancedSkin` returns `this`. Passing an empty array is valid and leaves zero instances.
Passing `null` disables the mode. While the mode is enabled, the older setters reject conflicting
assignments: `skinInstance`, `morphInstance`, `setInstancing` and `setMultiDraw` throw a
`RangeError`.

The mode only supports four-weight triangle meshes with a `StandardMaterial`. Morphing, batching,
indirect drawing, multi-draw and custom instance buffers cannot be combined with it, nor can a mesh
or material whose UV sets 6 and 7 occupy the default instance attribute locations. These
combinations throw a `RangeError` rather than being silently ignored.

`Material`/`mesh` and inverse bind pose are shared. Every entry's skin must be exactly the mesh's
skin, and every entry must have the same non-zero number of bones. Weights must be finite and
non-negative, one point's weights must sum to 1 within `1e-5`, and a positive weight must name a
valid bone.

Face orientation follows the existing rules: each entry's node must have a positive determinant
relative to the draw owner's node. The owner and the entries can reflect together, but two
characters with different chirality cannot be mixed in one instanced skin.

### Ownership

The mode borrows the `SkinInstance`s, their bones, the mesh and the material. It never rewrites a
borrowed `matrices`, `matrixPalette`, animation state or resources. It owns only the pose atlas
texture and the instance matrix vertex buffer, and releases exactly those on replacement or
disable (once each). `MeshInstance.destroy` releases the mode's resources but not the borrowed
objects; the mesh's reference counting is unchanged.

`InstancedSkin` exposes read-only `boneCount`, `count`, `boneTexture` and `matrixPalette` for
debugging. The palette includes any tail padding of the texture. It also exposes the instance
matrix `vertexBuffer`.

### Stages

1. **Pose evaluation.** Before culling, `Renderer.updateCpuSkinMatrices` evaluates every entry: the
   bone matrices into the atlas, the instance matrices into the buffer, and the world bounds from
   the mesh's bone influence boxes. No vertex is skinned on the CPU to find the bounds.
2. **Culling.** `MeshInstance.aabb` returns the bounds evaluated in the same step, so a character
   is never culled with a stale pose.
3. **GPU upload.** `Renderer.updateGpuSkinMatrices` uploads exactly the pose and instance data the
   previous step produced, once per change.
4. **Draw binding.** `Renderer.setSkinning` binds the atlas and the bone count for the forward,
   shadow and depth passes, and clears the single-bone pose map, so switching back to normal
   skinning leaves no atlas uniform behind.

The shader finds each instance's bone block by the instance index. The atlas is laid out by
instance and then by bone, using the 12 float row layout of `SkinInstance.matrixPalette`, three
RGBA32F texels per bone. Its width is a multiple of 3, so no bone crosses a row, and both
dimensions stay within `device.maxTextureSize`.

Each entry's bone matrix is `inverse(node.world) * bone.world * inverseBind`, and its instance
matrix is `inverse(meshInstance.node.world) * node.world`. The vertex shader therefore computes
`model * instance * skin`, so the world transform is applied exactly once. This is implemented for
both GLSL and WGSL, with an independent variant and define so the old normal skin, matrix
instancing and shader cache keys keep working unchanged.

Run `bash task_support/run.sh target` to run the target tests and the example, followed by the
normal single-skin baseline.

## Features

PlayCanvas is a fully-featured game engine.

* 🧊 **Graphics** - Advanced 2D + 3D graphics engine built on WebGL2 & WebGPU
* 💠 **Gaussian Splatting** - First-class support for loading and rendering [3D Gaussian Splats](https://developer.playcanvas.com/user-manual/graphics/gaussian-splatting/)
* 🥽 **XR** - Built-in support for immersive AR and VR experiences via [WebXR](https://developer.playcanvas.com/user-manual/xr/)
* ⚛️ **Physics** - Full integration with 3D rigid-body physics engine [ammo.js](https://github.com/kripken/ammo.js)
* 🏃 **Animation** - Powerful state-based animations for characters and arbitrary scene properties
* 🎮 **Input** - Mouse, keyboard, touch and gamepad APIs
* 🔊 **Sound** - 3D positional sounds built on the Web Audio API
* 📦 **Assets** - Asynchronous streaming system built on [glTF 2.0](https://www.khronos.org/gltf/), [Draco](https://google.github.io/draco/) and [Basis](https://github.com/BinomialLLC/basis_universal) compression
* 📜 **Scripts** - Write game behaviors in TypeScript or JavaScript

## Developing with AI

Coding agents: start from the [Engine guides index](https://developer.playcanvas.com/user-manual/engine/llms.txt) and the [Engine API index](https://api.playcanvas.com/engine/llms.txt), which link a Markdown version of every page. [Developing with AI](https://developer.playcanvas.com/user-manual/engine/developing-with-ai/) explains how to set up an agent on a project.

## Ecosystem

Build with PlayCanvas your way:

| Package | Description |
| ------- | ----------- |
| [`playcanvas`](https://www.npmjs.com/package/playcanvas) | Core engine (you are here) |
| [`@playcanvas/react`](https://www.npmjs.com/package/@playcanvas/react) | React renderer for PlayCanvas |
| [`@playcanvas/web-components`](https://www.npmjs.com/package/@playcanvas/web-components) | Declarative 3D via Custom Elements |
| [`create-playcanvas`](https://www.npmjs.com/package/create-playcanvas) | Project scaffolding CLI |
| [PlayCanvas Editor](https://github.com/playcanvas/editor) | Browser-based visual editor |

## Project Showcase

[Many games and apps](https://github.com/playcanvas/awesome-playcanvas) have been published using the PlayCanvas engine. Here is a small selection:

[![Seemore](https://s3-eu-west-1.amazonaws.com/images.playcanvas.com/projects/14705/319531/O4J4VU-image-25.jpg)](https://playcanv.as/p/MflWvdTW/) [![After The Flood](https://s3-eu-west-1.amazonaws.com/images.playcanvas.com/projects/14928/440410/98554E-image-25.jpg)](https://playcanv.as/p/44MRmJRU/) [![Casino](https://s3-eu-west-1.amazonaws.com/images.playcanvas.com/projects/14928/349824/U88HJQ-image-25.jpg)](https://playcanv.as/p/LpmXGUe6/)  
[![Swooop](https://s3-eu-west-1.amazonaws.com/images.playcanvas.com/projects/12/4763/TKYXB8-image-25.jpg)](https://playcanv.as/p/JtL2iqIH/) [![dev Archer](https://s3-eu-west-1.amazonaws.com/images.playcanvas.com/projects/12/415995/10A5A9-image-25.jpg)](https://playcanv.as/p/JERg21J8/) [![Gaussian Splat Statues](https://s3-eu-west-1.amazonaws.com/images.playcanvas.com/projects/12/1224723/266D9C-image-25.jpg)](https://playcanv.as/p/cLkf99ZV/)  
[![Car](https://s3-eu-west-1.amazonaws.com/images.playcanvas.com/projects/12/347824/7ULQ3Y-image-25.jpg)](https://playcanv.as/p/RqJJ9oU9/) [![Star-Lord](https://s3-eu-west-1.amazonaws.com/images.playcanvas.com/projects/12/333626/BGQN9H-image-25.jpg)](https://playcanv.as/p/SA7hVBLt/) [![Global Illumination](https://s3-eu-west-1.amazonaws.com/images.playcanvas.com/projects/4373/625081/6AB32D-image-25.jpg)](https://playcanv.as/p/ZV4PW6wr/ )

You can see more games on the [PlayCanvas website](https://playcanvas.com/explore).

## Users

PlayCanvas is used by leading companies in video games, advertising and visualization such as:  
**Animech, Arm, BMW, Disney, Facebook, Famobi, Funday Factory, IGT, King, Miniclip, Leapfrog, Mojiworks, Mozilla, Nickelodeon, Nordeus, NOWWA, PikPok, PlaySide Studios, Polaris, Product Madness, Samsung, Snap, Spry Fox, Zeptolab, Zynga**

## How to build

Ensure you have [Node.js 18+](https://nodejs.org) installed. Then, install all of the required Node.js dependencies:

```sh
npm install
```

Now you can run various build options:

| Command | Description | Outputs To |
| ------- | ----------- | ---------- |
| `npm run build` | Build all engine flavors and type declarations | `build` |
| `npm run docs` | Build engine [API reference docs](https://api.playcanvas.com/engine/) | `docs` |
