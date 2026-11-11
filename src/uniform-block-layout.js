/*
 * Copyright 2019 Gregg Tavares
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * Uniform block CPU-side layout functions.
 *
 * This module is WebGL free. It takes the reflection data captured in a
 * `UniformBlockSpec` (see {@link module:twgl.createUniformBlockSpecFromProgram})
 * and computes the CPU side views and setters that pack JavaScript values
 * into an `ArrayBuffer` using the block's actual offsets, strides and matrix
 * order. It exists so tools that do not have a WebGL context (for example
 * offline material inspectors) can share the exact same packing code as
 * the WebGL path in `programs.js`. There is only one packing algorithm and
 * it lives here.
 *
 * @module twgl/uniform-block-layout
 */

const FLOAT                         = 0x1406;
const FLOAT_VEC2                    = 0x8B50;
const FLOAT_VEC3                    = 0x8B51;
const FLOAT_VEC4                    = 0x8B52;
const INT                           = 0x1404;
const INT_VEC2                      = 0x8B53;
const INT_VEC3                      = 0x8B54;
const INT_VEC4                      = 0x8B55;
const BOOL                          = 0x8B56;
const BOOL_VEC2                     = 0x8B57;
const BOOL_VEC3                     = 0x8B58;
const BOOL_VEC4                     = 0x8B59;
const FLOAT_MAT2                    = 0x8B5A;
const FLOAT_MAT3                    = 0x8B5B;
const FLOAT_MAT4                    = 0x8B5C;
const FLOAT_MAT2x3                  = 0x8B65;
const FLOAT_MAT2x4                  = 0x8B66;
const FLOAT_MAT3x2                  = 0x8B67;
const FLOAT_MAT3x4                  = 0x8B68;
const FLOAT_MAT4x2                  = 0x8B69;
const FLOAT_MAT4x3                  = 0x8B6A;
const UNSIGNED_INT                  = 0x1405;
const UNSIGNED_INT_VEC2             = 0x8DC6;
const UNSIGNED_INT_VEC3             = 0x8DC7;
const UNSIGNED_INT_VEC4             = 0x8DC8;

/**
 * Type info for uniforms that can appear in a uniform block.
 * `size` is the tightly packed size in bytes. For matrices `rows` is the
 * number of GLSL columns and `cols` the number of GLSL rows (this matches
 * the historical naming in programs.js). For vectors `cols` is the number
 * of components.
 * @private
 */
const typeMap = {};
typeMap[FLOAT]             = { Type: Float32Array, size:  4, };
typeMap[FLOAT_VEC2]        = { Type: Float32Array, size:  8, cols: 2, };
typeMap[FLOAT_VEC3]        = { Type: Float32Array, size: 12, cols: 3, };
typeMap[FLOAT_VEC4]        = { Type: Float32Array, size: 16, cols: 4, };
typeMap[INT]               = { Type: Int32Array,   size:  4, };
typeMap[INT_VEC2]          = { Type: Int32Array,   size:  8, cols: 2, };
typeMap[INT_VEC3]          = { Type: Int32Array,   size: 12, cols: 3, };
typeMap[INT_VEC4]          = { Type: Int32Array,   size: 16, cols: 4, };
typeMap[UNSIGNED_INT]      = { Type: Uint32Array,  size:  4, };
typeMap[UNSIGNED_INT_VEC2] = { Type: Uint32Array,  size:  8, cols: 2, };
typeMap[UNSIGNED_INT_VEC3] = { Type: Uint32Array,  size: 12, cols: 3, };
typeMap[UNSIGNED_INT_VEC4] = { Type: Uint32Array,  size: 16, cols: 4, };
typeMap[BOOL]              = { Type: Uint32Array,  size:  4, };
typeMap[BOOL_VEC2]         = { Type: Uint32Array,  size:  8, cols: 2, };
typeMap[BOOL_VEC3]         = { Type: Uint32Array,  size: 12, cols: 3, };
typeMap[BOOL_VEC4]         = { Type: Uint32Array,  size: 16, cols: 4, };
typeMap[FLOAT_MAT2]        = { Type: Float32Array, size: 32, rows: 2, cols: 2, };
typeMap[FLOAT_MAT3]        = { Type: Float32Array, size: 48, rows: 3, cols: 3, };
typeMap[FLOAT_MAT4]        = { Type: Float32Array, size: 64, rows: 4, cols: 4, };
typeMap[FLOAT_MAT2x3]      = { Type: Float32Array, size: 32, rows: 2, cols: 3, };
typeMap[FLOAT_MAT2x4]      = { Type: Float32Array, size: 32, rows: 2, cols: 4, };
typeMap[FLOAT_MAT3x2]      = { Type: Float32Array, size: 48, rows: 3, cols: 2, };
typeMap[FLOAT_MAT3x4]      = { Type: Float32Array, size: 48, rows: 3, cols: 4, };
typeMap[FLOAT_MAT4x2]      = { Type: Float32Array, size: 64, rows: 4, cols: 2, };
typeMap[FLOAT_MAT4x3]      = { Type: Float32Array, size: 64, rows: 4, cols: 3, };

