import { RenderCollector } from './RenderCollector.js';
import { Object3D } from '../scenes/Object3D.js';
import { BasicMaterial } from '../resources/materials/BasicMaterial.js';
import { PBRMaterial } from '../resources/materials/PBRMaterial.js';
import { Matrix3 } from '../math/Matrix3.js';
import { DRAW_MODE, MATERIAL_TYPE } from '../const.js';

const _matrix3 = new Matrix3();

/**
 * SceneInstanceCollector is a RenderCollector that merges compatible opaque mesh
 * renderables into instance batches while collecting the render queue.
 *
 * Convertible renderables keep their original geometry, material and group.
 * The merged renderable references the mesh with the smallest id, and exposes
 * an `instanceBatch` descriptor instead of copying, replacing or mutating any
 * source geometry, material or mesh.
 *
 * Usage:
 * ```js
 * scene.collector = new SceneInstanceCollector();
 * ```
 *
 * The normal render order does not change:
 * update matrices, `scene.updateRenderStates(camera)`,
 * `scene.updateRenderQueue(camera)`, then draw the returned queue.
 * @extends RenderCollector
 */
class SceneInstanceCollector extends RenderCollector {

	/**
	 * Create a SceneInstanceCollector.
	 * In most cases it should be installed with `scene.collector = new SceneInstanceCollector()`.
	 */
	constructor() {
		super();
	}

	/**
	 * Traverse the scene and collect renderables, then merge compatible opaque
	 * renderables into instance batches before returning the queue.
	 * @param {Scene} scene - The scene to traverse.
	 * @param {Camera} camera - The camera to collect renderable objects for.
	 * @returns {RenderQueue} The collected render queue.
	 */
	traverseAndCollect(scene, camera) {
		const renderQueue = super.traverseAndCollect(scene, camera);

		const layerList = renderQueue.layerList;
		for (let i = 0, l = layerList.length; i < l; i++) {
			this._buildInstanceBatches(layerList[i]);
		}

		return renderQueue;
	}

	_buildInstanceBatches(layer) {
		const opaque = layer.opaque;
		const count = layer.opaqueCount;

		// Cached render queue entries are reused between collections.
		// Clear any instance descriptor left by a previous collection.
		for (let i = 0; i < count; i++) {
			opaque[i].instanceBatch = null;
		}
		const transparent = layer.transparent;
		for (let i = 0, l = layer.transparentCount; i < l; i++) {
			transparent[i].instanceBatch = null;
		}

		const groupMap = new Map();

		for (let i = 0; i < count; i++) {
			const renderable = opaque[i];

			if (!isBatchable(renderable)) {
				continue;
			}

			const key = getBatchKey(renderable, layer.id);
			if (key === null) {
				continue;
			}

			let group = groupMap.get(key);
			if (group === undefined) {
				group = {
					entries: []
				};
				groupMap.set(key, group);
			}

			group.entries.push(renderable);
		}

		if (groupMap.size === 0) {
			return;
		}

		const drop = new Set();
		let hasBatch = false;

		for (const group of groupMap.values()) {
			if (group.entries.length < 2) {
				continue;
			}

			hasBatch = true;

			// Members are ordered by Mesh id, the smallest id represents the batch.
			group.entries.sort(compareMeshId);

			for (let i = 1, l = group.entries.length; i < l; i++) {
				drop.add(group.entries[i]);
			}

			const objects = new Array(group.entries.length);
			const worldMatrices = new Float32Array(group.entries.length * 16);

			let frontFaceCW = false;

			for (let i = 0, l = group.entries.length; i < l; i++) {
				const object = group.entries[i].object;

				objects[i] = object;
				object.worldMatrix.toArray(worldMatrices, i * 16);

				if (i === 0) {
					frontFaceCW = isFrontFaceCW(object.worldMatrix);
				}
			}

			// The surviving queue entry is the member with the smallest mesh id,
			// which is exactly what `object` refers to.
			const representative = group.entries[0];
			representative.instanceBatch = {
				objects: objects,
				worldMatrices: worldMatrices,
				count: objects.length,
				frontFaceCW: frontFaceCW
			};
		}

		if (!hasBatch) {
			return;
		}

		// Keep the original queue order, dropping the members that were merged
		// into a batch (except the representative, which takes their place).
		const result = new Array(count - drop.size);
		let cursor = 0;

		for (let i = 0; i < count; i++) {
			const renderable = opaque[i];
			if (!drop.has(renderable)) {
				result[cursor++] = renderable;
			}
		}

		layer.opaque = result;
		layer.opaqueCount = result.length;
	}

}

