// Regression tests for the uniform block layout refactor.
//
// Verifies, through the public programs entry points AND through the
// WebGL-free src/uniform-block-layout.js module:
//   - column major and row major non-square matrices
//   - matrix arrays (arrayStride + matrixStride)
//   - vec3 arrays, scalars, int/uint/bool, nested struct members
//   - two blocks sharing one array/buffer with independent CPU/GPU offsets
//   - default GPU offset (bufferOffset omitted -> uses offset)
//   - missing block dummy behavior
//   - legacy hand written UniformBlockSpec (no stride fields)
//   - regular (non block) uniforms are unaffected
//
// Run: node regress/uniform-block-layout.test.mjs
import assert from 'node:assert/strict';
import * as p from '../src/programs.js';
import * as ubl from '../src/uniform-block-layout.js';

const ACTIVE_UNIFORMS                       = 0x8b86;
const ACTIVE_UNIFORM_BLOCKS                 = 0x8a36;
const UNIFORM_TYPE                          = 0x8a37;
const UNIFORM_SIZE                          = 0x8a38;
const UNIFORM_BLOCK_INDEX                   = 0x8a3a;
const UNIFORM_OFFSET                        = 0x8a3b;
const UNIFORM_ARRAY_STRIDE                  = 0x8a3c;
const UNIFORM_MATRIX_STRIDE                 = 0x8a3d;
const UNIFORM_IS_ROW_MAJOR                  = 0x8a3e;
const UNIFORM_BLOCK_DATA_SIZE               = 0x8a40;
const UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES  = 0x8a43;
const UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER   = 0x8a44;
const UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER = 0x8a46;
const UNIFORM_BUFFER                        = 0x8a11;

const FLOAT        = 0x1406;
const FLOAT_VEC3   = 0x8B51;
const FLOAT_VEC4   = 0x8B52;
const INT          = 0x1404;
const INT_VEC3     = 0x8B54;
const UNSIGNED_INT = 0x1405;
const BOOL         = 0x8B56;
const FLOAT_MAT2x3 = 0x8B65;
const FLOAT_MAT3x2 = 0x8B67;

const pnameToKey = new Map([
  [UNIFORM_TYPE, 'type'],
  [UNIFORM_SIZE, 'size'],
  [UNIFORM_BLOCK_INDEX, 'blockNdx'],
  [UNIFORM_OFFSET, 'offset'],
  [UNIFORM_ARRAY_STRIDE, 'arrayStride'],
  [UNIFORM_MATRIX_STRIDE, 'matrixStride'],
  [UNIFORM_IS_ROW_MAJOR, 'isRowMajor'],
]);

// A recording GL with the same field shapes a real WebGL2 context returns.
function makeGL(prog) {
  const calls = [];
  const gl = {
    UNIFORM_TYPE,
    UNIFORM_SIZE,
    UNIFORM_BLOCK_INDEX,
    UNIFORM_OFFSET,
    UNIFORM_ARRAY_STRIDE,
    UNIFORM_MATRIX_STRIDE,
    UNIFORM_IS_ROW_MAJOR,
    getProgramParameter: (_, pname) =>
      pname === ACTIVE_UNIFORMS ? prog.uniforms.length : prog.blocks.length,
    getActiveUniform: (_, ii) => {
      const u = prog.uniforms[ii];
      return { name: u.name, type: u.type, size: u.size };
    },
    getActiveUniforms: (_, indices, pname) =>
      indices.map(ii => prog.uniforms[ii][pnameToKey.get(pname)] ?? 0),
    getActiveUniformBlockName: (_, ii) => prog.blocks[ii].name,
    getUniformBlockIndex: (_, name) => prog.blocks.find(b => b.name === name).index,
    getActiveUniformBlockParameter: (_, ii, pname) => {
      const b = prog.blocks[ii];
      switch (pname) {
        case UNIFORM_BLOCK_DATA_SIZE: return b.size;
        case UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: return b.uniformIndices;
        case UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER: return true;
        case UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: return true;
      }
    },
    createBuffer: () => ({}),
    bindBuffer: () => {},
    bufferData: (...args) => calls.push(['bufferData', ...args]),
    uniformBlockBinding: () => {},
    bindBufferRange: (...args) => calls.push(['range', ...args]),
    bufferSubData: (target, dst, view, src, n) =>
      calls.push(['upload', dst, src, n, Array.from(view.slice(src, src + n))]),
    getUniformLocation: (_, name) => ({ name }),
    uniform1f: (loc, v) => calls.push(['uniform1f', loc.name, v]),
    uniform3fv: (loc, v) => calls.push(['uniform3fv', loc.name, Array.from(v)]),
  };
  return { gl, calls };
}

