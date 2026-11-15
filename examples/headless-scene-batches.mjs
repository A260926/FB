/**
 * Headless scene instance batch example.
 *
 * This example builds a small procedural scene, installs the production
 * SceneInstanceCollector, collects the queue for two cameras and draws both
 * queues through the real WebGLRenderer entry points. Every draw is recorded
 * by a recording WebGL2 device (no window or GPU is required) and the result
 * is written to the JSON file given as the first command line argument.
 *
 * Usage:
 *   node --loader ./task_support/glsl-loader.mjs examples/headless-scene-batches.mjs out.json
 */
import { writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

import {
	Scene, Camera, Mesh, Object3D, Geometry, Buffer, Attribute, BoxGeometry, PlaneGeometry,
	BasicMaterial, PBRMaterial, DirectionalLight, WebGLRenderer, ScreenRenderTarget,
	RenderInfo, SceneInstanceCollector, Vector3
} from '../src/main.js';
import { createDevice } from '../tests/support/recording-webgl2.mjs';

const outputPath = process.argv[2];

if (!outputPath) {
	console.error('usage: node examples/headless-scene-batches.mjs <output.json>');
	process.exit(2);
}

// --- Scene construction -----------------------------------------------------

function createNonIndexedQuad() {
	const geometry = new Geometry();
	geometry.addAttribute('a_Position', new Attribute(new Buffer(new Float32Array([
		-0.5, -0.5, 0, 0.5, -0.5, 0, 0.5, 0.5, 0,
		-0.5, -0.5, 0, 0.5, 0.5, 0, -0.5, 0.5, 0
	]), 3)));
	geometry.addAttribute('a_Normal', new Attribute(new Buffer(new Float32Array([
		0, 0, 1, 0, 0, 1, 0, 0, 1,
		0, 0, 1, 0, 0, 1, 0, 0, 1
	]), 3)));
	geometry.computeBoundingBox();
	geometry.computeBoundingSphere();
	return geometry;
}

function createScene() {
	const scene = new Scene();
	scene.collector = new SceneInstanceCollector();

	const light = new DirectionalLight();
	light.position.set(0.5, 1, 0.8);
	scene.add(light);

	const box = new BoxGeometry();
	const plane = new PlaneGeometry(1, 1);
	const quad = createNonIndexedQuad();
	const basic = new BasicMaterial();
	const pbr = new PBRMaterial();
	const multiA = new BasicMaterial();
	const multiB = new BasicMaterial();

	// A batch of four boxes sharing geometry and material.
	for (let i = 0; i < 4; i++) {
		const mesh = new Mesh(box, basic);
		mesh.position.set(-3 + i * 2, 2, 0);
		scene.add(mesh);
	}

	// Same material but customized callback: drawn one by one.
	const customizable = new Mesh(box, basic);
	customizable.position.set(0, 5, 0);
	customizable.onBeforeRender = function() {};
	scene.add(customizable);

	// A PBR batch.
	for (let i = 0; i < 3; i++) {
		const mesh = new Mesh(box, pbr);
		mesh.position.set(-2 + i * 2, -2, 0);
		scene.add(mesh);
	}

	// Multi material groups, meshes are merged per group slot.
	for (let i = 0; i < 2; i++) {
		const mesh = new Mesh(box, [multiA, multiA, multiB, multiB, multiA, multiB]);
		mesh.position.set(i * 2 - 1, 6.5, 0);
		scene.add(mesh);
	}

	// Non indexed geometry batch.
	for (let i = 0; i < 2; i++) {
		const mesh = new Mesh(quad, basic);
		mesh.position.set(i * 2 - 1, -5, 0);
		scene.add(mesh);
	}

	// Parent with shear and reflection: a separate batch with frontFaceCW.
	const shearedMirror = new Object3D();
	shearedMirror.matrix.set(
		1, 0.4, 0.2, 0,
		0, 1, 0.3, 0,
		0.15, 0, -1, 0,
		0, 0, 0, 1
	);
	shearedMirror.matrixAutoUpdate = false;
	shearedMirror.matrixNeedsUpdate = false;
	scene.add(shearedMirror);

	for (let i = 0; i < 2; i++) {
		const mesh = new Mesh(plane, basic);
		mesh.position.set(i * 2 - 1, -8, 0);
		shearedMirror.add(mesh);
	}

	// Transparent mesh, never merged.
	const transparent = new BasicMaterial();
	transparent.transparent = true;
	for (let i = 0; i < 2; i++) {
		const mesh = new Mesh(box, transparent);
		mesh.position.set(i * 2 - 1, 0, 4);
		scene.add(mesh);
	}

	return scene;
}

function createCameras() {
	const cameraA = new Camera();
	cameraA.position.set(0, 0, 22);
	cameraA.setPerspective(Math.PI / 3, 4 / 3, 0.1, 100);

	const cameraB = new Camera();
	cameraB.position.set(22, 0, 0);
	cameraB.lookAt(new Object3D().position.set(0, 0, 0), new Vector3(0, 1, 0));
	cameraB.setPerspective(Math.PI / 3, 4 / 3, 0.1, 100);

	return [cameraA, cameraB];
}

// --- Collection / drawing ---------------------------------------------------

function collect(scene, camera) {
	const renderStates = scene.updateRenderStates(camera);
	const renderQueue = scene.updateRenderQueue(camera);
	return { renderStates, renderQueue };
}

function drawQueue(renderer, device, camera, collected, renderInfo) {
	const target = new ScreenRenderTarget({ width: 320, height: 240 });

	device.reset();

	renderer.beginRender(target);

	const layerList = collected.renderQueue.layerList;
	for (let i = 0; i < layerList.length; i++) {
		const layer = layerList[i];
		renderer.renderRenderableList(layer.opaque, collected.renderStates, { renderInfo });
		renderer.renderRenderableList(layer.transparent, collected.renderStates, { renderInfo });
	}

	renderer.endRender();

	return {
		cameraId: camera.id,
		draws: device.draws().map(normalizeDraw),
		details: device.drawDetails.map(detail => Object.assign({}, detail, {
			instanceMatrices: detail.instanceMatrices ? detail.instanceMatrices.slice(0, 64) : null
		})),
		renderInfo: Object.assign({}, renderInfo.render)
	};
}

function normalizeDraw(call) {
	return {
		kind: call.name,
		args: call.args.map(value => (typeof value === 'number' ? value : String(value)))
	};
}

function describeQueue(collected) {
	const layerList = collected.renderQueue.layerList;
	const layers = [];

	for (let i = 0; i < layerList.length; i++) {
		const layer = layerList[i];
		layers.push({
			id: layer.id,
			opaque: layer.opaque.map(describeRenderable),
			transparent: layer.transparent.map(describeRenderable)
		});
	}

	return {
		sceneVersion: collected.renderStates.scene.version,
		cameraVersion: collected.renderStates.camera.version,
		layers: layers
	};
}

function describeRenderable(renderable) {
	const batch = renderable.instanceBatch || null;

	return {
		objectId: renderable.object.id,
		geometryId: renderable.geometry.id,
		materialId: renderable.material.id,
		group: renderable.group ? { start: renderable.group.start, count: renderable.group.count, materialIndex: renderable.group.materialIndex } : null,
		instanceBatch: batch ? {
			objectIds: batch.objects.map(object => object.id),
			count: batch.count,
			frontFaceCW: batch.frontFaceCW,
			worldMatrices: Array.from(batch.worldMatrices)
		} : null
	};
}

// --- Main -------------------------------------------------------------------

async function main() {
	const device = createDevice();
	const renderer = new WebGLRenderer();
	await renderer.init(device.gl);

	const scene = createScene();
	const cameras = createCameras();

	scene.updateMatrix();
	for (const camera of cameras) {
		camera.updateMatrix();
	}

	const collectedA = collect(scene, cameras[0]);
	const collectedB = collect(scene, cameras[1]);

	const renderInfoA = new RenderInfo();
	const drawA = drawQueue(renderer, device, cameras[0], collectedA, renderInfoA);

	const renderInfoB = new RenderInfo();
	const drawB = drawQueue(renderer, device, cameras[1], collectedB, renderInfoB);

	// Collect camera A again after B: its own queue must still be drawable.
	const collectedA2 = collect(scene, cameras[0]);
	const renderInfoA2 = new RenderInfo();
	const drawA2 = drawQueue(renderer, device, cameras[0], collectedA2, renderInfoA2);

	// Release and redraw. Release must not touch the source geometry or material,
	// and the next draw must recreate whatever it needs.
	const referenceRenderable = collectedA2.renderQueue.layerList[0].opaque[0];
	const geometryBeforeRelease = referenceRenderable.geometry;
	const materialBeforeRelease = referenceRenderable.material;
	const materialDefinesBeforeRelease = JSON.stringify(materialBeforeRelease.defines);
	const materialUniformsBeforeRelease = Object.keys(materialBeforeRelease.uniforms).join(',');

	device.reset();
	renderer.releaseSceneInstanceResources();
	const released = {
		deletedBuffers: device.find('deleteBuffer').length,
		deletedVertexArrays: device.find('deleteVertexArray').length,
		geometryIntact: geometryBeforeRelease === referenceRenderable.geometry &&
			geometryBeforeRelease.getAttribute('a_Position') !== undefined &&
			geometryBeforeRelease.instanceCount === -1,
		materialIntact: materialBeforeRelease === referenceRenderable.material &&
			materialBeforeRelease.type === referenceRenderable.material.type &&
			JSON.stringify(materialBeforeRelease.defines) === materialDefinesBeforeRelease &&
			Object.keys(materialBeforeRelease.uniforms).join(',') === materialUniformsBeforeRelease
	};

	const renderInfoAfterRelease = new RenderInfo();
	const drawAfterRelease = drawQueue(renderer, device, cameras[0], collectedA2, renderInfoAfterRelease);

	const result = {
		cameras: [
			{ id: cameras[0].id, description: describeQueue(collectedA), draw: drawA },
			{ id: cameras[1].id, description: describeQueue(collectedB), draw: drawB },
			{ id: cameras[0].id, description: describeQueue(collectedA2), draw: drawA2 },
			{ id: cameras[0].id, description: describeQueue(collectedA2), draw: drawAfterRelease }
		],
		release: released
	};

	writeFileSync(resolve(outputPath), JSON.stringify(result, null, 2));

	console.log('HEADLESS_SCENE_BATCHES cameras=' + result.cameras.length +
		' drawsA=' + drawA.draws.length + ' drawsB=' + drawB.draws.length);
}

main().catch(error => {
	console.error(error);
	process.exit(1);
});
