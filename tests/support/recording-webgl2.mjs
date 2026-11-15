/**
 * A recording WebGL2 device used by headless examples and tests.
 *
 * It implements the subset of the WebGL2 API used by the production renderer
 * and records every call so the produced draws, attribute bindings, program
 * switches, front face state and uploads can be inspected without a GPU.
 */

const GL = {
	DEPTH_BUFFER_BIT: 0x0100,
	STENCIL_BUFFER_BIT: 0x0400,
	COLOR_BUFFER_BIT: 0x4000,

	POINTS: 0, LINES: 1, LINE_LOOP: 2, LINE_STRIP: 3,
	TRIANGLES: 4, TRIANGLE_STRIP: 5, TRIANGLE_FAN: 6,

	ZERO: 0, ONE: 1,
	SRC_COLOR: 0x0300, ONE_MINUS_SRC_COLOR: 0x0301,
	SRC_ALPHA: 0x0302, ONE_MINUS_SRC_ALPHA: 0x0303,
	DST_ALPHA: 0x0304, ONE_MINUS_DST_ALPHA: 0x0305,
	DST_COLOR: 0x0306, ONE_MINUS_DST_COLOR: 0x0307,
	SRC_ALPHA_SATURATE: 0x0308,

	FUNC_ADD: 0x8006, FUNC_SUBTRACT: 0x800A, FUNC_REVERSE_SUBTRACT: 0x800B,
	MIN: 0x8007, MAX: 0x8008,

	BLEND: 0x0BE2, DEPTH_TEST: 0x0B71, SCISSOR_TEST: 0x0C11,
	CULL_FACE: 0x0B44, STENCIL_TEST: 0x0B90, POLYGON_OFFSET_FILL: 0x8037,
	SAMPLE_ALPHA_TO_COVERAGE: 0x809E,

	FRONT: 0x0404, BACK: 0x0405, FRONT_AND_BACK: 0x0408,
	CW: 0x0900, CCW: 0x0901,
	NEVER: 0x0200, LESS: 0x0201, EQUAL: 0x0202, LEQUAL: 0x0203,
	GREATER: 0x0204, NOTEQUAL: 0x0205, GEQUAL: 0x0206, ALWAYS: 0x0207,

	KEEP: 0x1E00, REPLACE: 0x1E01, INCR: 0x1E02, DECR: 0x1E03,
	INVERT: 0x150A, INCR_WRAP: 0x8507, DECR_WRAP: 0x8508,

	TEXTURE_2D: 0x0DE1, TEXTURE_CUBE_MAP: 0x8513,
	TEXTURE_CUBE_MAP_POSITIVE_X: 0x8515,
	TEXTURE0: 0x84C0,
	RGBA: 0x1908, RGB: 0x1907, ALPHA: 0x1906, LUMINANCE: 0x1909,
	UNSIGNED_BYTE: 0x1401, UNSIGNED_SHORT: 0x1403, UNSIGNED_INT: 0x1405,
	FLOAT: 0x1406, BYTE: 0x1400, SHORT: 0x1402, INT: 0x1404,
	HALF_FLOAT: 0x140B, DEPTH_COMPONENT: 0x1902, DEPTH_STENCIL: 0x84F9,
	UNSIGNED_INT_24_8: 0x84FA,

	NEAREST: 0x2600, LINEAR: 0x2601,
	NEAREST_MIPMAP_NEAREST: 0x2700, LINEAR_MIPMAP_NEAREST: 0x2701,
	NEAREST_MIPMAP_LINEAR: 0x2702, LINEAR_MIPMAP_LINEAR: 0x2703,
	TEXTURE_MAG_FILTER: 0x2800, TEXTURE_MIN_FILTER: 0x2801,
	TEXTURE_WRAP_S: 0x2802, TEXTURE_WRAP_T: 0x2803,
	CLAMP_TO_EDGE: 0x812F, REPEAT: 0x2901, MIRRORED_REPEAT: 0x8370,

	ARRAY_BUFFER: 0x8892, ELEMENT_ARRAY_BUFFER: 0x8893,
	STATIC_DRAW: 0x88E4, DYNAMIC_DRAW: 0x88E8, STREAM_DRAW: 0x88E0,

	VERTEX_SHADER: 0x8B31, FRAGMENT_SHADER: 0x8B30,
	COMPILE_STATUS: 0x8B81, LINK_STATUS: 0x8B82, VALIDATE_STATUS: 0x8B83,
	ACTIVE_UNIFORMS: 0x8B86, ACTIVE_ATTRIBUTES: 0x8B89,

	FLOAT_VEC2: 0x8B50, FLOAT_VEC3: 0x8B51, FLOAT_VEC4: 0x8B52,
	INT_VEC2: 0x8B53, INT_VEC3: 0x8B54, INT_VEC4: 0x8B55,
	BOOL: 0x8B56, BOOL_VEC2: 0x8B57, BOOL_VEC3: 0x8B58, BOOL_VEC4: 0x8B59,
	FLOAT_MAT2: 0x8B5A, FLOAT_MAT3: 0x8B5B, FLOAT_MAT4: 0x8B5C,
	SAMPLER_2D: 0x8B5E, SAMPLER_3D: 0x8B5F, SAMPLER_CUBE: 0x8B60,
	SAMPLER_2D_SHADOW: 0x8B62, SAMPLER_2D_ARRAY: 0x8DC1,
	SAMPLER_2D_ARRAY_SHADOW: 0x8DC4, SAMPLER_CUBE_SHADOW: 0x8DC5,
	INT_SAMPLER_2D: 0x8DCA, UNSIGNED_INT_SAMPLER_2D: 0x8DD2,

	FRAMEBUFFER: 0x8D40, READ_FRAMEBUFFER: 0x8CA8, DRAW_FRAMEBUFFER: 0x8CA9,
	COLOR_ATTACHMENT0: 0x8CE0, DEPTH_ATTACHMENT: 0x8D00,
	DEPTH_STENCIL_ATTACHMENT: 0x821A, STENCIL_ATTACHMENT: 0x8D20,
	RENDERBUFFER: 0x8D41,

	VERSION: 0x1F02,
	MAX_TEXTURE_IMAGE_UNITS: 0x8872,
	MAX_VERTEX_TEXTURE_IMAGE_UNITS: 0x8B4C,
	MAX_TEXTURE_SIZE: 0x0D33,
	MAX_CUBE_MAP_TEXTURE_SIZE: 0x851C,
	MAX_VERTEX_UNIFORM_VECTORS: 0x8DFB,
	MAX_SAMPLES: 0x8D57,
	ALIASED_LINE_WIDTH_RANGE: 0x846E,
	VIEWPORT: 0x0BA2,
	HIGH_FLOAT: 0x8DF2, MEDIUM_FLOAT: 0x8DF1, LOW_FLOAT: 0x8DF0,

	PIXEL_PACK_BUFFER: 0x88EB, STREAM_READ: 0x88E1,
	UNPACK_FLIP_Y_WEBGL: 0x9240, UNPACK_PREMULTIPLY_ALPHA_WEBGL: 0x9241,
	UNPACK_ALIGNMENT: 0x0CF5, UNPACK_COLORSPACE_CONVERSION_WEBGL: 0x9243,
	NONE: 0,
	SYNC_GPU_COMMANDS_COMPLETE: 0x9117, SYNC_FLUSH_COMMANDS_BIT: 0x00000001,
	WAIT_FAILED: 0x911D, TIMEOUT_EXPIRED: 0x9247,
	ANY_SAMPLES_PASSED: 0x8C2F, ANY_SAMPLES_PASSED_CONSERVATIVE: 0x8D6A
};