const arraySuffixRE = /\[\d+\]\.$/;  // better way to check?

const pad = (v, padding) => ((v + (padding - 1)) / padding | 0) * padding;

const tokenRE = /(\.|\[|]|\w+)/g;
const isDigit = s => s >= '0' && s <= '9';

function setUniformTree(tree, values) {
  for (const name in values) {
    const prop = tree[name];
    if (typeof prop === 'function') {
      prop(values[name]);
    } else {
      setUniformTree(tree[name], values[name]);
    }
  }
}

/**
 * Adds a setter to a tree of setters so structures and arrays of
 * structures can be set with nested JavaScript objects/arrays.
 * Also adds setters for each partial path to `uniformSetters`.
 * @private
 */
function addSetterToUniformTree(fullPath, setter, node, uniformSetters) {
  const tokens = fullPath.split(tokenRE).filter(s => s !== '');
  let tokenNdx = 0;
  let path = '';

  for (;;) {
    const token = tokens[tokenNdx++];  // has to be name or number
    path += token;
    const isArrayIndex = isDigit(token[0]);
    const accessor = isArrayIndex
        ? parseInt(token)
        : token;
    if (isArrayIndex) {
      path += tokens[tokenNdx++];  // skip ']'
    }
    const isLastToken = tokenNdx === tokens.length;
    if (isLastToken) {
      node[accessor] = setter;
      break;
    } else {
      const token = tokens[tokenNdx++];  // has to be . or [
      const isArray = token === '[';
      const child = node[accessor] || (isArray ? [] : {});
      node[accessor] = child;
      node = child;
      uniformSetters[path] = uniformSetters[path] || function(node) {
        return function(value) {
          setUniformTree(node, value);
        };
      }(child);
      path += token;
    }
  }
}

/**
 * The legacy setter used when a hand written `UniformBlockSpec` does not
 * provide `arrayStride` / `matrixStride` / `isRowMajor`. It assumes column
 * major matrices and std140 style vec4 aligned rows, exactly like twgl
 * always did before reflection strides were captured.
 * @private
 */
function createLegacyUniformBlockUniformSetter(view, isArray, rows, cols) {
  if (isArray || rows) {
    cols = cols || 1;
    const numElements = view.length;
    const totalRows = numElements / 4;
    return function(value) {
      let dst = 0;
      let src = 0;
      for (let row = 0; row < totalRows; ++row) {
        for (let col = 0; col < cols; ++col) {
          view[dst++] = value[src++];
        }
        dst += 4 - cols;
      }
    };
  } else {
    return function(value) {
      if (value.length) {
        view.set(value);
      } else {
        view[0] = value;
      }
    };
  }
}

/**
 * Creates a setter that packs a compact JavaScript value into `view`
 * following the reflected layout. Matrix values are always passed in as
 * compact column major (the same format as `gl.uniformMatrix*fv`) and are
 * scattered to the reflected offsets, strides and matrix order. Padding
 * bytes are never written.
 * @private
 */
function createStridedUniformBlockUniformSetter(view, layout) {
  const {
    elements, elementStride,
    vectorsPerElement, vectorStride,
    components, srcVectorMul, srcComponentMul,
  } = layout;
  return function(value) {
    if (elements === 1 && vectorsPerElement === 1 && components === 1 && value.length === undefined) {
      view[0] = value;
      return;
    }
    const srcElementStride = vectorsPerElement * components;
    for (let elem = 0; elem < elements; ++elem) {
      const elemBase = elem * elementStride;
      const srcElemBase = elem * srcElementStride;
      for (let vec = 0; vec < vectorsPerElement; ++vec) {
        let dst = elemBase + vec * vectorStride;
        let src = srcElemBase + vec * srcVectorMul;
        for (let comp = 0; comp < components; ++comp) {
          view[dst++] = value[src];
          src += srcComponentMul;
        }
      }
    }
  };
}

/**
 * Computes the CPU layout for one uniform.
 *
 * @param {module:twgl.UniformData} data reflection data for the uniform
 * @param {Object} typeInfo entry from the type map
 * @param {boolean} isArray true if the uniform is an array
 * @return {Object} layout description
 * @private
 */
