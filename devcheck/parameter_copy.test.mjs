/**
 * Parameter copy-in / copy-out regression tests.
 *
 * Every case is compiled through the public `GLSL(options)(source)` entry point,
 * executed by calling the generated `main`, and then checked against a list of
 * expected gl_FragColor components that is transcribed by hand / computed from
 * an independent data model below.  The compiler output is never used to
 * backfill the expectation and no generator helper is borrowed as a reference.
 *
 * Run normally:
 *   node devcheck/parameter_copy.test.mjs
 *
 * Run in "capture" mode against a deliberately mutated compiler copy:
 *   PARAMETER_COPY_CAPTURE=1 \
 *   PARAMETER_COPY_EXPECT_HITS=case1,case2 \
 *   PARAMETER_COPY_PATCH_LABEL=some-patch \
 *   node devcheck/parameter_copy.test.mjs
 *
 * Capture mode exits 0 only when the mutation is a *numeric* regression:
 * every fixture still compiles and executes, controls still pass, and each
 * expected hit fails in both optimize modes.  Compile errors, runtime errors,
 * zero executions, control failures and missing hits are reported separately
 * and make the capture invalid.
 */
import GLSL from '../index.js'
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const FIXTURE_DIR = path.join(HERE, 'fixtures', 'parameter-copy')
const OPTIMIZE_MODES = [false, true]
const COMPONENT_NAMES = ['x', 'y', 'z', 'w']
const EPSILON = 1e-6

/**
 * Expected vectors are hand-calculated:
 *  - an `in` parameter is a private copy, so any mutation inside the function
 *    must leave the caller untouched;
 *  - `out`/`inout` actuals are written back when the function ends and on every
 *    return path, including early returns from non-void functions;
 *  - a function result is independent of its out parameters;
 *  - output actuals target a specific array slot / struct field / swizzled
 *    component / matrix column, and a side-effecting index runs exactly once.
 */
const CASES = [
	{ name: 'in_vec_unchanged', file: 'in_vec_unchanged.glsl', expected: [1, 2, 3, 1], control: false },
	{ name: 'in_mat_unchanged', file: 'in_mat_unchanged.glsl', expected: [1, 2, 3, 4], control: false },
	{ name: 'in_struct_unchanged', file: 'in_struct_unchanged.glsl', expected: [1, 2, 3, 1], control: false },
	{ name: 'in_struct_array_unchanged', file: 'in_struct_array_unchanged.glsl', expected: [1, 2, 3, 4], control: false },
	{ name: 'out_normal_end', file: 'out_normal_end.glsl', expected: [5, 6, 7, 1], control: false },
	{ name: 'out_early_return', file: 'out_early_return.glsl', expected: [7, 8, 9, 1], control: false },
	{ name: 'out_early_return_value', file: 'out_early_return_value.glsl', expected: [7, 8, 7, 1], control: false },
	{ name: 'inout_normal_end', file: 'inout_normal_end.glsl', expected: [6, 0, 0, 1], control: false },
	{ name: 'inout_partial_vec', file: 'inout_partial_vec.glsl', expected: [1, 12, 3, 1], control: false },
	{ name: 'inout_early_return_value', file: 'inout_early_return_value.glsl', expected: [1, 6, 7, 1], control: false },
	{ name: 'out_and_return_value', file: 'out_and_return_value.glsl', expected: [2.5, 4, 6, 9], control: false },
	{ name: 'array_element_out', file: 'array_element_out.glsl', expected: [1, 9, 0, 1], control: false },
	{ name: 'struct_field_out', file: 'struct_field_out.glsl', expected: [1, 9, 0, 1], control: false },
	{ name: 'swizzle_reorder_out', file: 'swizzle_reorder_out.glsl', expected: [8, 2, 7, 1], control: false },
	{ name: 'matrix_column_out', file: 'matrix_column_out.glsl', expected: [1, 2, 7, 8], control: false },
	{ name: 'matrix_element_out', file: 'matrix_element_out.glsl', expected: [1, 9, 3, 4], control: false },
	{ name: 'dynamic_index_single_eval', file: 'dynamic_index_single_eval.glsl', expected: [1, 5, 1, 4], control: false },
	{ name: 'struct_by_value_return', file: 'struct_by_value_return.glsl', expected: [1, 2, 9, 7], control: false },
	{ name: 'multi_out_inout_return', file: 'multi_out_inout_return.glsl', expected: [0.25, 2, 3, 0.5], control: false },
	{ name: 'inout_struct_deep', file: 'inout_struct_deep.glsl', expected: [1, 9, 8, 4], control: false },
	{ name: 'control_scalar_arithmetic', file: 'control_scalar_arithmetic.glsl', expected: [3, 4, 0, 1], control: true },
	{ name: 'control_in_scalar_modified', file: 'control_in_scalar_modified.glsl', expected: [1, 2, 3, 1], control: true },
	{ name: 'control_struct_scalar_only', file: 'control_struct_scalar_only.glsl', expected: [1, 2, 0, 1], control: true }
]

