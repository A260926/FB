import { BUFFER_USAGE } from '../const.js';

/**
 * Renderer-owned GPU resources for scene instance batches.
 *
 * Every renderer owns an instance of this class, so the same batch descriptor
 * can be drawn by two renderers without sharing buffers or vertex bindings.
 * The buffers and vertex arrays are created lazily, reused between draws and
 * released by {@link WebGLInstanceBatches#release}.
 */
class WebGLInstanceBatches {

	/**
	 * @param {string} prefix - Unused; kept for consistency with the other renderer resource classes.
	 * @param {WebGLRenderingContext} gl - The WebGL context.
	 * @param {WebGLCapabilities} capabilities - The renderer capabilities.
	 * @param {WebGLVertexArrayBindings} vertexArrayBindings - The vertex array bindings.
	 * @param {GPUMemoryInfo} gpuMemory - The GPU memory statistics.
	 */
	constructor(prefix, gl, capabilities, vertexArrayBindings, gpuMemory) {
		this._gl = gl;
		this._capabilities = capabilities;
		this._vertexArrayBindings = vertexArrayBindings;
		this._gpuMemory = gpuMemory;

		this._propertiesMap = new Map();
	}

	/**
	 * Get the renderer-owned resources for a batch.
	 *
	 * Resources are keyed by the batch drawing configuration instead of the
	 * batch descriptor object, so buffers and vertex arrays are reused when the
	 * queue is collected again for another camera or another frame.
	 * @param {string} key - A stable key describing the batch drawing configuration.
	 * @returns {object} The batch properties.
	 */
	get(key) {
		let properties = this._propertiesMap.get(key);

		if (properties === undefined) {
			properties = {
				glBuffer: undefined,
				byteLength: 0,
				vaos: undefined
			};
			this._propertiesMap.set(key, properties);
		}

		return properties;
	}

	/**
	 * Upload the batch world matrices, creating the GPU buffer on first use.
	 * @param {object} properties - The batch properties.
	 * @param {Float32Array} worldMatrices - Column-major world matrices.
	 */
	update(properties, worldMatrices) {
		const gl = this._gl;

		const byteLength = worldMatrices.byteLength;

		if (properties.glBuffer === undefined) {
			properties.glBuffer = gl.createBuffer();

			gl.bindBuffer(gl.ARRAY_BUFFER, properties.glBuffer);
			gl.bufferData(gl.ARRAY_BUFFER, worldMatrices, BUFFER_USAGE.DYNAMIC_DRAW);

			this._gpuMemory._updateBuffer(0, byteLength);
			properties.byteLength = byteLength;
		} else {
			gl.bindBuffer(gl.ARRAY_BUFFER, properties.glBuffer);

			if (properties.byteLength !== byteLength) {
				gl.bufferData(gl.ARRAY_BUFFER, worldMatrices, BUFFER_USAGE.DYNAMIC_DRAW);
				this._gpuMemory._updateBuffer(properties.byteLength, byteLength);
				properties.byteLength = byteLength;
			} else {
				gl.bufferSubData(gl.ARRAY_BUFFER, 0, worldMatrices);
			}
		}
	}

	/**
	 * Bind a vertex array with the geometry attributes and the batch's instance matrix attribute.
	 * @param {object} properties - The batch properties.
	 * @param {Geometry} geometry - The source geometry.
	 * @param {WebGLProgram} program - The program used to draw the batch.
	 */
	setup(properties, geometry, program) {
		if (this._capabilities.version < 2) {
			throw new Error('SceneInstanceBatches: instance batches require WebGL2.');
		}

		this._vertexArrayBindings.setupInstanceBatch(geometry, program, properties.glBuffer, properties);
	}

	/**
	 * Release all buffers and vertex arrays owned by this renderer.
	 * It is safe to call this method multiple times, and the resources are
	 * recreated automatically on the next draw.
	 */
	release() {
		const gl = this._gl;
		const vertexArrayBindings = this._vertexArrayBindings;

		for (const properties of this._propertiesMap.values()) {
			vertexArrayBindings.releaseInstanceProperties(properties);

			if (properties.glBuffer !== undefined) {
				gl.deleteBuffer(properties.glBuffer);
				this._gpuMemory._updateBuffer(properties.byteLength, 0);
				properties.glBuffer = undefined;
				properties.byteLength = 0;
			}
		}

		this._propertiesMap.clear();
	}

}

export { WebGLInstanceBatches };