function floats(array, byteOffset, count) {
  return [...new Float32Array(array, byteOffset, count)];
}

let numCases = 0;
function check(name, fn) {
  fn();
  ++numCases;
}

// Creates the same block through the programs (online) entry points and
// through the offline layout module, sets the same values on both and
// asserts identical views and identical packed bytes.
function checkOnlineMatchesOffline(prog, blockName, values, options = {}) {
  const { gl } = makeGL(prog);
  const spec = p.createUniformBlockSpecFromProgram(gl, prog);
  const cpuOffset = options.offset ?? 0;
  const blockSize = spec.blockSpecs[blockName].size;
  const onlineArray = options.array ?? new ArrayBuffer(cpuOffset + blockSize);
  const online = p.createUniformBlockInfoFromProgram(
      gl, prog, spec, blockName, { ...options, array: onlineArray });
  const offline = ubl.createUniformBlockLayoutInfo(spec, blockName, {
    offset: cpuOffset,
    array: new ArrayBuffer(cpuOffset + blockSize),
  });
  p.setBlockUniforms(online, values);
  p.setBlockUniforms(offline, values);

  assert.deepEqual(Object.keys(online.uniforms).sort(), Object.keys(offline.uniforms).sort());
  for (const name of Object.keys(online.uniforms)) {
    const ov = online.uniforms[name];
    const fv = offline.uniforms[name];
    assert.equal(ov.constructor, fv.constructor, `${blockName}.${name} view type`);
    assert.equal(ov.byteOffset - cpuOffset, fv.byteOffset - cpuOffset, `${blockName}.${name} view offset`);
    assert.equal(ov.length, fv.length, `${blockName}.${name} view length`);
    // views map the original ArrayBuffer and stay inside the block
    assert.equal(ov.buffer, onlineArray);
    assert(ov.byteOffset + ov.byteLength <= cpuOffset + blockSize, `${blockName}.${name} view in range`);
  }
  assert.deepEqual(
      [...new Uint8Array(onlineArray, cpuOffset, blockSize)],
      [...new Uint8Array(offline.array, cpuOffset, blockSize)],
      `${blockName} packed bytes`);
  return { spec, online, offline };
}

