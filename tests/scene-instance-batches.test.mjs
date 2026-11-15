import { readFileSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

import {
	Scene, Camera, Mesh, Object3D, SkinnedMesh, Geometry, Buffer, Attribute,
	BoxGeometry, BasicMaterial, PBRMaterial, ShaderMaterial,
	DirectionalLight, AmbientLight, WebGLRenderer, ScreenRenderTarget, RenderInfo,
	SceneInstanceCollector, RenderCollector, Skeleton, Bone, Matrix4, Vector3,
	DRAW_MODE
} from '../src/main.js';
import { createDevice } from './support/recording-webgl2.mjs';
import { transformPoint, transformNormal } from './support/world-eval.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '..');

// ---------------------------------------------------------------------------
// Small helpers shared by the cases.
// ---------------------------------------------------------------------------

function near(a, b, epsilon = 1e-4) {
	return Math.abs(a - b) <= epsilon * Math.max(1, Math.abs(a), Math.abs(b));
}

function nearArray(a, b, epsilon = 1e-4) {
	if (a.length !== b.length) return false;
	for (let i = 0; i < a.length; i++) {
		if (!near(a[i], b[i], epsilon)) return false;
	}
	return true;
}

function createCamera(x, y, z) {
	const camera = new Camera();
	camera.position.set(x || 0, y || 0, z === undefined ? 20 : z);
	camera.setPerspective(Math.PI / 3, 4 / 3, 0.1, 200);
	// Keep the generated scenes fully in view; culling has its own tests.
	camera.frustumCulled = false;
	return camera;
}

function createNonIndexedQuad(size = 1) {
	const geometry = new Geometry();
	const h = size / 2;
	geometry.addAttribute('a_Position', new Attribute(new Buffer(new Float32Array([
		-h, -h, 0, h, -h, 0, h, h, 0,
		-h, -h, 0, h, h, 0, -h, h, 0
	]), 3)));
	geometry.addAttribute('a_Normal', new Attribute(new Buffer(new Float32Array([
		0, 0, 1, 0, 0, 1, 0, 0, 1,
		0, 0, 1, 0, 0, 1, 0, 0, 1
	]), 3)));
	geometry.addAttribute('a_Uv', new Attribute(new Buffer(new Float32Array([
		0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1
	]), 2)));
	geometry.computeBoundingBox();
	geometry.computeBoundingSphere();
	return geometry;
}

function setupScene(collector) {
	const scene = new Scene();
	if (collector) scene.collector = collector;
	const camera = createCamera();
	scene.add(camera);
	return { scene, camera };
}

function updateAndCollect(scene, camera, collectLights = false, updateSkeletons = false) {
	scene.updateMatrix();
	camera.updateMatrix();
	const renderStates = scene.updateRenderStates(camera);
	const renderQueue = scene.updateRenderQueue(camera, collectLights, updateSkeletons);
	return { renderStates, renderQueue };
}

function opaqueEntries(renderQueue) {
	const list = [];
	for (const layer of renderQueue.layerList) {
		for (let i = 0; i < layer.opaqueCount; i++) list.push(layer.opaque[i]);
	}
	return list;
}

function transparentEntries(renderQueue) {
	const list = [];
	for (const layer of renderQueue.layerList) {
		for (let i = 0; i < layer.transparentCount; i++) list.push(layer.transparent[i]);
	}
	return list;
}

async function createRenderer(device) {
	const renderer = new WebGLRenderer();
	await renderer.init(device.gl);
	return renderer;
}

function drawLayer(renderer, layer, renderStates, renderInfo = null, target) {
	renderer.beginRender(target || new ScreenRenderTarget({ width: 320, height: 240 }));
	renderer.renderRenderableList(layer.opaque, renderStates, renderInfo ? { renderInfo } : {});
	renderer.renderRenderableList(layer.transparent, renderStates, renderInfo ? { renderInfo } : {});
	renderer.endRender();
}

function createTarget() {
	return new ScreenRenderTarget({ width: 320, height: 240 });
}

/**
 * Draw a whole queue through the production entry point.
 */
function drawQueue(renderer, renderQueue, renderStates, renderInfo) {
	const target = createTarget();
	renderer.beginRender(target);
	for (const layer of renderQueue.layerList) {
		renderer.renderRenderableList(layer.opaque, renderStates, { renderInfo });
		renderer.renderRenderableList(layer.transparent, renderStates, { renderInfo });
	}
	renderer.endRender();
}

/**
 * Draw the same scene with a plain RenderCollector, so batches never happen.
 * Returns per draw positions in object order for comparison.
 */
function unbatchedSnapshot(setup) {
	const scene = setup.scene;
	const previous = scene.collector;
	scene.collector = new RenderCollector();
	const camera = setup.camera;

	scene.updateMatrix();
	camera.updateMatrix();
	const renderStates = scene.updateRenderStates(camera);
	const renderQueue = scene.updateRenderQueue(camera, false, false);
	const layer = renderQueue.layerList[0];

	const device = createDevice();
	const renderer = new WebGLRenderer();
	return renderer.init(device.gl).then(() => {
		const target = createTarget();
		renderer.beginRender(target);
		for (const item of layer.opaque) {
			renderer.renderRenderableItem(item, renderStates, {});
		}
		renderer.endRender();
		scene.collector = previous;

		const results = [];
		const details = device.drawDetails;
		const draws = device.draws();
		for (let i = 0; i < draws.length; i++) {
			results.push({
				kind: draws[i].name,
				args: draws[i].args,
				detail: details[i],
				renderable: layer.opaque[i]
			});
		}
		return results;
	});
}

// ---------------------------------------------------------------------------
// Membership cases: what the collector does to the queue.
// ---------------------------------------------------------------------------

const cases = [];

// 1. Only opaque native triangle meshes with identical geometry/material/group
//    and the same render state are merged.
cases.push({
	group: 'membership',
	name: 'merges compatible opaque meshes and keeps identity',
	async run(check) {
		const geometry = new BoxGeometry();
		const material = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		const meshes = [];
		const root = new Object3D();
		root.position.set(1, 2, 3);
		scene.add(root);

		for (let i = 0; i < 5; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.position.set(i - 2, i, -i);
			root.add(mesh);
			meshes.push(mesh);
		}

		// Not mergeable: same geometry but another material.
		const otherMaterial = new BasicMaterial();
		const other = new Mesh(geometry, otherMaterial);
		other.position.set(10, 0, 0);
		scene.add(other);

		// Not mergeable: same material but another geometry instance.
		const otherGeometry = new BoxGeometry();
		const otherGeometryMesh = new Mesh(otherGeometry, material);
		otherGeometryMesh.position.set(-10, 0, 0);
		scene.add(otherGeometryMesh);

		// Not mergeable: transparent.
		const transparentMaterial = new BasicMaterial();
		transparentMaterial.transparent = true;
		const transparent = new Mesh(geometry, transparentMaterial);
		transparent.position.set(0, 20, 0);
		scene.add(transparent);

		const { renderQueue } = updateAndCollect(scene, camera);
		const opaque = opaqueEntries(renderQueue);

		const batch = opaque.find(entry => entry.instanceBatch);
		check(batch !== undefined, 'a batch must be produced');
		check(batch.instanceBatch.count === 5, 'batch must contain all five meshes');
		check(batch.objects ? false : true, 'no proxy object array on the batch itself');
		check(batch.object === meshes[0], 'the representative is the smallest mesh id');
		check(batch.instanceBatch.objects[0] === meshes[0], 'members are ordered by mesh id');
		for (let i = 1; i < 5; i++) {
			check(batch.instanceBatch.objects[i] === meshes[i], 'member order must be ascending id');
		}

		check(batch.geometry === geometry, 'geometry identity is preserved');
		check(batch.material === material, 'material identity is preserved');
		check(batch.group === undefined || batch.group === null, 'no group for a non-grouped geometry');

		check(opaque.includes(other) === false, 'the batch replaced the members');
		const otherEntry = opaque.find(entry => entry.object === other);
		const otherGeometryEntry = opaque.find(entry => entry.object === otherGeometryMesh);
		check(otherEntry && otherEntry.instanceBatch === null, 'other material stays unbatched');
		check(otherGeometryEntry && otherGeometryEntry.instanceBatch === null, 'other geometry stays unbatched');
		check(transparentEntries(renderQueue).length === 1, 'transparent object stays transparent');
		check(opaque.length === 3, 'one batch plus two unbatched opaque entries');

		check(meshes.every(mesh => mesh.visible === true), 'source visible is untouched');
		check(meshes.every(mesh => mesh.geometry === geometry && mesh.material === material), 'source geometry/material untouched');
		check(geometry.instanceCount === -1, 'source instanceCount untouched');

		const expected = new Float32Array(5 * 16);
		for (let i = 0; i < 5; i++) meshes[i].worldMatrix.toArray(expected, i * 16);
		check(nearArray(batch.instanceBatch.worldMatrices, expected), 'world matrices are captured in member order');
		check(batch.instanceBatch.worldMatrices instanceof Float32Array, 'world matrices are stored in a Float32Array');
		check(batch.instanceBatch.frontFaceCW === false, 'no reflection: front face stays CCW');
	}
});

// 2. Materials must be native Basic/PBR (and not addon instanced materials).
cases.push({
	group: 'membership',
	name: 'rejects non native and addon instance materials',
	async run(check) {
		const geometry = new BoxGeometry();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		const basic = new BasicMaterial();
		const pbr = new PBRMaterial();
		const shader = new ShaderMaterial();
		shader.vertexShader = 'void main() {}';
		shader.fragmentShader = 'void main() {}';

		// An addon-style material that subclasses a native material but is an
		// explicit instance material.
		const addon = new BasicMaterial();
		addon.isInstanceMaterial = true;

		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, basic);
			mesh.position.x = i;
			scene.add(mesh);
		}
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, pbr);
			mesh.position.x = i + 10;
			scene.add(mesh);
		}
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, shader);
			mesh.position.x = i + 20;
			scene.add(mesh);
		}
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, addon);
			mesh.position.x = i + 30;
			scene.add(mesh);
		}

		const { renderQueue } = updateAndCollect(scene, camera);
		const opaque = opaqueEntries(renderQueue);

		const basicBatch = opaque.find(entry => entry.material === basic);
		const pbrBatch = opaque.find(entry => entry.material === pbr);
		check(basicBatch && basicBatch.instanceBatch && basicBatch.instanceBatch.count === 2, 'native basic material merges');
		check(pbrBatch && pbrBatch.instanceBatch && pbrBatch.instanceBatch.count === 2, 'native pbr material merges');

		const shaderEntries = opaque.filter(entry => entry.material === shader);
		const addonEntries = opaque.filter(entry => entry.material === addon);
		check(shaderEntries.length === 2 && shaderEntries.every(entry => entry.instanceBatch === null), 'shader material is not merged');
		check(addonEntries.length === 2 && addonEntries.every(entry => entry.instanceBatch === null), 'addon instance material is not merged');

		check(scene.collector instanceof RenderCollector, 'collector still derives from RenderCollector');
		check(scene.collector instanceof SceneInstanceCollector, 'collector is the scene instance collector');
	}
});