function createDevice(options = {}) {
	const version = options.version || 2;
	const calls = [];
	const uniformHistory = [];
	const state = {
		arrayBuffer: null,
		elementArrayBuffer: null,
		currentProgram: null,
		vertexArray: null,
		enabledAttributes: new Set(),
		divisors: new Map(),
		pointers: new Map(),
		activeTexture: GL.TEXTURE0,
		frontFace: GL.CCW,
		cullFace: GL.BACK,
		capabilities: { frontAndBack: false, front: false, back: true }
	};

	// Simple objects that stand in for GL resources.
	let nextId = 1;

	function record(name, args) {
		calls.push({ name: name, args: Array.prototype.slice.call(args) });
	}

	const gl = Object.assign({}, GL);

	gl.canvas = { width: 640, height: 480 };

	gl.getParameter = function(pname) {
		switch (pname) {
			case GL.VERSION: return 'WebGL ' + version + '.0 (recording)';
			case GL.MAX_TEXTURE_IMAGE_UNITS: return 16;
			case GL.MAX_VERTEX_TEXTURE_IMAGE_UNITS: return 16;
			case GL.MAX_TEXTURE_SIZE: return 4096;
			case GL.MAX_CUBE_MAP_TEXTURE_SIZE: return 4096;
			case GL.MAX_VERTEX_UNIFORM_VECTORS: return 1024;
			case GL.MAX_SAMPLES: return 4;
			case GL.ALIASED_LINE_WIDTH_RANGE: return [1, 1];
			case GL.VIEWPORT: return [0, 0, 640, 480];
			default: return 0;
		}
	};

	gl.getExtension = function() { return null; };
	gl.getShaderPrecisionFormat = function() { return { precision: 23, rangeMin: 127, rangeMax: 127 }; };

	gl.createBuffer = function() { record('createBuffer', arguments); return { __glBuffer: nextId++ }; };
	gl.deleteBuffer = function(buffer) { record('deleteBuffer', arguments); };
	gl.bindBuffer = function(target, buffer) {
		record('bindBuffer', arguments);
		if (target === GL.ARRAY_BUFFER) {
			state.arrayBuffer = buffer;
			if (state.vertexArray) state.vertexArray.__arrayBuffer = buffer;
		} else if (target === GL.ELEMENT_ARRAY_BUFFER) {
			state.elementArrayBuffer = buffer;
			if (state.vertexArray) state.vertexArray.__elementArrayBuffer = buffer;
		}
	};
	gl.bufferData = function(target, data, usage) {
		record('bufferData', arguments);
		const buffer = target === GL.ARRAY_BUFFER ? state.arrayBuffer : state.elementArrayBuffer;
		if (buffer) {
			buffer.__data = data && data.slice ? data.slice() : data;
			buffer.__usage = usage;
		}
	};
	gl.bufferSubData = function(target, offset, data) {
		record('bufferSubData', arguments);
		const buffer = target === GL.ARRAY_BUFFER ? state.arrayBuffer : state.elementArrayBuffer;
		if (buffer && buffer.__data && data && data.length !== undefined) {
			for (let i = 0; i < data.length; i++) buffer.__data[offset / 4 + i] = data[i];
		}
	};

	gl.createVertexArray = function() { record('createVertexArray', arguments); return { __glVao: nextId++ }; };
	gl.bindVertexArray = function(vao) {
		record('bindVertexArray', arguments);
		state.vertexArray = vao;
		if (vao) {
			if (vao.__enabledAttributes === undefined) {
				vao.__enabledAttributes = new Set();
				vao.__divisors = new Map();
				vao.__pointers = new Map();
				vao.__elementArrayBuffer = null;
				vao.__arrayBuffer = null;
			}
			state.enabledAttributes = vao.__enabledAttributes;
			state.divisors = vao.__divisors;
			state.pointers = vao.__pointers;
			state.elementArrayBuffer = vao.__elementArrayBuffer;
			state.arrayBuffer = vao.__arrayBuffer;
		} else {
			state.enabledAttributes = new Set();
			state.divisors = new Map();
			state.pointers = new Map();
			state.elementArrayBuffer = null;
			state.arrayBuffer = null;
		}
	};
	gl.deleteVertexArray = function(vao) { record('deleteVertexArray', arguments); };

	gl.enableVertexAttribArray = function(index) { record('enableVertexAttribArray', arguments); state.enabledAttributes.add(index); };
	gl.disableVertexAttribArray = function(index) { record('disableVertexAttribArray', arguments); state.enabledAttributes.delete(index); };
	gl.vertexAttribDivisor = function(index, divisor) { record('vertexAttribDivisor', arguments); state.divisors.set(index, divisor); };
	gl.vertexAttribPointer = function(index, size, type, normalized, stride, offset) {
		record('vertexAttribPointer', arguments);
		state.pointers.set(index, { buffer: state.arrayBuffer, size: size, type: type, normalized: normalized, stride: stride, offset: offset });
	};

	const createdPrograms = [];

	gl.createProgram = function() {
		record('createProgram', arguments);
		const program = { __glProgram: nextId++, __uniforms: [], __attributes: [] };
		createdPrograms.push(program);
		return program;
	};
	gl.deleteProgram = function(program) { record('deleteProgram', arguments); };
	gl.attachShader = function(program, shader) {
		record('attachShader', arguments);
		if (shader.type === GL.VERTEX_SHADER && !program.__vertexSource) {
			program.__vertexSource = shader.source;
		}
	};
	gl.linkProgram = function(program) {
		record('linkProgram', arguments);
		// Extract active attributes/uniforms from the attached shader sources.
		program.__uniforms = program.__vertexSource ? parseAttributes(program.__vertexSource).uniforms : [];
		program.__attributes = program.__vertexSource ? parseAttributes(program.__vertexSource).attributes : [];
		program.__shaderSource = program.__vertexSource || '';
	};
	gl.getProgramParameter = function(program, pname) {
		if (pname === GL.LINK_STATUS) return true;
		if (pname === GL.VALIDATE_STATUS) return true;
		if (pname === GL.ACTIVE_UNIFORMS) return program.__uniforms.length;
		if (pname === GL.ACTIVE_ATTRIBUTES) return program.__attributes.length;
		return 0;
	};
	gl.getProgramInfoLog = function() { return ''; };
	gl.getActiveUniform = function(program, index) {
		const uniform = program.__uniforms[index];
		return { name: uniform.name, type: uniform.type, size: uniform.size || 1 };
	};
	gl.getActiveAttrib = function(program, index) {
		const attribute = program.__attributes[index];
		return { name: attribute.name, type: attribute.type, size: attribute.size || 1 };
	};
	gl.getUniformLocation = function(program, name) { return { program: program, name: name }; };
	gl.getAttribLocation = function(program, name) {
		let index = program.__attributes.findIndex(a => a.name === name);
		// Emulate an implementation that keeps unused slots between attributes.
		if (index >= 0) index = index * 2;
		return index;
	};
	gl.getShaderPrecisionFormat = function() { return { precision: 23, rangeMin: 127, rangeMax: 127 }; };

	gl.createShader = function(type) {
		record('createShader', arguments);
		return { __shader: nextId++, type: type, source: '', compiled: true };
	};
	gl.shaderSource = function(shader, source) { shader.source = source; };
	gl.compileShader = function(shader) {
		shader.compiled = true;
		// Remember the vertex source so the program can expose attributes.
		if (shader.type === GL.VERTEX_SHADER) {
			shader.__program && (shader.__program.__vertexSource = shader.source);
		}
	};
	gl.getShaderParameter = function() { return true; };
	gl.getShaderInfoLog = function() { return ''; };
	gl.getShaderSource = function(shader) { return shader.source; };
	gl.deleteShader = function(shader) { record('deleteShader', arguments); };

	gl.useProgram = function(program) { record('useProgram', arguments); state.currentProgram = program; };

	gl.uniform1f = function(location, v) { record('uniform1f', arguments); recordUniform(location, [v]); };
	gl.uniform1fv = function(location, v) { record('uniform1fv', arguments); recordUniform(location, v); };
	gl.uniform1i = function(location, v) { record('uniform1i', arguments); recordUniform(location, [v]); };
	gl.uniform1iv = function(location, v) { record('uniform1iv', arguments); recordUniform(location, v); };
	gl.uniform2f = function(location, a, b) { record('uniform2f', arguments); recordUniform(location, [a, b]); };
	gl.uniform2fv = function(location, v) { record('uniform2fv', arguments); recordUniform(location, v); };
	gl.uniform3f = function(location, a, b, c) { record('uniform3f', arguments); recordUniform(location, [a, b, c]); };
	gl.uniform3fv = function(location, v) { record('uniform3fv', arguments); recordUniform(location, v); };
	gl.uniform4f = function(location, a, b, c, d) { record('uniform4f', arguments); recordUniform(location, [a, b, c, d]); };
	gl.uniform4fv = function(location, v) { record('uniform4fv', arguments); recordUniform(location, v); };
	gl.uniform2i = function(location, a, b) { record('uniform2i', arguments); recordUniform(location, [a, b]); };
	gl.uniform2iv = function(location, v) { record('uniform2iv', arguments); recordUniform(location, v); };
	gl.uniform3i = function(location, a, b, c) { record('uniform3i', arguments); recordUniform(location, [a, b, c]); };
	gl.uniform3iv = function(location, v) { record('uniform3iv', arguments); recordUniform(location, v); };
	gl.uniform4i = function(location, a, b, c, d) { record('uniform4i', arguments); recordUniform(location, [a, b, c, d]); };
	gl.uniform4iv = function(location, v) { record('uniform4iv', arguments); recordUniform(location, v); };
	gl.uniformMatrix2fv = function(location, transpose, v) { record('uniformMatrix2fv', arguments); recordUniform(location, toArray(v)); };
	gl.uniformMatrix3fv = function(location, transpose, v) { record('uniformMatrix3fv', arguments); recordUniform(location, toArray(v)); };
	gl.uniformMatrix4fv = function(location, transpose, v) { record('uniformMatrix4fv', arguments); recordUniform(location, toArray(v)); };

	function toArray(value) {
		return value && value.length !== undefined ? Array.from(value) : value;
	}

	function recordUniform(location, value) {
		if (!location || !location.name) return;
		uniformHistory.push({ name: location.name, value: toArray(value) });
	}

	gl.enable = function(cap) { record('enable', arguments); };
	gl.disable = function(cap) { record('disable', arguments); };
	gl.blendEquation = function() {};
	gl.blendEquationSeparate = function() {};
	gl.blendFunc = function() {};
	gl.blendFuncSeparate = function() {};
	gl.depthFunc = function() {};
	gl.depthMask = function() {};
	gl.colorMask = function() {};
	gl.stencilMask = function() {};
	gl.stencilFunc = function() {};
	gl.stencilFuncSeparate = function() {};
	gl.stencilOp = function() {};
	gl.stencilOpSeparate = function() {};
	gl.polygonOffset = function() {};
	gl.lineWidth = function() {};
	gl.clearColor = function() {};
	gl.clearDepth = function() {};
	gl.clearStencil = function() {};
	gl.clear = function() {};
	gl.viewport = function() { record('viewport', arguments); };
	gl.scissor = function() {};
	gl.cullFace = function(mode) { record('cullFace', arguments); state.cullFace = mode; };
	gl.frontFace = function(mode) { record('frontFace', arguments); state.frontFace = mode; };

	gl.createTexture = function() { record('createTexture', arguments); return { __glTexture: nextId++ }; };
	gl.deleteTexture = function() { record('deleteTexture', arguments); };
	gl.bindTexture = function() { record('bindTexture', arguments); };
	gl.texParameteri = function() {};
	gl.pixelStorei = function() {};
	gl.texImage2D = function() {};
	gl.texImage3D = function() {};
	gl.texSubImage2D = function() {};
	gl.texSubImage3D = function() {};
	gl.generateMipmap = function() {};
	gl.activeTexture = function() {};

	gl.createFramebuffer = function() { record('createFramebuffer', arguments); return { __glFramebuffer: nextId++ }; };
	gl.deleteFramebuffer = function() { record('deleteFramebuffer', arguments); };
	gl.bindFramebuffer = function() {};
	gl.framebufferTexture2D = function() {};
	gl.framebufferTextureLayer = function() {};
	gl.framebufferRenderbuffer = function() {};
	gl.drawBuffers = function() {};

	gl.createRenderbuffer = function() { return { __glRenderbuffer: nextId++ }; };
	gl.deleteRenderbuffer = function() {};
	gl.bindRenderbuffer = function() {};
	gl.renderbufferStorage = function() {};
	gl.renderbufferStorageMultisample = function() {};

	gl.drawElements = function(mode, count, type, offset) {
		record('drawElements', arguments);
		recordDrawDetail('drawElements');
	};
	gl.drawArrays = function(mode, first, count) {
		record('drawArrays', arguments);
		recordDrawDetail('drawArrays');
	};
	gl.drawElementsInstanced = function(mode, count, type, offset, instanceCount) {
		record('drawElementsInstanced', arguments);
		recordDrawDetail('drawElementsInstanced');
	};
	gl.drawArraysInstanced = function(mode, first, count, instanceCount) {
		record('drawArraysInstanced', arguments);
		recordDrawDetail('drawArraysInstanced');
	};

	const drawDetails = [];

	function recordDrawDetail(kind) {
		const program = state.currentProgram;
		const attributes = program && program.__attributes ? program.__attributes : [];
		let instanceLocation = -1;
		for (let i = 0; i < attributes.length; i++) {
			if (attributes[i].name === 'a_InstanceMatrix') instanceLocation = i * 2;
		}

		const pointer = instanceLocation >= 0 ? state.pointers.get(instanceLocation) : undefined;
		const instanceBuffer = pointer ? pointer.buffer : null;

		const divisors = {};
		for (const [location, divisor] of state.divisors) divisors[location] = divisor;

		let matrices = null;
		if (instanceBuffer && instanceBuffer.__data) {
			matrices = Array.from(instanceBuffer.__data.slice ? instanceBuffer.__data : []);
		}

		const elementBuffer = state.elementArrayBuffer;
		let indexData = null;
		if (elementBuffer && elementBuffer.__data) {
			indexData = Array.from(elementBuffer.__data);
		}

		let arrayBuffer = null;
		const knownPointers = state.pointers;
		for (const pointer of knownPointers.values()) {
			if (pointer.buffer && pointer.buffer !== elementBuffer) {
				arrayBuffer = pointer.buffer;
				break;
			}
		}

		drawDetails.push({
			kind: kind,
			programId: program ? program.__glProgram : -1,
			frontFace: state.frontFace,
			cullFace: state.cullFace,
			instanceMatrixLocation: instanceLocation,
			instanceMatrices: matrices,
			divisors: divisors,
			indexBuffer: elementBuffer ? elementBuffer.__glBuffer : null,
			indexData: indexData,
			vertexArrayId: state.vertexArray ? state.vertexArray.__glVao : 0
		});
	}

	gl.flush = function() {};
	gl.finish = function() {};
	gl.getError = function() { return 0; };

	return {
		gl: gl,
		calls: calls,
		drawDetails: drawDetails,
		uniformHistory: uniformHistory,
		state: state,
		GL: GL,
		reset() { calls.length = 0; drawDetails.length = 0; uniformHistory.length = 0; },
		programs() {
			return createdPrograms.slice();
		},
		draws() {
			return calls.filter(c => c.name === 'drawElements' || c.name === 'drawArrays' ||
				c.name === 'drawElementsInstanced' || c.name === 'drawArraysInstanced');
		},
		find(name) {
			return calls.filter(c => c.name === name);
		}
	};
}