const CASE_BY_NAME = new Map(CASES.map(c => [c.name, c]))

function readFixture(file) {
	return fs.readFileSync(path.join(FIXTURE_DIR, file), 'utf8')
}

function modeLabel(optimize) {
	return optimize ? 'true' : 'false'
}

/** Compare gl_FragColor against the hand-computed expectation. */
function compare(actual, expected) {
	if (!Array.isArray(actual)) {
		return expected.map((value, index) => ({
			index, component: COMPONENT_NAMES[index] || String(index), expected: value, actual
		}))
	}

	const mismatches = []
	for (let index = 0; index < expected.length; index++) {
		const got = actual[index]
		const ok = typeof got === 'number' && Number.isFinite(got) && Math.abs(got - expected[index]) <= EPSILON
		if (!ok) mismatches.push({
			index,
			component: COMPONENT_NAMES[index] || String(index),
			expected: expected[index],
			actual: got
		})
	}
	// A shader that writes too few components is also wrong, even if the
	// components it did write happen to match.
	if (actual.length !== expected.length) {
		mismatches.push({
			index: Math.min(actual.length, expected.length),
			component: 'length',
			expected: expected.length,
			actual: actual.length
		})
	}
	return mismatches
}

function executeOnce(js) {
	return new Function(
		'let gl_FragColor;\n' + js + '\nmain();\nreturn Array.from(gl_FragColor);'
	)()
}

/**
 * Same generated shader, called twice inside the same generated program.
 * A correct compiler must produce the same result on both calls; out-parameter
 * state and global helpers must not leak between invocations.
 */
function executeTwice(js) {
	return new Function(
		'let gl_FragColor;\n' + js +
		'\nmain();\nconst $$first = Array.from(gl_FragColor);\nmain();\nconst $$second = Array.from(gl_FragColor);\nreturn { first: $$first, second: $$second };'
	)()
}

function runCase(testCase, optimize) {
	const record = {
		kind: 'case',
		name: testCase.name,
		optimize,
		control: !!testCase.control,
		expected: testCase.expected,
		compiled: false,
		executed: false,
		stable: false,
		actual: null,
		actualSecond: null,
		mismatches: [],
		repeatMismatches: [],
		error: null
	}

	let js
	try {
		js = GLSL({ optimize })(readFixture(testCase.file))
		record.compiled = true
	} catch (error) {
		record.error = { stage: 'compile', type: error.name, message: error.message }
		return record
	}

	try {
		const result = executeTwice(js)
		record.executed = true
		record.actual = result.first
		record.actualSecond = result.second
		record.mismatches = compare(result.first, testCase.expected)
		record.repeatMismatches = compare(result.second, testCase.expected)
		record.stable = JSON.stringify(result.first) === JSON.stringify(result.second)
	} catch (error) {
		record.error = { stage: 'execute', type: error.name, message: error.message }
	}
	return record
}