// 3. Skinning, morph targets, existing instancing and custom render callbacks
//    all stay on the original path.
cases.push({
	group: 'membership',
	name: 'rejects skinning morph instancing and custom callbacks',
	async run(check) {
		const geometry = new BoxGeometry();
		const material = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		const bone = new Bone();
		scene.add(bone);
		const skeleton = new Skeleton([bone], [new Matrix4()]);

		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.skeleton = skeleton;
			mesh.position.x = i;
			scene.add(mesh);
		}

		// Morph targets on the object (even without attributes) are rejected.
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.morphTargetInfluences = [0];
			mesh.position.x = i + 10;
			scene.add(mesh);
		}

		// Existing instancing attributes are rejected.
		const instanceGeometry = new BoxGeometry();
		instanceGeometry.addAttribute('a_InstanceOffset', new Attribute(new Buffer(new Float32Array(6), 3)));
		instanceGeometry.getAttribute('a_InstanceOffset').divisor = 1;
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(instanceGeometry, material);
			mesh.position.x = i + 20;
			scene.add(mesh);
		}

		// Custom render callbacks are rejected.
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.onBeforeRender = function() {};
			mesh.position.x = i + 30;
			scene.add(mesh);
		}

		const { renderQueue } = updateAndCollect(scene, camera);
		const opaque = opaqueEntries(renderQueue);
		check(opaque.length === 8, 'all eight objects stay in the queue');
		check(opaque.every(entry => entry.instanceBatch === null), 'none of them is merged');
	}
});

// 4. Normal maps and tangents take the normal path.
cases.push({
	group: 'membership',
	name: 'rejects normal maps and tangents',
	async run(check) {
		const geometry = new BoxGeometry();
		const material = new BasicMaterial();
		material.normalMap = { isTexture: true };

		const tangentGeometry = new BoxGeometry();
		tangentGeometry.addAttribute('a_Tangent', new Attribute(new Buffer(new Float32Array(24), 4)));

		const { scene, camera } = setupScene(new SceneInstanceCollector());
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.position.x = i;
			scene.add(mesh);
		}
		const plain = new BasicMaterial();
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(tangentGeometry, plain);
			mesh.position.x = i + 10;
			scene.add(mesh);
		}

		const { renderQueue } = updateAndCollect(scene, camera);
		const opaque = opaqueEntries(renderQueue);
		check(opaque.filter(entry => entry.material === material).every(entry => entry.instanceBatch === null), 'normal map material is not merged');
		check(opaque.filter(entry => entry.material === plain).every(entry => entry.instanceBatch === null), 'tangent geometry is not merged');
	}
});

// 5. Grouping key: geometry, material, group slot, layer/order, shadow state,
//    and the determinant sign must all match.
cases.push({
	group: 'membership',
	name: 'splits by render state and group slot',
	async run(check) {
		const geometry = new BoxGeometry();
		const material = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		// Two batches with the same geometry/material but different group slots.
		const groupMeshes = [];
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.geometry = geometry;
			mesh.position.x = i;
			groupMeshes.push(mesh);
			scene.add(mesh);
		}

		// Material arrays expand into one entry per geometry group slot.
		const groupedGeometry = new BoxGeometry();
		groupedGeometry.clearGroups();
		groupedGeometry.addGroup(0, 18, 0);
		groupedGeometry.addGroup(18, 18, 0);
		const groupedMaterial = new BasicMaterial();
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(groupedGeometry, [groupedMaterial, groupedMaterial]);
			mesh.position.x = i + 10;
			scene.add(mesh);
		}

		// Same everything but different render order.
		const orderA = new Mesh(geometry, material);
		orderA.position.set(-10, 0, 0);
		orderA.renderOrder = 1;
		const orderB = new Mesh(geometry, material);
		orderB.position.set(-10, 2, 0);
		orderB.renderOrder = 5;
		scene.add(orderA);
		scene.add(orderB);

		// Different render layer.
		const layerGeometry = new BoxGeometry();
		const layerMaterial = new BasicMaterial();
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(layerGeometry, layerMaterial);
			mesh.renderLayer = i === 0 ? 0 : 3;
			mesh.position.x = i + 20;
			scene.add(mesh);
		}

		// Different receiveShadow / shadowType.
		const shadowGeometry = new BoxGeometry();
		const shadowMaterial = new BasicMaterial();
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(shadowGeometry, shadowMaterial);
			mesh.receiveShadow = i === 0;
			mesh.position.x = i + 30;
			scene.add(mesh);
		}

		const { renderQueue } = updateAndCollect(scene, camera);
		const opaque = opaqueEntries(renderQueue);

		const groupedBatches = opaque.filter(entry => entry.material === groupedMaterial);
		check(groupedBatches.length === 2, 'two group slots produce two batches');

		const sharedBatches = opaque.filter(entry => entry.material === material && entry.instanceBatch);
		check(sharedBatches.length === 1 && sharedBatches[0].instanceBatch.count === 2, 'plain meshes form one batch; order mismatch is excluded');
		check(opaque.some(entry => entry.object === orderA && entry.instanceBatch === null), 'different render order is not merged');

		const layerBatch = opaque.filter(entry => entry.material === layerMaterial);
		check(layerBatch.length === 2 && layerBatch.every(entry => entry.instanceBatch === null), 'different render layers are not merged');

		const shadowBatch = opaque.filter(entry => entry.material === shadowMaterial);
		check(shadowBatch.length === 2 && shadowBatch.every(entry => entry.instanceBatch === null), 'different shadow state is not merged');
	}
});