// ---------------------------------------------------------------------------
// non-square matrices, column major and row major
// ---------------------------------------------------------------------------
{
  const mat2x3 = isRowMajor => ({
    uniforms: [
      { name: 'Scene.m', type: FLOAT_MAT2x3, size: 1, blockNdx: 0,
        offset: 0, arrayStride: 0, matrixStride: 16, isRowMajor },
    ],
    blocks: [{ name: 'Scene', index: 0, size: 48, uniformIndices: [0] }],
  });
  const values = { m: [1, 2, 3, 4, 5, 6] };

  check('column-major mat2x3 packs as 2 columns of vec3', () => {
    const { online } = checkOnlineMatchesOffline(mat2x3(false), 'Scene', values);
    assert.deepEqual(floats(online.array, 0, 8), [1, 2, 3, 0, 4, 5, 6, 0]);
  });

  check('createUniformBlockInfo(gl, programInfo, blockName) call form preserved', () => {
    const prog = mat2x3(false);
    const { gl } = makeGL(prog);
    const spec = p.createUniformBlockSpecFromProgram(gl, prog);
    const programInfo = { program: prog, uniformBlockSpec: spec };
    const info = p.createUniformBlockInfo(gl, programInfo, 'Scene');
    p.setBlockUniforms(info, values);
    assert.deepEqual(floats(info.array, 0, 8), [1, 2, 3, 0, 4, 5, 6, 0]);
  });

  check('row-major mat2x3 packs as 3 rows of vec2, padding untouched', () => {
    const prog = mat2x3(true);
    const { gl } = makeGL(prog);
    const spec = p.createUniformBlockSpecFromProgram(gl, prog);
    const array = new ArrayBuffer(48);
    new Float32Array(array).fill(0.5);  // sentinel
    const info = p.createUniformBlockInfoFromProgram(gl, prog, spec, 'Scene', { array });
    p.setBlockUniforms(info, values);
    // rows are [1,4], [2,5], [3,6]; padding must not be treated as input
    // nor written by the setter.
    assert.deepEqual(floats(array, 0, 12),
        [1, 4, 0.5, 0.5, 2, 5, 0.5, 0.5, 3, 6, 0.5, 0.5]);
    // offline module agrees
    const offline = ubl.createUniformBlockLayoutInfo(spec, 'Scene');
    p.setBlockUniforms(offline, values);
    assert.deepEqual(floats(offline.array, 0, 12), [1, 4, 0, 0, 2, 5, 0, 0, 3, 6, 0, 0]);
  });

  check('column-major mat3x2 packs as 3 columns of vec2', () => {
    const prog = {
      uniforms: [
        { name: 'Scene.m', type: FLOAT_MAT3x2, size: 1, blockNdx: 0,
          offset: 0, arrayStride: 0, matrixStride: 16, isRowMajor: false },
      ],
      blocks: [{ name: 'Scene', index: 0, size: 48, uniformIndices: [0] }],
    };
    const { online } = checkOnlineMatchesOffline(prog, 'Scene', values);
    assert.deepEqual(floats(online.array, 0, 12), [1, 2, 0, 0, 3, 4, 0, 0, 5, 6, 0, 0]);
  });
}

// ---------------------------------------------------------------------------
// matrix arrays honor both arrayStride and matrixStride
// ---------------------------------------------------------------------------
{
  const mat2x3Array = (isRowMajor, arrayStride, size) => ({
    uniforms: [
      { name: 'Scene.m[0]', type: FLOAT_MAT2x3, size: 2, blockNdx: 0,
        offset: 0, arrayStride, matrixStride: 16, isRowMajor },
    ],
    blocks: [{ name: 'Scene', index: 0, size, uniformIndices: [0] }],
  });
  const values = { m: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12] };

  check('column-major mat2x3[2] with arrayStride 32', () => {
    const { online } = checkOnlineMatchesOffline(mat2x3Array(false, 32, 64), 'Scene', values);
    assert.deepEqual(floats(online.array, 0, 16),
        [1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0, 10, 11, 12, 0]);
  });

  check('row-major mat2x3[2] with arrayStride 48', () => {
    const { online } = checkOnlineMatchesOffline(mat2x3Array(true, 48, 96), 'Scene', values);
    assert.deepEqual(floats(online.array, 0, 24), [
      1, 4, 0, 0, 2, 5, 0, 0, 3, 6, 0, 0,
      7, 10, 0, 0, 8, 11, 0, 0, 9, 12, 0, 0,
    ]);
  });
}