/**
 * Two different shaders, compiled by two compiler instances and executed in
 * interleaved order.  Results must not depend on a previous compilation or
 * execution leaking through compiler state.
 */
function runInterleaved(optimize) {
	const a = CASE_BY_NAME.get('in_vec_unchanged')
	const b = CASE_BY_NAME.get('out_normal_end')
	const record = {
		kind: 'interleave',
		name: `${a.name}+${b.name}`,
		optimize,
		control: true,
		expected: null,
		compiled: false,
		executed: false,
		stable: false,
		actual: null,
		actualSecond: null,
		mismatches: [],
		repeatMismatches: [],
		error: null
	}
	try {
		const compilerA = GLSL({ optimize })
		const compilerB = GLSL({ optimize })
		const jsA = compilerA(readFixture(a.file))
		const jsB = compilerB(readFixture(b.file))
		record.compiled = true
		record.executed = true
		const a1 = executeOnce(jsA)
		const b1 = executeOnce(jsB)
		const a2 = executeOnce(jsA)
		record.actual = a1
		record.actualSecond = a2
		const mismatchA = compare(a1, a.expected)
		const mismatchB = compare(b1, b.expected)
		const mismatchA2 = compare(a2, a.expected)
		record.mismatches = [...mismatchA, ...mismatchB.map(m => ({ ...m, shader: b.name }))]
		record.repeatMismatches = mismatchA2
		record.stable = JSON.stringify(a1) === JSON.stringify(a2)
	} catch (error) {
		record.error = { stage: 'interleave', type: error.name, message: error.message }
	}
	return record
}

function formatRecordFailure(record) {
	const mode = modeLabel(record.optimize)
	if (record.error) {
		return `[error] case=${record.name} optimize=${mode} stage=${record.error.stage} type=${record.error.type} message=${record.error.message}`
	}
	const lines = []
	const mismatches = [...record.mismatches, ...record.repeatMismatches.map(m => ({ ...m, repeat: true }))]
	for (const m of mismatches) {
		if (m.shader) {
			lines.push(`[fail] case=${record.name} shader=${record.shader} optimize=${mode} component=${m.component}(${m.index}) expected=${m.expected} actual=${String(m.actual)} gl_FragColor=${JSON.stringify(record.actual)}`)
		} else {
			const suffix = m.repeat ? ' repeat-call=true' : ''
			lines.push(`[fail] case=${record.name} optimize=${mode} component=${m.component}(${m.index})${suffix} expected=${m.expected} actual=${String(m.actual)} gl_FragColor=${JSON.stringify(record.actual)}`)
		}
	}
	if (!mismatches.length && !record.stable) {
		lines.push(`[fail] case=${record.name} optimize=${mode} repeated-call result differs first=${JSON.stringify(record.actual)} second=${JSON.stringify(record.actualSecond)}`)
	}
	return lines.join('\n')
}

function passed(record) {
	return !record.error && record.stable && record.mismatches.length === 0 && record.repeatMismatches.length === 0
}

function printRecords(records) {
	for (const record of records) {
		if (passed(record)) {
			console.log(`[pass] case=${record.name} optimize=${modeLabel(record.optimize)} gl_FragColor=${JSON.stringify(record.actual)}`)
		} else {
			console.log(formatRecordFailure(record))
		}
	}
}

function writeResults(payload) {
	const target = process.env.PARAMETER_COPY_RESULTS
	if (!target) return
	fs.mkdirSync(path.dirname(path.resolve(target)), { recursive: true })
	fs.writeFileSync(path.resolve(target), JSON.stringify(payload, null, 2) + '\n')
}

function summarize(records) {
	return {
		records: records.length,
		compiled: records.filter(r => r.compiled).length,
		executed: records.filter(r => r.executed).length,
		passed: records.filter(passed).length,
		failed: records.filter(r => !passed(r)).length,
		controls: records.filter(r => r.control).length,
		controlsPassed: records.filter(r => r.control && passed(r)).length,
		errors: records.filter(r => r.error).length
	}
}