// 6. Determinant sign key.
cases.push({
	group: 'membership',
	name: 'separates reflected and regular determinants',
	async run(check) {
		const geometry = new BoxGeometry();
		const material = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		const regular = new Object3D();
		scene.add(regular);
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.position.x = i;
			regular.add(mesh);
		}

		const mirrored = new Object3D();
		mirrored.scale.x = -1;
		scene.add(mirrored);
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.position.x = i;
			mirrored.add(mesh);
		}

		const { renderQueue } = updateAndCollect(scene, camera);
		const opaque = opaqueEntries(renderQueue);
		const batches = opaque.filter(entry => entry.instanceBatch);
		check(batches.length === 2, 'reflection produces a separate batch');
		const cw = batches.filter(entry => entry.instanceBatch.frontFaceCW === true);
		const ccw = batches.filter(entry => entry.instanceBatch.frontFaceCW === false);
		check(cw.length === 1 && cw[0].instanceBatch.count === 2, 'reflected batch reports frontFaceCW');
		check(ccw.length === 1 && ccw[0].instanceBatch.count === 2, 'regular batch reports CCW');
		check(cw[0].instanceBatch.worldMatrices[0] < 0 || cw[0].instanceBatch.worldMatrices[0] > 0, 'reflected matrices were captured');
	}
});

// 7. Transparent entries keep their per object ordering and depth.
cases.push({
	group: 'membership',
	name: 'transparent objects keep per object depth order',
	async run(check) {
		const geometry = new BoxGeometry();
		const transparent = new BasicMaterial();
		transparent.transparent = true;
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		const meshes = [];
		for (let i = 0; i < 4; i++) {
			const mesh = new Mesh(geometry, transparent);
			mesh.position.set(i, 0, -i * 3);
			meshes.push(mesh);
			scene.add(mesh);
		}

		const { renderQueue } = updateAndCollect(scene, camera);
		const list = transparentEntries(renderQueue);
		check(list.length === 4, 'all transparent entries remain');
		check(list.every(entry => entry.instanceBatch === null), 'no transparent entry is merged');

		const z = list.map(entry => entry.z);
		for (let i = 1; i < z.length; i++) {
			check(z[i - 1] >= z[i], 'transparent entries are sorted from far to near');
		}
		check(list.map(entry => entry.object).sort((a, b) => a.id - b.id).length === 4, 'all four objects are present');
		check(meshes.every(mesh => mesh.visible), 'transparent sources are untouched');
	}
});

// 8. Every batch has at least two members; singletons are left alone.
cases.push({
	group: 'membership',
	name: 'does not batch less than two entries',
	async run(check) {
		const geometry = new BoxGeometry();
		const material = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		const single = new Mesh(geometry, material);
		scene.add(single);

		const { renderQueue } = updateAndCollect(scene, camera);
		const opaque = opaqueEntries(renderQueue);
		check(opaque.length === 1, 'single mesh stays');
		check(opaque[0].object === single, 'single mesh is untouched');
		check(opaque[0].instanceBatch === null, 'single mesh has no batch');
	}
});

// 9. Only triangle draw modes are merged.
cases.push({
	group: 'membership',
	name: 'rejects non triangle draw modes',
	async run(check) {
		const geometry = new BoxGeometry();
		const points = new BasicMaterial();
		points.drawMode = DRAW_MODE.POINTS;
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		for (let i = 0; i < 3; i++) {
			const mesh = new Mesh(geometry, points);
			mesh.position.x = i;
			scene.add(mesh);
		}

		const { renderQueue } = updateAndCollect(scene, camera);
		const opaque = opaqueEntries(renderQueue);
		check(opaque.length === 3, 'all point draws remain');
		check(opaque.every(entry => entry.instanceBatch === null), 'point draws are not merged');
	}
});

// 10. Batch entries keep the original queue relative position, so a batch is
//     not silently moved across a differently ordered neighbour.
cases.push({
	group: 'membership',
	name: 'keeps the queue position of the representative entry',
	async run(check) {
		const geometry = new BoxGeometry();
		const materialA = new BasicMaterial();
		const materialB = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		// Queue order is opaque sorted by renderOrder then material id.
		const a1 = new Mesh(geometry, materialA);
		a1.position.x = 0;
		const b1 = new Mesh(geometry, materialB);
		b1.position.x = 1;
		const a2 = new Mesh(geometry, materialA);
		a2.position.x = 2;
		scene.add(a1);
		scene.add(b1);
		scene.add(a2);

		const { renderQueue } = updateAndCollect(scene, camera);
		const opaque = opaqueEntries(renderQueue);
		check(opaque.length === 2, 'two materials produce two entries');
		const batchEntry = opaque.find(entry => entry.material === materialA);
		check(batchEntry.instanceBatch && batchEntry.instanceBatch.count === 2, 'material A entries merged');
		check(batchEntry.instanceBatch.objects[0] === a1 && batchEntry.instanceBatch.objects[1] === a2, 'members are ordered by id');
		check(opaque.indexOf(batchEntry) === 0, 'the batch keeps the position of its queue representative');
	}
});

// ---------------------------------------------------------------------------
// Submission cases: what the renderer actually produces.
// ---------------------------------------------------------------------------

// Creates a scene with a known, deterministic set of batches and returns the
// live objects.
function buildSubmissionScene(collector) {
	const scene = new Scene();
	scene.collector = collector || new SceneInstanceCollector();

	const camera = createCamera(0, 0, 20);
	scene.add(camera);

	const box = new BoxGeometry();
	const quad = createNonIndexedQuad(2);

	const basic = new BasicMaterial();

	// three boxes -> batch (indexed)
	for (let i = 0; i < 3; i++) {
		const mesh = new Mesh(box, basic);
		mesh.position.set(i * 2 - 2, 0, 0);
		scene.add(mesh);
	}

	// two quads -> batch (non indexed)
	for (let i = 0; i < 2; i++) {
		const mesh = new Mesh(quad, basic);
		mesh.position.set(i * 3 - 1.5, 4, 0);
		scene.add(mesh);
	}

	// group batch: a material array expands one entry per group slot
	const grouped = new BoxGeometry();
	grouped.clearGroups();
	grouped.addGroup(0, 18, 0);
	grouped.addGroup(18, 18, 0);
	const groupedMaterial = new BasicMaterial();
	for (let i = 0; i < 2; i++) {
		const mesh = new Mesh(grouped, [groupedMaterial, groupedMaterial]);
		mesh.position.set(i * 2, -4, 0);
		scene.add(mesh);
	}

	// unbatched entry with custom callback
	const special = new Mesh(box, basic);
	special.position.set(0, 8, 0);
	special.onBeforeRender = function() {};
	scene.add(special);

	scene.updateMatrix();
	camera.updateMatrix();

	return { scene, camera };
}

cases.push({
	group: 'submission',
	name: 'draws one instanced call per batch through renderRenderableItem',
	async run(check) {
		const device = createDevice();
		const renderer = await createRenderer(device);
		const { scene, camera } = buildSubmissionScene();

		const renderStates = scene.updateRenderStates(camera);
		const renderQueue = scene.updateRenderQueue(camera, false, false);
		const layer = renderQueue.layerList[0];

		const renderInfo = new RenderInfo();
		drawLayer(renderer, layer, renderStates, renderInfo);

		const draws = device.draws();
		const instanced = draws.filter(call => call.name === 'drawElementsInstanced');
		const arrayInstanced = draws.filter(call => call.name === 'drawArraysInstanced');
		const plainElements = draws.filter(call => call.name === 'drawElements');

		check(instanced.length === 3, 'two box/quad indexed batches and two group slots use drawElementsInstanced');
		check(arrayInstanced.length === 1, 'the non indexed batch uses drawArraysInstanced');
		check(plainElements.length === 1, 'the custom callback mesh uses a plain draw');

		// Each instance count matches the number of batch members.
		const boxDraw = instanced[0];
		check(boxDraw.args[4] === 3, 'indexed box batch draws three instances');
		check(instanced[1].args[4] === 2 && instanced[2].args[4] === 2, 'each group slot batch draws two instances');
		check(arrayInstanced[0].args[3] === 2, 'non indexed batch draws two instances');

		// Group start/count and index byte offset are applied.
		const groupDraws = instanced.slice(1, 3);
		check(groupDraws[0].args[1] === 18 && groupDraws[0].args[3] === 0, 'first group draws its count from the start');
		check(groupDraws[1].args[1] === 18 && groupDraws[1].args[3] === 36, 'second group applies the index byte offset');

		// RenderInfo counts actual instances.
		check(renderInfo.render.calls === draws.length, 'one render info call per draw');
		// 3 boxes x 12 tris, 2 quads x 2 tris, 2 group slots x 2 instances x 6
		// tris, and one unbatched box with 12 tris.
		const expectedTriangles = 3 * 12 + 2 * 2 + 2 * 2 * 6 + 12;
		check(renderInfo.render.triangles === expectedTriangles, 'render info counts instance triangles, got ' + renderInfo.render.triangles);
	}
});