// ---------------------------------------------------------------------------
// vec3 array, scalars, int/uint/bool
// ---------------------------------------------------------------------------
{
  const prog = {
    uniforms: [
      { name: 'Mix.v[0]', type: FLOAT_VEC3, size: 2, blockNdx: 0,
        offset: 0, arrayStride: 16, matrixStride: 0, isRowMajor: false },
      { name: 'Mix.f', type: FLOAT, size: 1, blockNdx: 0,
        offset: 32, arrayStride: 0, matrixStride: 0, isRowMajor: false },
      { name: 'Mix.i', type: INT, size: 1, blockNdx: 0,
        offset: 36, arrayStride: 0, matrixStride: 0, isRowMajor: false },
      { name: 'Mix.u', type: UNSIGNED_INT, size: 1, blockNdx: 0,
        offset: 40, arrayStride: 0, matrixStride: 0, isRowMajor: false },
      { name: 'Mix.b', type: BOOL, size: 1, blockNdx: 0,
        offset: 44, arrayStride: 0, matrixStride: 0, isRowMajor: false },
      { name: 'Mix.iv', type: INT_VEC3, size: 1, blockNdx: 0,
        offset: 48, arrayStride: 0, matrixStride: 0, isRowMajor: false },
    ],
    blocks: [{ name: 'Mix', index: 0, size: 64, uniformIndices: [0, 1, 2, 3, 4, 5] }],
  };
  const values = { v: [1, 2, 3, 4, 5, 6], f: 7, i: -8, u: 9, b: true, iv: [10, 11, 12] };

  check('vec3 array + scalar/int/uint/bool members keep their types and offsets', () => {
    const { online } = checkOnlineMatchesOffline(prog, 'Mix', values);
    assert.deepEqual(floats(online.array, 0, 8), [1, 2, 3, 0, 4, 5, 6, 0]);
    assert.equal(new Float32Array(online.array, 32, 1)[0], 7);
    assert.equal(new Int32Array(online.array, 36, 1)[0], -8);
    assert.equal(new Uint32Array(online.array, 40, 1)[0], 9);
    assert.equal(new Uint32Array(online.array, 44, 1)[0], 1);
    assert.deepEqual([...new Int32Array(online.array, 48, 3)], [10, 11, 12]);
    assert(online.uniforms.i instanceof Int32Array);
    assert(online.uniforms.u instanceof Uint32Array);
    assert(online.uniforms.b instanceof Uint32Array);
    assert(online.uniforms.iv instanceof Int32Array);
    assert.equal(online.uniforms.v.length, 7);  // 2 vec3s + interior pad, no trailing pad
  });
}

// ---------------------------------------------------------------------------
// nested struct members
// ---------------------------------------------------------------------------
{
  const structMember = (name, type, size, offset, arrayStride = 0) => ({
    name, type, size, blockNdx: 0, offset, arrayStride, matrixStride: 0, isRowMajor: false,
  });
  const prog = {
    uniforms: [
      structMember('Lights.lights[0].intensity', FLOAT, 1, 0),
      structMember('Lights.lights[0].color', FLOAT_VEC4, 1, 16),
      structMember('Lights.lights[0].nearFar[0]', FLOAT, 2, 32, 16),
      structMember('Lights.lights[1].intensity', FLOAT, 1, 64),
      structMember('Lights.lights[1].color', FLOAT_VEC4, 1, 80),
      structMember('Lights.lights[1].nearFar[0]', FLOAT, 2, 96, 16),
    ],
    blocks: [{ name: 'Lights', index: 0, size: 128, uniformIndices: [0, 1, 2, 3, 4, 5] }],
  };

  check('struct array members set via nested objects, flat paths and partial paths', () => {
    const { online, offline } = checkOnlineMatchesOffline(prog, 'Lights', {
      lights: [
        { intensity: 5, color: [1, 0, 0, 1], nearFar: [0.5, 10] },
        { intensity: 2, color: [0, 0, 1, 1], nearFar: [0.25, 15] },
      ],
    });
    const f = floats(online.array, 0, 32);
    assert.equal(f[0], 5);
    assert.deepEqual(f.slice(4, 8), [1, 0, 0, 1]);
    assert.equal(f[8], 0.5);
    assert.equal(f[12], 10);
    assert.equal(f[16], 2);
    assert.deepEqual(f.slice(20, 24), [0, 0, 1, 1]);
    assert.equal(f[24], 0.25);
    assert.equal(f[28], 15);
    // flat path
    p.setBlockUniforms(online, { 'lights[1].intensity': 7 });
    assert.equal(new Float32Array(online.array, 64, 1)[0], 7);
    // partial path
    p.setBlockUniforms(online, { 'lights[0]': { intensity: 9 } });
    assert.equal(new Float32Array(online.array, 0, 1)[0], 9);
    // same entry points exist offline
    assert(typeof offline.setters['lights[0].intensity'] === 'function');
    assert(typeof offline.setters['lights[1].nearFar'] === 'function');
  });
}

