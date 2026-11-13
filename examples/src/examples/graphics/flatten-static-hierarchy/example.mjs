// @config
//
// Demonstrates flattenStaticHierarchy: a static assembly of pivots is loaded, then the pivots are
// folded away. The sheared local matrices produced by non-uniform scale and rotation are preserved
// by putting the promoted nodes into local matrix mode. Every box keeps its exact world transform
// while the scene graph becomes shallower. The camera orbits the result and one retained parent
// keeps rotating, showing that descendants still follow their parent.
//
// @flag ENGINE=debug

import {
    AppBase,
    AppOptions,
    CameraComponentSystem,
    Color,
    Entity,
    FILLMODE_FILL_WINDOW,
    GraphNode,
    LightComponentSystem,
    RESOLUTION_AUTO,
    RenderComponentSystem,
    TextureHandler,
    Vec3,
    createGraphicsDevice,
    flattenStaticHierarchy
} from 'playcanvas';

import { deviceType } from 'examples/context';

const canvas = /** @type {HTMLCanvasElement} */ (document.getElementById('application-canvas'));
window.focus();

const gfxOptions = {
    deviceTypes: [deviceType]
};

const device = await createGraphicsDevice(canvas, gfxOptions);
device.maxPixelRatio = Math.min(window.devicePixelRatio, 2);

const createOptions = new AppOptions();
createOptions.graphicsDevice = device;
createOptions.componentSystems = [RenderComponentSystem, CameraComponentSystem, LightComponentSystem];
createOptions.resourceHandlers = [TextureHandler];

const app = new AppBase(canvas);
app.init(createOptions);
app.start();

app.setCanvasFillMode(FILLMODE_FILL_WINDOW);
app.setCanvasResolution(RESOLUTION_AUTO);

const resize = () => app.resizeCanvas();
window.addEventListener('resize', resize);
app.on('destroy', () => window.removeEventListener('resize', resize));

app.scene.ambientLight = new Color(0.3, 0.3, 0.3);

// Build a static assembly: a non-uniformly scaled root, with nested pivots and boxes hanging off
// them. The pivots carry rotations, so the promoted boxes need local matrices containing shear.
// Pivots are plain GraphNodes: flattenStaticHierarchy rejects Entity subclasses as pivots.
const assembly = new Entity('assembly');
assembly.setLocalScale(2, 1, 1.5);
assembly.setLocalEulerAngles(0, 20, 0);
app.root.addChild(assembly);

const pivots = [];

const addPivot = (parent, name, position, euler) => {
    const pivot = new GraphNode(name);
    pivot.setLocalPosition(position);
    pivot.setLocalEulerAngles(euler);
    parent.addChild(pivot);
    pivots.push(pivot);
    return pivot;
};

const addBox = (parent, name, position, scale) => {
    const box = new Entity(name);
    box.addComponent('render', { type: 'box' });
    box.setLocalPosition(position);
    box.setLocalScale(scale);
    parent.addChild(box);
    return box;
};

const pivotA = addPivot(assembly, 'pivotA', new Vec3(0, 0, 0), new Vec3(0, 30, 15));
const pivotB = addPivot(pivotA, 'pivotB', new Vec3(1.5, 0.5, -1), new Vec3(45, 0, 0));
addBox(pivotA, 'boxA', new Vec3(2, 0, 1), new Vec3(1, 2, 0.5));
addBox(pivotB, 'boxB', new Vec3(1, 0, 0), new Vec3(1.5, 1, 0.5));
addBox(pivotB, 'boxC', new Vec3(0, 2, 0), new Vec3(0.75, 0.75, 2));

// Record the world transforms before flattening, then fold away both pivots.
const boxes = ['boxA', 'boxB', 'boxC'].map(name => assembly.findByName(name));
const before = boxes.map(node => node.getWorldTransform().clone());

flattenStaticHierarchy(assembly, pivots);

boxes.forEach((node, i) => {
    const world = node.getWorldTransform();
    let maxError = 0;
    for (let j = 0; j < 16; j++) {
        maxError = Math.max(maxError, Math.abs(world.data[j] - before[i].data[j]));
    }
    console.log(`${node.name}: world transform preserved (max error ${maxError.toExponential(2)}), local matrix mode: ${node.isLocalMatrix}`);
});

// Add a camera looking at the assembly and an omni light to shape the boxes.
const camera = new Entity('camera');
camera.addComponent('camera', {
    clearColor: new Color(0.1, 0.12, 0.16)
});
app.root.addChild(camera);
camera.setPosition(0, 6, 14);
camera.lookAt(0, 1, 0);

const light = new Entity('light');
light.addComponent('light', { type: 'omni', range: 200 });
light.setPosition(5, 15, 8);
app.root.addChild(light);

// Spin the retained ancestor. The boxes promoted into local matrix mode still follow it, while
// the removed pivots no longer take part in the hierarchy sync.
let angle = 0;
app.on('update', (dt) => {
    angle += dt * 0.2;
    assembly.setLocalEulerAngles(0, 20 + Math.sin(angle) * 15, 0);
});