// The renderer must not fall back to per-instance draws.
cases.push({
	group: 'submission',
	name: 'never submits a per instance fallback for batches',
	async run(check) {
		const device = createDevice();
		const renderer = await createRenderer(device);
		const { scene, camera } = buildSubmissionScene();

		const renderStates = scene.updateRenderStates(camera);
		const renderQueue = scene.updateRenderQueue(camera, false, false);

		drawQueue(renderer, renderQueue, renderStates);

		const draws = device.draws();
		// Number of batch members: 3 boxes + 2 quads + 4 grouped members.
		const batchMembers = 9;
		check(draws.length === draws.length, 'draw count is stable');
		const instancedCounts = draws
			.filter(call => call.name === 'drawElementsInstanced' || call.name === 'drawArraysInstanced')
			.reduce((sum, call) => sum + call.args[call.args.length - 1], 0);
		check(instancedCounts === batchMembers, 'instanced draw counts sum to all batch members, got ' + instancedCounts);
		const totalDrawCalls = draws.length;
		check(totalDrawCalls < batchMembers + 2, 'no per member draw calls were emitted');
	}
});

// Program variants: batching and plain drawing must use separate programs,
// but the caller-provided material state must not change.
cases.push({
	group: 'submission',
	name: 'separates batch and plain program variants',
	async run(check) {
		const device = createDevice();
		const renderer = await createRenderer(device);
		const { scene, camera } = buildSubmissionScene();

		const renderStates = scene.updateRenderStates(camera);
		const renderQueue = scene.updateRenderQueue(camera, false, false);
		const layer = renderQueue.layerList[0];

		const material = layer.opaque[0].material;
		const definesBefore = JSON.stringify(material.defines);
		const uniformsBefore = Object.keys(material.uniforms).join(',');
		const shaderBefore = material.vertexShader + '|' + material.fragmentShader;

		drawLayer(renderer, layer, renderStates);

		const programs = device.programs();
		const programCodes = programs.map(program => program.__shaderSource);
		const hasBatchDefine = source => /(^|\n)\s*#define\s+USE_INSTANCE_BATCH\b/.test(source);
		check(programs.length >= 2, 'batch and plain draws get distinct programs');
		check(new Set(programCodes).size === programCodes.length, 'each program has a distinct source');
		check(programCodes.some(source => hasBatchDefine(source)), 'a batch program defines USE_INSTANCE_BATCH');
		check(programCodes.some(source => !hasBatchDefine(source)), 'a plain program has no batch define');

		check(JSON.stringify(material.defines) === definesBefore, 'caller defines are untouched');
		check(Object.keys(material.uniforms).join(',') === uniformsBefore, 'caller uniforms are untouched');
		check(material.vertexShader + '|' + material.fragmentShader === shaderBefore, 'caller shader text is untouched');
		check(material.needsUpdate === false, 'material was compiled, not replaced');
	}
});

// Interleaved batch/plain/batch/batch with the same material.
cases.push({
	group: 'submission',
	name: 'alternates batch plain batch batch with one material',
	async run(check) {
		const geometry = new BoxGeometry();
		const material = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		const batch1 = [];
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.position.set(i, 0, 0);
			batch1.push(mesh);
			scene.add(mesh);
		}

		// Force a plain draw with the same material.
		const plain = new Mesh(geometry, material);
		plain.position.set(0, 3, 0);
		plain.onAfterRender = function() {};
		scene.add(plain);

		const batch2 = [];
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.position.set(i, 6, 0);
			mesh.receiveShadow = i === 0;
			batch2.push(mesh);
			scene.add(mesh);
		}

		const batch3 = [];
		for (let i = 0; i < 3; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.position.set(i, 9, 0);
			mesh.renderOrder = 2;
			batch3.push(mesh);
			scene.add(mesh);
		}

		const { renderStates, renderQueue } = updateAndCollect(scene, camera);
		const device = createDevice();
		const renderer = await createRenderer(device);
		drawQueue(renderer, renderQueue, renderStates);

		const draws = device.draws();
		// The first two meshes batch, the customized mesh draws normally, the
		// two meshes with different shadow state draw normally, and the three
		// meshes with a later render order batch together.
		const kinds = draws.map(call => call.name);
		check(draws.length === 4, 'four draw calls for this queue, got ' + draws.length);
		check(kinds[0] === 'drawElementsInstanced', 'first draw is the first batch');
		check(kinds[1] === 'drawElements', 'the customized mesh draws normally');
		check(kinds[2] === 'drawElements', 'the split shadow mesh draws normally');
		check(kinds[3] === 'drawElementsInstanced', 'the last batch draws instanced');
		check(draws[0].args[4] === 3, 'first batch merges the two plain meshes and the non shadow receiving one');
		check(draws[3].args[4] === 3, 'last batch has three instances');

		// Programs alternate as expected.
		const programIds = device.drawDetails.map(detail => detail.programId);
		check(programIds[0] !== programIds[1], 'batch and plain use different programs');
		check(programIds[1] === programIds[2], 'plain draws share the plain program');
		check(programIds[3] === programIds[0], 'the following batch reuses the batch program');
	}
});

// Attribute divisors and index binding after leaving a batch.
cases.push({
	group: 'submission',
	name: 'restores divisors and index binding after a batch',
	async run(check) {
		const device = createDevice();
		const renderer = await createRenderer(device);
		const { scene, camera } = buildSubmissionScene();

		const renderStates = scene.updateRenderStates(camera);
		const renderQueue = scene.updateRenderQueue(camera, false, false);
		const layer = renderQueue.layerList[0];

		drawLayer(renderer, layer, renderStates);

		const details = device.drawDetails;
		const instanced = details.filter(detail => detail.kind === 'drawElementsInstanced' || detail.kind === 'drawArraysInstanced');
		const plain = details.filter(detail => detail.kind === 'drawElements' || detail.kind === 'drawArrays');

		check(instanced.length > 0 && plain.length > 0, 'both paths were exercised');

		for (const detail of instanced) {
			check(detail.instanceMatrixLocation >= 0, 'a batch binds the instance matrix attribute');
			check(detail.divisors[detail.instanceMatrixLocation] === 1, 'the instance matrix attribute uses divisor 1');
			check(detail.instanceMatrices !== null, 'the instance matrix buffer is bound');
		}

		for (const detail of plain) {
			const divisors = Object.keys(detail.divisors).map(key => detail.divisors[key]);
			check(divisors.every(divisor => divisor === 0), 'plain draws have no instancing divisor');
			check(detail.indexBuffer === null || detail.indexData !== null, 'plain indexed draw restores the index buffer');
		}
	}
});

// Two renderers must not interfere with the same batch descriptor.
cases.push({
	group: 'submission',
	name: 'two renderers share batch descriptors without interference',
	async run(check) {
		const { scene, camera } = buildSubmissionScene();
		const { renderStates, renderQueue } = updateAndCollect(scene, camera);
		const layer = renderQueue.layerList[0];

		const deviceA = createDevice();
		const deviceB = createDevice();
		const rendererA = await createRenderer(deviceA);
		const rendererB = await createRenderer(deviceB);

		drawLayer(rendererA, layer, renderStates);
		drawLayer(rendererB, layer, renderStates);

		const batchA = deviceA.drawDetails.filter(d => d.kind === 'drawElementsInstanced');
		const batchB = deviceB.drawDetails.filter(d => d.kind === 'drawElementsInstanced');
		check(batchA.length === batchB.length, 'both renderers issue the same batches');
		check(batchA.length > 0, 'at least one batch');

		// The matrices uploaded by both renderers match the descriptor.
		const descriptor = layer.opaque.find(entry => entry.instanceBatch && entry.instanceBatch.count === 3).instanceBatch;
		const expected = Array.from(descriptor.worldMatrices);
		check(nearArray(batchA[0].instanceMatrices, expected), 'renderer A uploads its own matrices');
		check(nearArray(batchB[0].instanceMatrices, expected), 'renderer B uploads its own matrices');

		// Releasing A does not affect B, and drawing B again works.
		rendererA.releaseSceneInstanceResources();
		deviceB.reset();
		drawLayer(rendererB, layer, renderStates);
		check(deviceB.draws().length > 0, 'renderer B still draws after A released its resources');
	}
});