// ---------------------------------------------------------------------------
// two blocks sharing one array and one buffer
// ---------------------------------------------------------------------------
{
  const prog = {
    uniforms: [
      { name: 'A.a', type: FLOAT_VEC4, size: 1, blockNdx: 0,
        offset: 0, arrayStride: 0, matrixStride: 0, isRowMajor: false },
      { name: 'A.f', type: FLOAT, size: 1, blockNdx: 0,
        offset: 16, arrayStride: 0, matrixStride: 0, isRowMajor: false },
      { name: 'B.b', type: FLOAT_VEC4, size: 1, blockNdx: 1,
        offset: 0, arrayStride: 0, matrixStride: 0, isRowMajor: false },
    ],
    blocks: [
      { name: 'A', index: 0, size: 32, uniformIndices: [0, 1] },
      { name: 'B', index: 1, size: 16, uniformIndices: [2] },
    ],
  };

  check('interleaved blocks keep sentinels and upload only their own range', () => {
    const { gl, calls } = makeGL(prog);
    const spec = p.createUniformBlockSpecFromProgram(gl, prog);
    const array = new ArrayBuffer(96);
    new Float32Array(array).fill(0.5);  // sentinel everywhere
    const buffer = {};
    // A: CPU 0, GPU defaults to CPU offset (0). B: CPU 64, GPU 128.
    const infoA = p.createUniformBlockInfoFromProgram(gl, prog, spec, 'A', { array, buffer });
    const infoB = p.createUniformBlockInfoFromProgram(
        gl, prog, spec, 'B', { array, offset: 64, buffer, bufferOffset: 128 });
    assert.equal(infoA.offset, 0);
    assert.equal(infoA.arrayOffset, 0);
    assert.equal(infoB.offset, 128);
    assert.equal(infoB.arrayOffset, 64);

    p.setBlockUniforms(infoA, { a: [1, 2, 3, 4], f: 5 });
    p.setBlockUniforms(infoB, { b: [6, 7, 8, 9] });
    // A's writes did not touch B's CPU region and vice versa
    assert.deepEqual(floats(array, 0, 5), [1, 2, 3, 4, 5]);
    assert.deepEqual(floats(array, 64, 4), [6, 7, 8, 9]);
    // sentinels between and after the blocks survive
    assert.deepEqual(floats(array, 20, 11), new Array(11).fill(0.5));
    assert.deepEqual(floats(array, 80, 4), new Array(4).fill(0.5));

    p.setUniformBlock(gl, spec, infoA);
    p.setUniformBlock(gl, spec, infoB);
    const uploads = calls.filter(c => c[0] === 'upload');
    const ranges = calls.filter(c => c[0] === 'range');
    // A: exactly 32 bytes from CPU 0 to GPU 0, bound range matches
    assert.deepEqual(uploads[0].slice(1, 4), [0, 0, 32]);
    assert.deepEqual(ranges[0], ['range', UNIFORM_BUFFER, 0, buffer, 0, 32]);
    // B: exactly 16 bytes from CPU 64 to GPU 128, bound range matches
    assert.deepEqual(uploads[1].slice(1, 4), [128, 64, 16]);
    assert.deepEqual(ranges[1], ['range', UNIFORM_BUFFER, 1, buffer, 128, 16]);
    // upload payloads are the blocks' own bytes
    assert.deepEqual([...new Float32Array(new Uint8Array(uploads[0][4]).buffer)].slice(0, 4), [1, 2, 3, 4]);
    assert.deepEqual([...new Float32Array(new Uint8Array(uploads[1][4]).buffer)], [6, 7, 8, 9]);
  });

  check('bufferOffset omitted -> GPU offset defaults to CPU offset', () => {
    const { gl, calls } = makeGL(prog);
    const spec = p.createUniformBlockSpecFromProgram(gl, prog);
    const array = new ArrayBuffer(64);
    const info = p.createUniformBlockInfoFromProgram(
        gl, prog, spec, 'B', { array, offset: 32, buffer: {} });
    assert.equal(info.offset, 32);
    assert.equal(info.arrayOffset, 32);
    p.setBlockUniforms(info, { b: [1, 2, 3, 4] });
    p.setUniformBlock(gl, spec, info);
    const upload = calls.find(c => c[0] === 'upload');
    assert.deepEqual(upload.slice(1, 4), [32, 32, 16]);
    const range = calls.find(c => c[0] === 'range');
    assert.equal(range[1], UNIFORM_BUFFER);
    assert.equal(range[2], 1);  // block index of B
    assert.deepEqual(range.slice(4), [32, 16]);
  });
}