function computeUniformLayout(data, typeInfo, isArray) {
  const isMatrix = typeInfo.rows !== undefined;
  const isRowMajor = isMatrix && !!data.isRowMajor;
  // For matrices: storage vectors are columns (column major) or rows
  // (row major). `typeInfo.rows`/`typeInfo.cols` are the GLSL column/row
  // counts so the storage vector count and its component count swap with
  // the matrix order.
  const vectorsPerElement = isMatrix
      ? (isRowMajor ? typeInfo.cols : typeInfo.rows)
      : 1;
  const components = isMatrix
      ? (isRowMajor ? typeInfo.rows : typeInfo.cols)
      : (typeInfo.cols || 1);
  const vectorStride = isMatrix
      ? (data.matrixStride || 16)
      : typeInfo.size;
  const elements = isArray ? data.size : 1;
  const elementStride = isArray
      ? (data.arrayStride || pad(typeInfo.size, 16))
      : 0;
  // The footprint of the last element does not include its trailing
  // padding so views never extend past the uniform's actual data.
  const lastElementSize = isMatrix
      ? vectorStride * (vectorsPerElement - 1) + components * 4
      : typeInfo.size;
  const byteLength = (isArray ? elementStride * (elements - 1) : 0) + lastElementSize;
  // Matrix values arrive compact column major: source index of the
  // component at GLSL (column c, row r) is c * glslRows + r.
  //   column major: storage vector v = column v     -> src = v * comps + c
  //   row major:    storage vector v = row v        -> src = c * rows + v
  const srcVectorMul = isRowMajor ? 1 : components;
  const srcComponentMul = isRowMajor ? vectorsPerElement : 1;
  return {
    elements,
    elementStride,
    vectorsPerElement,
    vectorStride,
    components,
    srcVectorMul,
    srcComponentMul,
    byteLength,
  };
}

/**
 * The CPU side of a uniform block: typed array views and setters for each
 * uniform in the block.
 *
 * @typedef {Object} UniformBlockLayout
 * @property {Object<string, ArrayBufferView>} uniforms A uniform name to
 *    `ArrayBufferView` map. Each view maps the given `array` at the
 *    uniform's reflected offset and never extends past the uniform's
 *    actual data.
 * @property {Object<string, function>} setters A uniform name to setter
 *    map, including setters for partial paths of structures and arrays of
 *    structures. Matrix values are compact column major.
 * @memberOf module:twgl/uniform-block-layout
 */

/**
 * Creates the CPU side views and setters for one uniform block from its
 * reflection data.
 *
 * `blockSpec` and `uniformData` come from a
 * {@link module:twgl.UniformBlockSpec}. When the `UniformData` entries
 * provide `arrayStride`, `matrixStride` and `isRowMajor` (as captured by
 * `createUniformBlockSpecFromProgram`) the data is mapped using those
 * actual offsets, strides and matrix order. When they are absent (old
 * hand written specs) the uniform is interpreted as column major std140,
 * which is what twgl has always assumed.
 *
 * @param {module:twgl.BlockSpec} blockSpec The BlockSpec for the block.
 * @param {module:twgl.UniformData[]} uniformData Reflection data for each uniform.
 * @param {string} blockName The name of the block, used to strip the
 *    `BlockName.` prefix from uniform names.
 * @param {ArrayBuffer} array The array buffer the views map into.
 * @param {number} [offset] The byte offset in `array` where this block
 *    starts (default = 0).
 * @return {module:twgl/uniform-block-layout.UniformBlockLayout} The views and setters.
 * @memberOf module:twgl/uniform-block-layout
 */