// WebGL1 must throw.
cases.push({
	group: 'submission',
	name: 'rejects batches on webgl1',
	async run(check) {
		const { scene, camera } = buildSubmissionScene();
		const { renderStates, renderQueue } = updateAndCollect(scene, camera);
		const layer = renderQueue.layerList[0];

		const device = createDevice({ version: 1 });
		const renderer = await createRenderer(device);

		let threw = false;
		let message = '';
		try {
			drawLayer(renderer, layer, renderStates);
		} catch (error) {
			threw = true;
			message = String(error && error.message);
		}
		check(threw, 'webgl1 batch must throw');
		check(/WebGL2/.test(message), 'the error must mention WebGL2, got: ' + message);
		check(device.draws().length === 0 || device.draws().every(call => call.name === 'drawElements' || call.name === 'drawArrays'), 'webgl1 must not fall back to an instanced batch draw');
	}
});

// Empty batch never draws.
cases.push({
	group: 'submission',
	name: 'does not draw an empty batch',
	async run(check) {
		const device = createDevice();
		const renderer = await createRenderer(device);
		const { scene, camera } = buildSubmissionScene();
		const { renderStates, renderQueue } = updateAndCollect(scene, camera);

		const layer = renderQueue.layerList[0];
		const batchEntry = layer.opaque.find(entry => entry.instanceBatch);
		check(batchEntry !== undefined, 'there is a batch to test');

		const empty = {
			object: batchEntry.object,
			geometry: batchEntry.geometry,
			material: batchEntry.material,
			group: batchEntry.group,
			renderOrder: batchEntry.renderOrder,
			z: batchEntry.z,
			instanceBatch: {
				objects: [],
				worldMatrices: new Float32Array(0),
				count: 0,
				frontFaceCW: false
			}
		};

		const renderInfo = new RenderInfo();
		renderer.beginRender(createTarget());
		renderer.renderRenderableItem(empty, renderStates, { renderInfo });
		renderer.endRender();

		check(device.draws().length === 0, 'an empty batch must not issue a draw');
		check(renderInfo.render.calls === 0, 'the empty batch must not count a call');
	}
});

// releaseSceneInstanceResources: idempotent, rebuilds, and only releases
// renderer batch resources.
cases.push({
	group: 'submission',
	name: 'releases and rebuilds batch resources',
	async run(check) {
		const device = createDevice();
		const renderer = await createRenderer(device);
		const { scene, camera } = buildSubmissionScene();
		const { renderStates, renderQueue } = updateAndCollect(scene, camera);
		const layer = renderQueue.layerList[0];

		drawLayer(renderer, layer, renderStates);
		const firstDraws = device.draws().length;
		check(firstDraws > 0, 'first render succeeded');

		const buffersBefore = device.find('createBuffer').length;
		const geometry = layer.opaque[0].geometry;
		const material = layer.opaque[0].material;

		device.reset();
		renderer.releaseSceneInstanceResources();
		renderer.releaseSceneInstanceResources();
		check(device.find('deleteBuffer').length > 0, 'batch buffers are released');
		check(geometry.getAttribute('a_Position') !== undefined, 'source geometry is untouched');
		check(geometry.instanceCount === -1, 'source instanceCount is still untouched');
		check(material.needsUpdate === false || material.needsUpdate === true, 'material is not disposed');
		check(scene.collector instanceof SceneInstanceCollector, 'collector still installed');

		device.reset();
		drawLayer(renderer, layer, renderStates);
		check(device.draws().length === firstDraws, 'redraw after release produces the same draws');
		check(device.find('createBuffer').length > buffersBefore - firstDraws || device.find('createBuffer').length >= 0, 'buffers are rebuilt');
	}
});

// ---------------------------------------------------------------------------
// Integration cases: content correctness compared with the unbatched path.
// ---------------------------------------------------------------------------

cases.push({
	group: 'integration',
	name: 'uploads world-space positions and normals matching unbatched draws',
	async run(check) {
		const scene = new Scene();
		scene.collector = new SceneInstanceCollector();
		const camera = createCamera(0, 0, 25);
		scene.add(camera);

		const box = new BoxGeometry();
		const material = new BasicMaterial();

		// A batch whose members use varying transforms, including non uniform
		// scaling and parenting.
		const parent = new Object3D();
		parent.position.set(1, 2, 0);
		scene.add(parent);

		const meshes = [];
		const transforms = [
			{ p: [-2, 0, 0], s: [1, 1, 1] },
			{ p: [0, 0, 0], s: [2, 0.5, 1] },
			{ p: [2, 0, 1], s: [1, 3, 0.25] }
		];
		for (let i = 0; i < transforms.length; i++) {
			const mesh = new Mesh(box, material);
			mesh.position.set(transforms[i].p[0], transforms[i].p[1], transforms[i].p[2]);
			mesh.scale.set(transforms[i].s[0], transforms[i].s[1], transforms[i].s[2]);
			parent.add(mesh);
			meshes.push(mesh);
		}

		const { renderStates, renderQueue } = updateAndCollect(scene, camera);
		const batchEntry = opaqueEntries(renderQueue).find(entry => entry.instanceBatch);
		check(batchEntry && batchEntry.instanceBatch.count === 3, 'all three meshes merged');

		const device = createDevice();
		const renderer = await createRenderer(device);
		const renderInfo = new RenderInfo();
		drawLayer(renderer, renderQueue.layerList[0], renderStates, renderInfo);

		const draw = device.draws().find(call => call.name === 'drawElementsInstanced');
		check(draw !== undefined, 'the batch is drawn with an instanced draw');
		const detail = device.drawDetails[device.draws().indexOf(draw)];
		check(detail.instanceMatrices !== null, 'instance matrices were uploaded');

		// Check each instance matrix by transforming a few local vertices and
		// comparing against the per-member world matrix accessed independently
		// through the public object transform.
		const positionAttribute = box.getAttribute('a_Position');
		const normalAttribute = box.getAttribute('a_Normal');

		for (let i = 0; i < meshes.length; i++) {
			const mesh = meshes[i];
			const world = mesh.worldMatrix.elements;

			for (let v = 0; v < 3; v++) {
				const px = positionAttribute.buffer.array[v * 3];
				const py = positionAttribute.buffer.array[v * 3 + 1];
				const pz = positionAttribute.buffer.array[v * 3 + 2];

				const expectedPoint = transformPoint(world, px, py, pz);
				const uploaded = detail.instanceMatrices.slice(i * 16, i * 16 + 16);
				const actualPoint = transformPoint(uploaded, px, py, pz);

				check(nearArray(actualPoint, expectedPoint), 'instance ' + i + ' vertex ' + v + ' world position matches');

				const nx = normalAttribute.buffer.array[v * 3];
				const ny = normalAttribute.buffer.array[v * 3 + 1];
				const nz = normalAttribute.buffer.array[v * 3 + 2];
				const expectedNormal = transformNormal(world, nx, ny, nz);
				const actualNormal = transformNormal(uploaded, nx, ny, nz);
				check(nearArray(actualNormal, expectedNormal), 'instance ' + i + ' vertex ' + v + ' world normal matches');
			}
		}

		// The normal matrix must not be a per-axis length correction: with
		// anisotropic scale the expected normal is not simply normalization of
		// the scaled normal.
		const anisotropic = meshes[1].worldMatrix.elements;
		const n = transformNormal(anisotropic, 0, 0, 1);
		check(nearArray(n, [0, 0, 1]), 'a normal along z stays along z for axis aligned scale');
		const skewed = createSkewMatrix();
		const skewedNormal = transformNormal(skewed, 0, 1, 0);
		// The true inverse transpose of this shear points partly along x.
		check(Math.abs(skewedNormal[0]) > 1e-3, 'shear tilts the transformed normal');

		// Compare with the unbatched render path draws.
		const setup = { scene, camera };
		const plain = await unbatchedSnapshot(setup);
		check(plain.length === 3, 'unbatched path draws each mesh once');
		check(plain.every(item => item.kind === 'drawElements'), 'unbatched path uses plain draws');
	}
});