// ---------------------------------------------------------------------------
// missing block keeps the dummy behavior
// ---------------------------------------------------------------------------
{
  check('missing block returns a harmless dummy', () => {
    const prog = {
      uniforms: [],
      blocks: [{ name: 'Real', index: 0, size: 16, uniformIndices: [] }],
    };
    const { gl, calls } = makeGL(prog);
    const spec = p.createUniformBlockSpecFromProgram(gl, prog);
    const origWarn = console.warn;
    console.warn = () => {};
    let dummy;
    try {
      dummy = p.createUniformBlockInfoFromProgram(gl, prog, spec, 'Nope');
    } finally {
      console.warn = origWarn;
    }
    assert.equal(dummy.name, 'Nope');
    assert.deepEqual(dummy.uniforms, {});
    p.setBlockUniforms(dummy, {});  // same empty-block behavior as before
    assert.equal(p.bindUniformBlock(gl, spec, dummy), false);
    p.setUniformBlock(gl, spec, dummy);
    assert.equal(calls.filter(c => c[0] === 'upload').length, 0);
    assert.equal(ubl.createUniformBlockLayoutInfo(spec, 'Nope'), null);
  });
}

// ---------------------------------------------------------------------------
// legacy hand written spec without stride fields: column major std140
// ---------------------------------------------------------------------------
{
  check('hand written spec without layout fields uses legacy std140 packing', () => {
    const prog = { uniforms: [], blocks: [] };  // not used, spec is hand written
    const { gl } = makeGL(prog);
    const spec = {
      blockSpecs: {
        L: { index: 0, size: 64, uniformIndices: [0, 1],
             usedByVertexShader: true, usedByFragmentShader: true, used: true },
      },
      uniformData: [
        { name: 'L.m', type: FLOAT_MAT2x3, size: 1, blockNdx: 0, offset: 0 },
        { name: 'L.v[0]', type: FLOAT_VEC3, size: 2, blockNdx: 0, offset: 32 },
      ],
    };
    const info = p.createUniformBlockInfoFromProgram(gl, {}, spec, 'L');
    p.setBlockUniforms(info, { m: [1, 2, 3, 4, 5, 6], v: [7, 8, 9, 10, 11, 12] });
    assert.deepEqual(floats(info.array, 0, 16),
        [1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0, 10, 11, 12, 0]);
    // offline module interprets the same spec the same way
    const offline = ubl.createUniformBlockLayoutInfo(spec, 'L');
    p.setBlockUniforms(offline, { m: [1, 2, 3, 4, 5, 6], v: [7, 8, 9, 10, 11, 12] });
    assert.deepEqual([...offline.asUint8], [...info.asUint8]);
  });
}

// ---------------------------------------------------------------------------
// regular uniforms are unaffected
// ---------------------------------------------------------------------------
{
  check('regular (non block) uniforms still work', () => {
    const prog = {
      uniforms: [
        { name: 'u_f', type: FLOAT, size: 1 },
        { name: 'u_v', type: FLOAT_VEC3, size: 1 },
      ],
      blocks: [],
    };
    const { gl, calls } = makeGL(prog);
    const setters = p.createUniformSetters(gl, prog);
    p.setUniforms(setters, { u_f: 3, u_v: [1, 2, 3] });
    assert.deepEqual(calls, [
      ['uniform1f', 'u_f', 3],
      ['uniform3fv', 'u_v', [1, 2, 3]],
    ]);
  });
}

console.log(`UNIFORM_BLOCK_LAYOUT_OK cases=${numCases}`);