const captureMode = process.env.PARAMETER_COPY_CAPTURE === '1'
const expectedHits = (process.env.PARAMETER_COPY_EXPECT_HITS || '').split(',').map(s => s.trim()).filter(Boolean)
const patchLabel = process.env.PARAMETER_COPY_PATCH_LABEL || 'unnamed-patch'

const records = []
for (const testCase of CASES) {
	for (const optimize of OPTIMIZE_MODES) records.push(runCase(testCase, optimize))
}
const interleaved = OPTIMIZE_MODES.map(runInterleaved)
records.push(...interleaved)

printRecords(records)

const summary = summarize(records)
const caseCount = CASES.length
const controlCount = CASES.filter(c => c.control).length
const executionCount = records.filter(r => r.executed).length

const payload = {
	generatedAt: new Date().toISOString(),
	mode: captureMode ? 'capture' : 'normal',
	patch: captureMode ? patchLabel : null,
	expectedHits: captureMode ? expectedHits : null,
	optimizeModes: OPTIMIZE_MODES,
	caseCount,
	controls: controlCount,
	summary,
	records
}

if (captureMode) {
	const invalid = reason => {
		writeResults({ ...payload, capture: { ok: false, reason } })
		console.log(`PARAMETER_COPY_CAPTURE_INVALID patch=${patchLabel} reason=${reason}`)
		process.exit(3)
	}

	if (!records.length || summary.executed === 0) invalid('zero_executions')
	if (summary.errors > 0) {
		const stages = new Set(records.filter(r => r.error).map(r => r.error.stage))
		invalid([...stages].some(stage => stage === 'compile') ? 'compile_error' : 'execution_error')
	}
	if (summary.compiled !== records.length) invalid('not_all_compiled')
	if (summary.executed !== records.length) invalid('not_all_executed')
	const controls = records.filter(r => r.control)
	if (!controls.length) invalid('no_controls')
	if (!controls.every(passed)) invalid('control_failure')
	if (!interleaved.every(passed)) invalid('interleave_failure')
	const failing = records.filter(r => !passed(r))
	if (!failing.length) invalid('no_numeric_mismatch')

	// Every expected hit must exist and fail, with a numeric mismatch rather
	// than a compile/execution error, in both optimize modes.
	for (const name of expectedHits) {
		const hits = records.filter(r => r.name === name)
		if (!hits.length) invalid(`missing_hit_fixture:${name}`)
		if (!OPTIMIZE_MODES.every(mode => hits.some(r => r.optimize === mode && !passed(r) && r.mismatches.length > 0 && !r.error))) {
			invalid(`expected_hit_not_numeric:${name}`)
		}
	}

	const hitSummary = expectedHits.map(name => {
		const hit = records.find(r => r.name === name && !passed(r))
		const m = hit.mismatches[0]
		return `${name}[component=${m?.component},actual=${m?.actual},expected=${m?.expected}]`
	})
	writeResults({ ...payload, capture: { ok: true, hits: hitSummary } })
	console.log(`PARAMETER_COPY_CAPTURE_OK patch=${patchLabel} cases=${caseCount} compiled=${summary.compiled}/${records.length} executed=${summary.executed}/${records.length} controls=${summary.controlsPassed}/${summary.controls} numeric_mismatches=${summary.failed} hits=${hitSummary.join(';')}`)
	process.exit(0)
}

writeResults({ ...payload, capture: null })

// Normal (unmodified source) mode: all assertions must hold.
assert.ok(caseCount >= 12, `expected at least 12 named cases, got ${caseCount}`)
assert.ok(executionCount > 0, 'no shader was executed')
assert.ok(records.every(passed), 'at least one parameter-copy case failed (see [fail] lines above)')

console.log(`PARAMETER_COPY_TEST_OK cases=${caseCount} executions=${executionCount} modes=${OPTIMIZE_MODES.length} controls=${controlCount} optimized-and-unoptimized=equal`)