function parseAttributes(source) {
	const attributes = [];
	const uniforms = [];

	const useInstanceBatch = /#define\s+USE_INSTANCE_BATCH\b/.test(source);
	const conditionalNames = new Set(['a_InstanceMatrix', 'u_InstanceAnchor']);

	const attributePattern = /(?:^|\n)\s*(?:layout\s*\([^)]*\)\s*)?(?:attribute|in)\s+(?:highp\s+|mediump\s+|lowp\s+)?(\w+)\s+(\w+)\s*;/g;
	let match;
	while ((match = attributePattern.exec(source)) !== null) {
		if (conditionalNames.has(match[2]) && !useInstanceBatch) continue;
		attributes.push({ name: match[2], type: typeFromName(match[1]), size: 1 });
	}

	const uniformPattern = /(?:^|\n)\s*uniform\s+(?:highp\s+|mediump\s+|lowp\s+)?(\w+)\s+(\w+)\s*(?:\[([^\]]+)\])?\s*;/g;
	while ((match = uniformPattern.exec(source)) !== null) {
		if (conditionalNames.has(match[2]) && !useInstanceBatch) continue;
		uniforms.push({ name: match[2], type: typeFromName(match[1]), size: match[3] ? parseInt(match[3]) || 1 : 1 });
	}

	// Routines that are defined by the shader chunks are not attributes.
	const filteredAttributes = attributes.filter(a => !a.name.startsWith('get') && !a.name.startsWith('is'));
	return { attributes: filteredAttributes, uniforms: uniforms };
}

function typeFromName(name) {
	switch (name) {
		case 'float': return GL.FLOAT;
		case 'vec2': return GL.FLOAT_VEC2;
		case 'vec3': return GL.FLOAT_VEC3;
		case 'vec4': return GL.FLOAT_VEC4;
		case 'int': return GL.INT;
		case 'ivec2': return GL.INT_VEC2;
		case 'ivec3': return GL.INT_VEC3;
		case 'ivec4': return GL.INT_VEC4;
		case 'bool': return GL.BOOL;
		case 'mat2': return GL.FLOAT_MAT2;
		case 'mat3': return GL.FLOAT_MAT3;
		case 'mat4': return GL.FLOAT_MAT4;
		case 'sampler2D': return GL.SAMPLER_2D;
		case 'sampler3D': return GL.SAMPLER_3D;
		case 'samplerCube': return GL.SAMPLER_CUBE;
		case 'sampler2DArray': return GL.SAMPLER_2D_ARRAY;
		case 'sampler2DShadow': return GL.SAMPLER_2D_SHADOW;
		default: return GL.FLOAT;
	}
}

export { createDevice, GL };