function createUniformBlockLayout(blockSpec, uniformData, blockName, array, offset = 0) {
  let prefix = blockName + ".";
  if (arraySuffixRE.test(prefix)) {
    prefix = prefix.replace(arraySuffixRE, ".");
  }
  const uniforms = {};
  const setters = {};
  const setterTree = {};
  blockSpec.uniformIndices.forEach(function(uniformNdx) {
    const data = uniformData[uniformNdx];
    let name = data.name;
    if (name.startsWith(prefix)) {
      name = name.substr(prefix.length);
    }
    const isArray = name.endsWith('[0]');
    if (isArray) {
      name = name.substr(0, name.length - 3);
    }
    const typeInfo = typeMap[data.type];
    const Type = typeInfo.Type;
    const hasReflectedLayout = data.arrayStride !== undefined ||
        data.matrixStride !== undefined ||
        data.isRowMajor !== undefined;
    let uniformView;
    let setter;
    if (hasReflectedLayout) {
      const layout = computeUniformLayout(data, typeInfo, isArray);
      uniformView = new Type(array, offset + data.offset, layout.byteLength / Type.BYTES_PER_ELEMENT);
      setter = createStridedUniformBlockUniformSetter(uniformView, {
        elements: layout.elements,
        elementStride: layout.elementStride / Type.BYTES_PER_ELEMENT,
        vectorsPerElement: layout.vectorsPerElement,
        vectorStride: layout.vectorStride / Type.BYTES_PER_ELEMENT,
        components: layout.components,
        srcVectorMul: layout.srcVectorMul,
        srcComponentMul: layout.srcComponentMul,
      });
    } else {
      const byteLength = isArray
          ? pad(typeInfo.size, 16) * data.size
          : typeInfo.size * data.size;
      uniformView = new Type(array, offset + data.offset, byteLength / Type.BYTES_PER_ELEMENT);
      setter = createLegacyUniformBlockUniformSetter(uniformView, isArray, typeInfo.rows, typeInfo.cols);
    }
    uniforms[name] = uniformView;
    // Note: I'm not sure what to do here. The original
    // idea was to create TypedArray views into each part
    // of the block. This is useful, for example if you have
    // a block with { mat4: model; mat4 view; mat4 projection; }
    // you'll get a Float32Array for each one suitable for
    // passing to most JS math libraries including twgl's and glMatrix.js.
    //
    // But, if you have a an array of structures, especially if that
    // array is large, you get a whole bunch of TypedArray views.
    // Every one of them has overhead and switching between them all
    // is probably a cache miss. In that case it would really be better
    // to just have one view (asFloat) and have all the setters
    // just reference the correct portion. But, then you can't easily
    // treat a matrix, or a vec4, as a standalone thing like you can
    // with all the views.
    //
    // Another problem with the views is they are not shared. With
    // uniforms you have one set of setters. With UniformBlockInfo
    // you have a set of setters *pre block instance*. That's because
    // TypedArray views can't be mapped to different buffers.
    //
    // My gut right now is if you really want the speed and compactness
    // then you should probably roll your own solution. TWGL's goal
    // here is ease of use as AFAICT there is no simple generic efficient
    // solution.
    setters[name] = setter;
    addSetterToUniformTree(name, setter, setterTree, setters);
  });
  return {
    uniforms,
    setters,
  };
}

/**
 * A WebGL free equivalent of `UniformBlockInfo`: an `ArrayBuffer` holding
 * the block's values plus the views and setters that pack them. There is
 * no `WebGLBuffer` because this module never touches WebGL.
 *
 * @typedef {Object} UniformBlockLayoutInfo
 * @property {string} name The name of the block
 * @property {ArrayBuffer} array The array buffer that contains the uniform values
 * @property {Float32Array} asFloat A float view on the array buffer.
 * @property {Uint8Array} asUint8 A uint8 view on the array buffer.
 * @property {number} offset The byte offset in `array` where this block starts.
 * @property {number} size The size in bytes of the block.
 * @property {Object<string, ArrayBufferView>} uniforms See {@link module:twgl/uniform-block-layout.UniformBlockLayout}
 * @property {Object<string, function>} setters See {@link module:twgl/uniform-block-layout.UniformBlockLayout}
 * @memberOf module:twgl/uniform-block-layout
 */

/**
 * Options for {@link module:twgl/uniform-block-layout.createUniformBlockLayoutInfo}
 *
 * @typedef {Object} UniformBlockLayoutInfoOptions
 * @property {ArrayBuffer} [array] an existing array buffer to use for values
 * @property {number} [offset] the offset in bytes to use in the array buffer (default = 0)
 * @memberOf module:twgl/uniform-block-layout
 */

/**
 * Creates a `UniformBlockLayoutInfo` for the specified block without
 * touching WebGL. The result has the same CPU side shape as the
 * `UniformBlockInfo` created by `createUniformBlockInfoFromProgram` so
 * offline tools can inspect or fill a block with the exact same layout.
 *
 * @param {module:twgl.UniformBlockSpec} uniformBlockSpec A UniformBlockSpec as returned
 *     from {@link module:twgl.createUniformBlockSpecFromProgram} or written by hand.
 * @param {string} blockName The name of the block.
 * @param {module:twgl/uniform-block-layout.UniformBlockLayoutInfoOptions} [options] Optional options.
 * @return {module:twgl/uniform-block-layout.UniformBlockLayoutInfo|null} The layout info
 *     or `null` if the block does not exist in the spec.
 * @memberOf module:twgl/uniform-block-layout
 */
function createUniformBlockLayoutInfo(uniformBlockSpec, blockName, options = {}) {
  const blockSpec = uniformBlockSpec.blockSpecs[blockName];
  if (!blockSpec) {
    return null;
  }
  const offset = options.offset ?? 0;
  const array = options.array ?? new ArrayBuffer(blockSpec.size);
  const { uniforms, setters } = createUniformBlockLayout(
      blockSpec, uniformBlockSpec.uniformData, blockName, array, offset);
  return {
    name: blockName,
    array,
    asFloat: new Float32Array(array),
    asUint8: new Uint8Array(array),
    uniforms,
    setters,
    offset,
    size: blockSpec.size,
  };
}

export {
  addSetterToUniformTree,
  createUniformBlockLayout,
  createUniformBlockLayoutInfo,
};