function isNativeMaterial(material) {
	if (!(material instanceof BasicMaterial) && !(material instanceof PBRMaterial)) {
		return false;
	}

	// Materials that are explicitly marked as instance materials (for example
	// the ones provided by addons) take their own drawing path.
	if (material.isInstanceMaterial || material.isInstancedMaterial || material.isSceneInstanceMaterial) {
		return false;
	}

	return material.type === MATERIAL_TYPE.BASIC || material.type === MATERIAL_TYPE.PBR;
}

function hasCustomCallback(object) {
	return object.onBeforeRender !== Object3D.prototype.onBeforeRender ||
		object.onAfterRender !== Object3D.prototype.onAfterRender;
}

function hasInstanceAttributes(geometry) {
	if (geometry.instanceCount >= 0) {
		return true;
	}

	for (const name in geometry.attributes) {
		if (geometry.attributes[name].divisor > 0) {
			return true;
		}
	}

	return false;
}

function hasMorph(geometry) {
	const morphAttributes = geometry.morphAttributes;
	for (const name in morphAttributes) {
		if (morphAttributes[name] && morphAttributes[name].length > 0) {
			return true;
		}
	}
	return false;
}

function hasTangent(geometry) {
	return geometry.getAttribute('a_Tangent') !== undefined && geometry.getAttribute('a_Tangent') !== null;
}

function isBatchable(renderable) {
	const object = renderable.object;
	const geometry = renderable.geometry;
	const material = renderable.material;
	const group = renderable.group;

	if (!object.isMesh || object.isSkinnedMesh || object.skeleton) {
		return false;
	}

	if (object.morphTargetInfluences || hasMorph(geometry)) {
		return false;
	}

	if (hasInstanceAttributes(geometry)) {
		return false;
	}

	if (hasCustomCallback(object)) {
		return false;
	}

	if (!isNativeMaterial(material) || material.transparent) {
		return false;
	}

	if (material.drawMode !== DRAW_MODE.TRIANGLES) {
		return false;
	}

	if (material.normalMap || hasTangent(geometry)) {
		return false;
	}

	if (group !== undefined && group !== null && group.multiDrawCount !== undefined) {
		return false;
	}

	return true;
}

function getBatchKey(renderable, layerId) {
	const object = renderable.object;
	const geometry = renderable.geometry;
	const material = renderable.material;
	const group = renderable.group;

	const matrix = object.worldMatrix.elements;
	for (let i = 0; i < 16; i++) {
		if (!Number.isFinite(matrix[i])) {
			return null;
		}
	}

	const determinant = _matrix3.setFromMatrix4(object.worldMatrix).determinant();
	if (!Number.isFinite(determinant) || determinant === 0) {
		return null;
	}

	const groupStart = group !== undefined && group !== null ? group.start : -1;
	const groupCount = group !== undefined && group !== null ? group.count : -1;
	const groupMaterialIndex = group !== undefined && group !== null && group.materialIndex !== undefined ? group.materialIndex : -1;

	return geometry.id + '|' + material.id + '|' + groupStart + '|' + groupCount + '|' + groupMaterialIndex + '|' +
		layerId + '|' + object.renderOrder + '|' + (object.receiveShadow ? 1 : 0) + '|' + object.shadowType + '|' +
		(determinant < 0 ? 1 : 0);
}

function isFrontFaceCW(worldMatrix) {
	return _matrix3.setFromMatrix4(worldMatrix).determinant() < 0;
}

function compareMeshId(a, b) {
	return a.object.id - b.object.id;
}

export { SceneInstanceCollector };