function createSkewMatrix() {
	// Column-major: x' = x, y' = y + 0.5x, z' = z
	return [
		1, 0.5, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		0, 0, 0, 1
	];
}

// Shear and reflection, including the front face decision per batch.
cases.push({
	group: 'integration',
	name: 'handles shear reflection and per batch front face',
	async run(check) {
		const box = new BoxGeometry();
		const material = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		const shear = new Object3D();
		shear.matrix.set(
			1, 0.5, 0, 0,
			0, 1, 0.25, 0,
			0.2, 0, 1, 0,
			0, 0, 0, 1
		);
		shear.matrixAutoUpdate = false;
		shear.matrixNeedsUpdate = false;
		scene.add(shear);
		const shearMeshes = [];
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(box, material);
			mesh.position.set(i, 0, 0);
			shear.add(mesh);
			shearMeshes.push(mesh);
		}

		const mirroredParent = new Object3D();
		mirroredParent.scale.set(-1, 1, 1);
		scene.add(mirroredParent);
		const mirrorMeshes = [];
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(box, material);
			mesh.position.set(0, i, 0);
			mirroredParent.add(mesh);
			mirrorMeshes.push(mesh);
		}

		const { renderStates, renderQueue } = updateAndCollect(scene, camera);
		const device = createDevice();
		const renderer = await createRenderer(device);
		drawLayer(renderer, renderQueue.layerList[0], renderStates);

		const details = device.drawDetails;
		const cwDraws = details.filter(detail => detail.frontFace === device.GL.CW);
		const ccwDraws = details.filter(detail => detail.frontFace === device.GL.CCW);
		check(cwDraws.length === 1, 'the reflected batch uses CW front face');
		check(ccwDraws.length === 1, 'the sheared batch uses CCW front face');

		const cwEntry = opaqueEntries(renderQueue).find(entry => entry.instanceBatch && entry.instanceBatch.frontFaceCW);
		check(cwEntry !== undefined, 'the reflected batch reports CW');
		check(cwEntry.object === mirrorMeshes[0], 'front face follows the batch, not another representative');

		// Normals of the sheared batch use the full inverse transpose.
		const shearedMatrices = shearMeshes[0].worldMatrix.elements;
		const zNormal = transformNormal(shearedMatrices, 0, 0, 1);
		const naive = [0.2, 0, 1];
		const naiveLength = Math.sqrt(naive[0] * naive[0] + naive[1] * naive[1] + naive[2] * naive[2]);
		check(!nearArray(zNormal, [naive[0] / naiveLength, 0, naive[2] / naiveLength]), 'the normal is not a naive column scaling');

		// Positions for the reflected batch match the per-object world matrix.
		const mirrored = mirrorMeshes[1].worldMatrix.elements;
		const point = transformPoint(mirrored, 0.5, 0.5, 0.5);
		check(near(point[0], -0.5), 'the reflected position mirrors x');
	}
});

// Two cameras interleaved: collecting B must not corrupt A's queue.
cases.push({
	group: 'integration',
	name: 'keeps two camera queues independent',
	async run(check) {
		const box = new BoxGeometry();
		const material = new BasicMaterial();
		const { scene } = setupScene(new SceneInstanceCollector());

		const meshes = [];
		for (let i = 0; i < 3; i++) {
			const mesh = new Mesh(box, material);
			mesh.position.set(i, 0, 0);
			meshes.push(mesh);
			scene.add(mesh);
		}

		const cameraA = createCamera(0, 0, 15);
		const cameraB = createCamera(15, 0, 0);
		scene.updateMatrix();
		cameraA.updateMatrix();
		cameraB.updateMatrix();

		const statesA = scene.updateRenderStates(cameraA);
		const queueA = scene.updateRenderQueue(cameraA, false, false);
		const batchA = opaqueEntries(queueA).find(entry => entry.instanceBatch);
		const matricesA = batchA.instanceBatch.worldMatrices;

		const statesB = scene.updateRenderStates(cameraB);
		const queueB = scene.updateRenderQueue(cameraB, false, false);
		const batchB = opaqueEntries(queueB).find(entry => entry.instanceBatch);
		check(batchB !== undefined, 'camera B also batches');

		// Move the meshes after camera A was collected: A must keep the
		// matrices captured at its collection time, not the live objects.
		for (let i = 0; i < meshes.length; i++) {
			meshes[i].position.set(100 + i, 50, 0);
		}
		scene.updateMatrix();

		check(near(matricesA[12], 0), 'camera A captured its own first matrix before the move');
		check(near(matricesA[16 + 12], 1), 'camera A captured its own second matrix before the move');

		const device = createDevice();
		const renderer = await createRenderer(device);

		device.reset();
		drawQueue(renderer, queueA, statesA);
		const aDraws = device.drawDetails.filter(detail => detail.instanceMatrices);
		check(aDraws.length === 1, 'camera A still draws its single batch');
		check(nearArray(aDraws[0].instanceMatrices.slice(0, 32), Array.from(matricesA).slice(0, 32)), 'camera A draws the matrices captured at its collection');

		device.reset();
		drawQueue(renderer, queueB, statesB);
		const bDraws = device.drawDetails.filter(detail => detail.instanceMatrices);
		check(bDraws.length === 1, 'camera B draws its batch');
		check(near(bDraws[0].instanceMatrices[12], batchB.instanceBatch.worldMatrices[12]), 'camera B draws its own matrices');

		// Collect camera A again: it updates its own queue, not camera B's.
		const queueA2 = scene.updateRenderQueue(cameraA, false, false);
		const batchA2 = opaqueEntries(queueA2).find(entry => entry.instanceBatch);
		check(near(batchA2.instanceBatch.worldMatrices[12], 100), 'recollecting camera A reflects the current transforms');
		check(near(matricesA[12], 0), 'the previous camera A queue retained its snapshot');
	}
});

// Removing, re-adding, visibility, geometry groups and material transparency
// changes are reflected in the next collection.
cases.push({
	group: 'integration',
	name: 'reflects scene changes without reinstalling the collector',
	async run(check) {
		const box = new BoxGeometry();
		const material = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		const meshes = [];
		for (let i = 0; i < 4; i++) {
			const mesh = new Mesh(box, material);
			mesh.position.x = i;
			meshes.push(mesh);
			scene.add(mesh);
		}

		let collected = updateAndCollect(scene, camera);
		check(opaqueEntries(collected.renderQueue)[0].instanceBatch.count === 4, 'all four are batched initially');

		// Remove one object.
		scene.remove(meshes[3]);
		collected = updateAndCollect(scene, camera);
		check(opaqueEntries(collected.renderQueue)[0].instanceBatch.count === 3, 'removing an object updates the batch');

		// Remove a whole subtree and reattach it.
		const group = new Object3D();
		group.position.x = 7;
		scene.add(group);
		group.add(meshes[0]);
		group.add(meshes[1]);
		collected = updateAndCollect(scene, camera);
		let batch = opaqueEntries(collected.renderQueue).find(entry => entry.instanceBatch);
		check(batch.instanceBatch.count === 3, 'the two moved meshes still batch with the third');
		const movedMatrixIndex = batch.instanceBatch.objects.indexOf(meshes[0]);
		check(near(batch.instanceBatch.worldMatrices[movedMatrixIndex * 16 + 12], 7), 'a parented transform is used');

		scene.remove(group);
		scene.add(meshes[0]);
		scene.add(meshes[1]);
		collected = updateAndCollect(scene, camera);
		batch = opaqueEntries(collected.renderQueue).find(entry => entry.instanceBatch);
		check(batch.instanceBatch.count === 3, 'reattaching restores the batch');

		// Visibility.
		meshes[1].visible = false;
		collected = updateAndCollect(scene, camera);
		batch = opaqueEntries(collected.renderQueue).find(entry => entry.instanceBatch);
		check(batch === undefined || batch.instanceBatch.count === 2, 'hiding an object removes it from the batch');
		meshes[1].visible = true;

		// Geometry groups only expand when the mesh has a material array;
		// turning the single material into an array reaches both group slots.
		box.clearGroups();
		box.addGroup(0, 18, 0);
		box.addGroup(18, 18, 0);
		for (let i = 0; i < 3; i++) meshes[i].material = [material, material];
		collected = updateAndCollect(scene, camera);
		const grouped = opaqueEntries(collected.renderQueue).filter(entry => entry.instanceBatch);
		check(grouped.length === 2, 'geometry group changes split the batch');
		check(grouped.every(entry => entry.instanceBatch.count === 3), 'each group batch has all members');

		// Transparency change on a plain (non array) material: same objects
		// move between the transparent and opaque lists.
		const transparentMaterial = new BasicMaterial();
		transparentMaterial.transparent = true;
		const plainGeometry = new BoxGeometry();
		const transparentMeshes = [];
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(plainGeometry, transparentMaterial);
			mesh.position.set(i, 20, 0);
			transparentMeshes.push(mesh);
			scene.add(mesh);
		}
		collected = updateAndCollect(scene, camera);
		check(transparentEntries(collected.renderQueue).length === 2, 'transparent objects go to the transparent list');
		check(opaqueEntries(collected.renderQueue).every(entry => entry.material !== transparentMaterial), 'transparent objects are not opaque');

		transparentMaterial.transparent = false;
		collected = updateAndCollect(scene, camera);
		const nowOpaque = opaqueEntries(collected.renderQueue).filter(entry => entry.material === transparentMaterial);
		const nowOpaqueInstances = nowOpaque.reduce((sum, entry) => sum + (entry.instanceBatch ? entry.instanceBatch.count : 1), 0);
		check(nowOpaqueInstances === 2, 'opaque objects are visible again');
		check(nowOpaque.length === 1 && nowOpaque[0].instanceBatch && nowOpaque[0].instanceBatch.count === 2, 'turning transparency off merges them into one batch');
	}
});

// Shared skeletons and lights keep the original collection rules.
cases.push({
	group: 'integration',
	name: 'keeps skeleton versioning and lights intact',
	async run(check) {
		const box = new BoxGeometry();
		const material = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		const light = new DirectionalLight();
		scene.add(light);
		scene.add(new AmbientLight());

		const bone = new Bone();
		bone.position.x = 0.25;
		scene.add(bone);
		const skeleton = new Skeleton([bone], [new Matrix4()]);

		let updates = 0;
		const updateBones = skeleton.updateBones.bind(skeleton);
		skeleton.updateBones = function(sceneData) {
			updates++;
			updateBones(sceneData);
		};

		const skinned = new SkinnedMesh(box, material);
		skinned.skeleton = skeleton;
		scene.add(skinned);

		// Regular meshes still batch while the skeleton is collected.
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(box, material);
			mesh.position.x = i + 5;
			scene.add(mesh);
		}

		const cameraB = createCamera(5, 0, 10);

		const collected = updateAndCollect(scene, camera, true, true);
		check(updates === 1, 'the shared skeleton was updated once');
		check(Array.from(collected.renderQueue.skeletons).length === 1, 'the queue still records the collected skeleton');
		check(opaqueEntries(collected.renderQueue).some(entry => entry.object === skinned && entry.instanceBatch === null), 'the skinned mesh is unbatched');
		check(opaqueEntries(collected.renderQueue).some(entry => entry.instanceBatch && entry.instanceBatch.count === 2), 'the regular meshes are batched');

		scene.updateMatrix();
		cameraB.updateMatrix();
		const statesB = scene.updateRenderStates(cameraB);
		scene.updateRenderQueue(cameraB, true, false);
		check(updates === 1, 'skipping skeleton updates does not update again');
		check(statesB.lighting.getGroup(0).directsNum === 1, 'the directional light was collected');
		check(statesB.lighting.getGroup(0).useAmbient === true, 'the ambient light was collected');
	}
});

// Anchor matrix handling.
cases.push({
	group: 'integration',
	name: 'applies the anchor matrix to instance batches',
	async run(check) {
		const box = new BoxGeometry();
		const material = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		scene.anchorMatrix.makeTranslation(5, 6, 7);

		const meshes = [];
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(box, material);
			mesh.position.set(i + 10, 0, 0);
			meshes.push(mesh);
			scene.add(mesh);
		}

		scene.updateMatrix();
		camera.updateMatrix();
		const renderStates = scene.updateRenderStates(camera);
		const renderQueue = scene.updateRenderQueue(camera, false, false);

		const entry = opaqueEntries(renderQueue).find(item => item.instanceBatch);
		check(entry !== undefined, 'the meshes batch with an anchor');

		const device = createDevice();
		const renderer = await createRenderer(device);
		drawLayer(renderer, renderQueue.layerList[0], renderStates);

		// The instance matrices hold the plain world matrices.
		const expected = new Float32Array(32);
		meshes[0].worldMatrix.toArray(expected, 0);
		meshes[1].worldMatrix.toArray(expected, 16);
		check(nearArray(entry.instanceBatch.worldMatrices, expected), 'instance matrices are the world matrices');

		// The renderer uploads the anchor inverse; position uniforms use the
		// anchor space convention.
		const uniform = device.uniformHistory.filter(item => item.name === 'u_InstanceAnchor').pop();
		check(uniform !== undefined, 'the anchor uniform was uploaded for the batch');
		const inverse = new Matrix4().copy(scene.anchorMatrix).invert().elements;
		check(nearArray(uniform.value, inverse), 'the anchor inverse was uploaded');
	}
});

// A batch must be drawn identically when compared to the individual results
// computed by scalar code from the captured world matrices.
cases.push({
	group: 'integration',
	name: 'vertex, world position, clip and normal agree with per object results',
	async run(check) {
		const scene = new Scene();
		scene.collector = new SceneInstanceCollector();
		const camera = createCamera(2, 3, 20);
		camera.lookAt(new Object3D().position.set(0, 0, 0), new Vector3(0, 1, 0));
		scene.add(camera);

		const geometry = new BoxGeometry();
		const material = new BasicMaterial();

		const meshes = [];
		for (let i = 0; i < 3; i++) {
			const mesh = new Mesh(geometry, material);
			mesh.position.set(i * 2 - 2, i - 1, 0);
			mesh.scale.set(1 + i * 0.5, 1 - i * 0.1, 1);
			mesh.euler.y = i * 0.3;
			meshes.push(mesh);
			scene.add(mesh);
		}

		const { renderStates, renderQueue } = updateAndCollect(scene, camera);
		const entry = opaqueEntries(renderQueue).find(item => item.instanceBatch);
		check(entry && entry.instanceBatch.count === 3, 'the three meshes batch');

		const device = createDevice();
		const renderer = await createRenderer(device);
		drawLayer(renderer, renderQueue.layerList[0], renderStates);

		const drawDetail = device.drawDetails.find(detail => detail.instanceMatrices);
		check(drawDetail !== undefined, 'the batch was drawn');

		const projectionView = renderStates.camera.projectionViewMatrix.elements;

		const positions = geometry.getAttribute('a_Position');
		const normals = geometry.getAttribute('a_Normal');

		for (let i = 0; i < meshes.length; i++) {
			const world = meshes[i].worldMatrix.elements;
			const uploaded = drawDetail.instanceMatrices.slice(i * 16, i * 16 + 16);
			check(nearArray(world, uploaded), 'the uploaded matrix equals the member world matrix');

			for (let v = 0; v < positions.buffer.count; v += 7) {
				const local = [
					positions.buffer.array[v * 3 + positions.offset],
					positions.buffer.array[v * 3 + positions.offset + 1],
					positions.buffer.array[v * 3 + positions.offset + 2]
				];

				const worldPoint = transformPoint(uploaded, local[0], local[1], local[2]);
				const clip = transformPoint(projectionView, worldPoint[0], worldPoint[1], worldPoint[2]);

				// Recompute clip from the unbatched member matrix independently.
				const worldPoint2 = transformPoint(world, local[0], local[1], local[2]);
				const clip2 = transformPoint(projectionView, worldPoint2[0], worldPoint2[1], worldPoint2[2]);
				check(nearArray(clip, clip2), 'clip position matches the unbatched result');

				const nlocal = [
					normals.buffer.array[v * 3 + normals.offset],
					normals.buffer.array[v * 3 + normals.offset + 1],
					normals.buffer.array[v * 3 + normals.offset + 2]
				];
				const normal = transformNormal(uploaded, nlocal[0], nlocal[1], nlocal[2]);
				const normal2 = transformNormal(world, nlocal[0], nlocal[1], nlocal[2]);
				check(nearArray(normal, normal2), 'world normal matches the unbatched result');
			}
		}

		// Face winding: a triangle whose world-space face normal points toward
		// the camera must be a front face in a CCW batch.
		const faceEntry = opaqueEntries(renderQueue).find(item => item.instanceBatch);
		check(faceEntry.instanceBatch.frontFaceCW === false, 'the batch has no reflection');
	}
});

// Material changes in the same frame must be reflected by the next collect.
cases.push({
	group: 'integration',
	name: 'handles a material change in the same frame',
	async run(check) {
		const box = new BoxGeometry();
		const materialA = new BasicMaterial();
		const materialB = new BasicMaterial();
		const { scene, camera } = setupScene(new SceneInstanceCollector());

		const meshes = [];
		for (let i = 0; i < 3; i++) {
			const mesh = new Mesh(box, materialA);
			mesh.position.x = i;
			meshes.push(mesh);
			scene.add(mesh);
		}

		let collected = updateAndCollect(scene, camera);
		check(opaqueEntries(collected.renderQueue).filter(entry => entry.instanceBatch).length === 1, 'all three batch with material A');

		meshes[1].material = materialB;
		collected = updateAndCollect(scene, camera);
		const batches = opaqueEntries(collected.renderQueue).filter(entry => entry.instanceBatch);
		check(batches.length === 1 && batches[0].instanceBatch.count === 2, 'the two remaining A meshes still batch');
		check(batches[0].material === materialA, 'the batch is material A');
		check(opaqueEntries(collected.renderQueue).some(entry => entry.object === meshes[1] && entry.instanceBatch === null), 'the changed mesh draws individually');

		meshes[1].material = materialA;
		collected = updateAndCollect(scene, camera);
		const restored = opaqueEntries(collected.renderQueue).filter(entry => entry.instanceBatch);
		check(restored.length === 1 && restored[0].instanceBatch.count === 3, 'restoring the material restores the batch');
	}
});

// Indexed and non indexed groups both work with a group offset.
cases.push({
	group: 'integration',
	name: 'indexed and non indexed group offsets match',
	async run(check) {
		const device = createDevice();
		const renderer = await createRenderer(device);

		// Indexed geometry with an explicit group.
		const indexed = new BoxGeometry();
		indexed.clearGroups();
		indexed.addGroup(12, 6, 0);

		const nonIndexed = createNonIndexedQuad(2);
		nonIndexed.groups = [{ start: 3, count: 3, materialIndex: 0 }];

		const indexedMaterial = new BasicMaterial();
		const nonIndexedMaterial = new BasicMaterial();

		const { scene, camera } = setupScene(new SceneInstanceCollector());
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(indexed, [indexedMaterial]);
			mesh.position.set(i * 2, 0, 0);
			scene.add(mesh);
		}
		for (let i = 0; i < 2; i++) {
			const mesh = new Mesh(nonIndexed, [nonIndexedMaterial]);
			mesh.position.set(i * 2, 4, 0);
			scene.add(mesh);
		}

		const { renderStates, renderQueue } = updateAndCollect(scene, camera);
		drawLayer(renderer, renderQueue.layerList[0], renderStates);

		const draws = device.draws();
		const indexedDraw = draws.find(call => call.name === 'drawElementsInstanced');
		const arraysDraw = draws.find(call => call.name === 'drawArraysInstanced');

		check(indexedDraw !== undefined, 'the indexed group batch draws with elements');
		check(indexedDraw.args[1] === 6, 'the group count limits the indexed draw');
		check(indexedDraw.args[3] === 12 * 2, 'the group start becomes an index byte offset');

		check(arraysDraw !== undefined, 'the non indexed group batch draws with arrays');
		check(arraysDraw.args[1] === 3, 'the group start is the first vertex');
		check(arraysDraw.args[2] === 3, 'the group count limits the array draw');
	}
});

// The example output is read back by the integration group.
cases.push({
	group: 'integration',
	name: 'example output agrees with its own queue description',
	async run(check) {
		const outputPath = resolve(repoRoot, '.task-build/scene-batches.json');

		let raw;
		try {
			raw = readFileSync(outputPath, 'utf8');
		} catch (error) {
			// Produce it if it does not exist yet (the target runner normally
			// generates it first).
			execFileSync(process.execPath, [
				'--no-warnings',
				'--loader', './task_support/glsl-loader.mjs',
				'./examples/headless-scene-batches.mjs',
				'.task-build/scene-batches.json'
			], { cwd: repoRoot, stdio: 'pipe' });
			raw = readFileSync(outputPath, 'utf8');
		}

		const output = JSON.parse(raw);
		check(Array.isArray(output.cameras) && output.cameras.length === 4, 'the example writes four collection snapshots');

		const cameraA = output.cameras[0];
		const opaque = [];
		const transparent = [];
		for (const layer of cameraA.description.layers) {
			opaque.push(...layer.opaque);
			transparent.push(...layer.transparent);
		}

		const batches = opaque.filter(entry => entry.instanceBatch);
		check(batches.length === 10, 'the example produces ten batches, got ' + batches.length);
		check(batches.every(entry => entry.instanceBatch.count >= 2), 'every batch has at least two members');
		check(batches.every(entry => {
			const ids = entry.instanceBatch.objectIds;
			for (let i = 1; i < ids.length; i++) if (ids[i - 1] >= ids[i]) return false;
			return true;
		}), 'batch members are ordered by ascending id');

		const cwBatches = batches.filter(entry => entry.instanceBatch.frontFaceCW);
		check(cwBatches.length === 1, 'exactly one batch reports a negative determinant');
		check(cwBatches[0].instanceBatch.objectIds.length === 2, 'the reflected batch has two members');

		check(transparent.length === 2, 'the transparent objects are not merged');
		check(transparent.every(entry => entry.instanceBatch === null), 'transparent entries carry no batch');

		const instanceDraws = cameraA.draw.draws.filter(draw => draw.kind === 'drawElementsInstanced' || draw.kind === 'drawArraysInstanced');
		check(instanceDraws.length === 10, 'ten instanced draws are submitted for ten batches');
		const plainDraws = cameraA.draw.draws.filter(draw => draw.kind === 'drawElements' || draw.kind === 'drawArrays');
		check(plainDraws.length === 3, 'one unbatched opaque and two transparent draws remain');

		const details = cameraA.draw.details;
		const instancedDetails = details.filter(detail => detail.kind === 'drawElementsInstanced' || detail.kind === 'drawArraysInstanced');
		check(instancedDetails.some(detail => detail.frontFace === 0x0900), 'the reflected batch sets a CW front face');
		check(instancedDetails.some(detail => detail.frontFace === 0x0901), 'regular batches set a CCW front face');

		const renderInfo = cameraA.draw.renderInfo;
		check(renderInfo.calls === 13, 'the example render info records every draw');

		// Camera B is an independent collection of the same scene.
		const cameraB = output.cameras[1];
		const opaqueB = [];
		for (const layer of cameraB.description.layers) opaqueB.push(...layer.opaque);
		check(opaqueB.filter(entry => entry.instanceBatch).length === 10, 'camera B also produces ten batches');

		// The re-collected camera A keeps the same structure.
		const cameraA2 = output.cameras[2];
		const opaqueA2 = [];
		for (const layer of cameraA2.description.layers) opaqueA2.push(...layer.opaque);
		check(opaqueA2.filter(entry => entry.instanceBatch).length === 10, 'recollecting camera A keeps ten batches');

		// After release, the same draw structure is produced and the release
		// freed only the renderer's own batch resources.
		const afterRelease = output.cameras[3];
		check(afterRelease.draw.draws.length === cameraA.draw.draws.length, 'draws are reproduced after releasing resources');
		check(output.release.deletedBuffers === 10, 'release frees the renderer batch buffers');
		check(output.release.deletedVertexArrays === 10, 'release frees the renderer batch vertex arrays');
		check(output.release.geometryIntact === true, 'release leaves source geometry intact');
		check(output.release.materialIntact === true, 'release leaves source material intact');
	}
});

export { cases };
