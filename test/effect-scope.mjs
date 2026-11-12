import assert from "node:assert/strict";
import { Uniform } from "three";
import { Effect } from "../src/effects/Effect.js";
import { EffectPass } from "../src/passes/EffectPass.js";

/**
 * Extracts the renamed identifier captured by the given declaration pattern.
 *
 * @param {String} shader - The shader to search.
 * @param {RegExp} pattern - A pattern with one capture group for the identifier.
 * @return {String} The captured identifier.
 */

function declaredName(shader, pattern) {

	const match = pattern.exec(shader);
	assert.ok(match !== null, `declaration not found: ${pattern}`);
	return match[1];

}

// Group 1: Named structures and constants with colliding names stay effect-private.

const fragment1A = /* glsl */`
struct Sample {
	vec3 color;
	float weight;
};

const float cutoff = 0.25;
const float scaled = cutoff * 2.0;

Sample makeSample(const in vec3 c) {
	Sample s = Sample(c * cutoff, scaled);
	s.weight = 1.0;
	return s;
}

void mainImage(const in vec4 inputColor, const in vec2 uv, out vec4 outputColor) {
	Sample s = makeSample(inputColor.rgb);
	outputColor = vec4(s.color * s.weight, inputColor.a);
}
`;

const fragment1B = /* glsl */`
struct Sample {
	float alpha;
};

const float cutoff = 0.75;

float applyCutoff(const in float x) {
	return x > cutoff ? x : 0.0;
}

void mainImage(const in vec4 inputColor, const in vec2 uv, out vec4 outputColor) {
	Sample s;
	s.alpha = applyCutoff(inputColor.r);
	outputColor = vec4(vec3(s.alpha), inputColor.a);
}
`;

const effect1A = new Effect("a", fragment1A);
const effect1B = new Effect("b", fragment1B);
const pass1 = new EffectPass(null, effect1A, effect1B);
pass1.recompile();
const fragment1 = pass1.fullscreenMaterial.fragmentShader;

// Both structures survive under their own scoped name and keep their own members.
assert.match(fragment1, /struct e0Sample \{\s*vec3 color;\s*float weight;\s*\}/);
assert.match(fragment1, /struct e1Sample \{\s*float alpha;\s*\}/);
assert.doesNotMatch(fragment1, /\bstruct Sample\b/);

// The constants keep their own values and their references follow the renamed declarations.
const cutoff1A = declaredName(fragment1, /const float (e\d+Cutoff) = 0\.25;/);
const cutoff1B = declaredName(fragment1, /const float (e\d+Cutoff) = 0\.75;/);
assert.notEqual(cutoff1A, cutoff1B);
assert.doesNotMatch(fragment1, /\bconst float cutoff\b/);
assert.ok(fragment1.includes(`const float e0Scaled = ${cutoff1A} * 2.0;`));
assert.ok(fragment1.includes(`return x > ${cutoff1B} ? x : 0.0;`));

// Type usages, constructor calls and initializers point at the same effect's declarations.
const sample1A = declaredName(fragment1, /struct (e\d+Sample) \{\s*vec3 color;/);
const makeSample1A = declaredName(fragment1, /\be\d+Sample (e\d+MakeSample)\(const in vec3 c\)/);
assert.ok(fragment1.includes(`${sample1A} ${makeSample1A}(const in vec3 c)`));
assert.ok(fragment1.includes(`${sample1A} s = ${sample1A}(c * ${cutoff1A}, e0Scaled);`));
assert.ok(fragment1.includes(`${sample1A} s = ${makeSample1A}(inputColor.rgb);`));

// The second effect uses its own structure type and function everywhere.
const applyCutoff1B = declaredName(fragment1, /float (e\d+ApplyCutoff)\(const in float x\)/);
assert.ok(fragment1.includes("e1Sample s;"));
assert.ok(fragment1.includes(`s.alpha = ${applyCutoff1B}(inputColor.r);`));
assert.ok(!fragment1.includes(`${sample1A}(c * ${cutoff1B}`));

pass1.dispose();

// Group 2: Function overloads and forward declarations share the effect's renamed name.

const fragment2A = /* glsl */`
float weight(const in float x);

const float factor = 2.0;

float weight(const in float x) {
	return x * factor;
}

float weight(const in vec2 v) {
	return weight(v.x) + weight(v.y);
}

void mainImage(const in vec4 inputColor, const in vec2 uv, out vec4 outputColor) {
	outputColor = vec4(vec3(weight(inputColor.r)), inputColor.a);
}
`;

const fragment2B = /* glsl */`
float weight(const in float x) {
	return x * 0.5;
}

void mainImage(const in vec4 inputColor, const in vec2 uv, out vec4 outputColor) {
	outputColor = vec4(vec3(weight(inputColor.g)), inputColor.a);
}
`;

const effect2A = new Effect("a", fragment2A);
const effect2B = new Effect("b", fragment2B);
const pass2 = new EffectPass(null, effect2A, effect2B);
pass2.recompile();
const fragment2 = pass2.fullscreenMaterial.fragmentShader;

// The forward declaration and the definition are renamed consistently.
assert.match(fragment2, /float e0Weight\(const in float x\);/);
assert.match(fragment2, /float e0Weight\(const in float x\) \{/);

// The overload shares the same renamed name.
assert.match(fragment2, /float e0Weight\(const in vec2 v\) \{/);

// Calls inside the overload reference the renamed function; swizzles stay untouched.
assert.ok(fragment2.includes("return e0Weight(v.x) + e0Weight(v.y);"));

// The constant is renamed and its reference follows the declaration.
const factor2A = declaredName(fragment2, /const float (e\d+Factor) = 2\.0;/);
assert.ok(fragment2.includes(`return x * ${factor2A};`));

// The same-named function of the other effect gets its own rename.
assert.match(fragment2, /float e1Weight\(const in float x\) \{/);
assert.ok(fragment2.includes("vec3(e0Weight(inputColor.r))"));
assert.ok(fragment2.includes("vec3(e1Weight(inputColor.g))"));
assert.doesNotMatch(fragment2, /\bweight\s*\(/);

pass2.dispose();

// Group 3: Local variables, parameters and loop variables shadow global names.

const fragment3 = /* glsl */`
const float cutoff = 0.5;

float globalCutoff() {
	return cutoff;
}

float apply(const in float cutoff) {
	return cutoff * 2.0;
}

void mainImage(const in vec4 inputColor, const in vec2 uv, out vec4 outputColor) {
	float sum = globalCutoff();
	{
		float cutoff = 1.0;
		sum += cutoff;
		{
			float cutoff = 2.0;
			sum += cutoff;
		}
		sum += cutoff;
	}
	for(float cutoff = 0.0; cutoff < 4.0; cutoff += 1.0) {
		sum += cutoff;
	}
	sum += apply(sum);
	outputColor = vec4(vec3(sum), inputColor.a);
}
`;

const effect3 = new Effect("shadow", fragment3);
const pass3 = new EffectPass(null, effect3);
pass3.recompile();
const fragment3Out = pass3.fullscreenMaterial.fragmentShader;

// The global constant is renamed and the unshadowed reference follows the declaration.
const cutoff3 = declaredName(fragment3Out, /const float (e\d+Cutoff) = 0\.5;/);
assert.ok(fragment3Out.includes(`float e0GlobalCutoff() {\n\treturn ${cutoff3};\n}`));

// The parameter shadows the global: its declaration and references keep the local meaning.
assert.ok(fragment3Out.includes("float e0Apply(const in float cutoff) {"));
assert.ok(fragment3Out.includes("return cutoff * 2.0;"));

// Local variables in nested blocks shadow the global.
assert.ok(fragment3Out.includes("float cutoff = 1.0;"));
assert.ok(fragment3Out.includes("float cutoff = 2.0;"));

// The for-loop variable shadows the global in the init, condition, increment and body.
assert.ok(fragment3Out.includes("for(float cutoff = 0.0; cutoff < 4.0; cutoff += 1.0)"));

// No renamed reference leaks into the shadowed regions of mainImage.
const mainImage3 = fragment3Out.slice(fragment3Out.indexOf("void e0MainImage"));
assert.ok(!mainImage3.includes(cutoff3));

// Global function calls are renamed.
assert.ok(fragment3Out.includes("float sum = e0GlobalCutoff();"));
assert.ok(fragment3Out.includes("sum += e0Apply(sum);"));

pass3.dispose();

// Group 4: Structure member names are not treated as global references.

const fragment4 = /* glsl */`
struct Sample {
	float cutoff;
	float gain;
};

const float cutoff = 0.5;
const float gain = 0.25;

float readWeight(const in Sample s) {
	return s.cutoff + s.gain;
}

void mainImage(const in vec4 inputColor, const in vec2 uv, out vec4 outputColor) {
	Sample s;
	s.cutoff = cutoff;
	s.gain = gain;
	outputColor = vec4(vec3(readWeight(s) * inputColor.r), inputColor.a);
}
`;

const effect4 = new Effect("members", fragment4);
const pass4 = new EffectPass(null, effect4);
pass4.recompile();
const fragment4Out = pass4.fullscreenMaterial.fragmentShader;

// Member declarations keep their names although globals with the same names exist.
assert.match(fragment4Out, /struct e0Sample \{\s*float cutoff;\s*float gain;\s*\}/);

// Member selectors are not renamed.
assert.ok(fragment4Out.includes("return s.cutoff + s.gain;"));

// The globals are renamed and references to them follow their declarations.
const cutoff4 = declaredName(fragment4Out, /const float (e\d+Cutoff) = 0\.5;/);
const gain4 = declaredName(fragment4Out, /const float (e\d+Gain) = 0\.25;/);
assert.ok(fragment4Out.includes(`s.cutoff = ${cutoff4};`));
assert.ok(fragment4Out.includes(`s.gain = ${gain4};`));
assert.doesNotMatch(fragment4Out, /\.e\d+Cutoff/);
assert.doesNotMatch(fragment4Out, /\.e\d+Gain/);

// Structure-typed parameters and call sites are renamed consistently.
assert.ok(fragment4Out.includes("float e0ReadWeight(const in e0Sample s)"));
assert.ok(fragment4Out.includes("vec3(e0ReadWeight(s) * inputColor.r)"));

pass4.dispose();

// Group 5: Interface names stay connected across the vertex and fragment stages.

const vertex5A = /* glsl */`
uniform float scale;
varying float vMask;

void mainSupport(const in vec2 uv) {
	vMask = uv.x * scale;
}
`;

const fragment5A = /* glsl */`
varying float vMask;

float adjust(const in float x) {
	return x * 2.0;
}

void mainImage(const in vec4 inputColor, const in vec2 uv, out vec4 outputColor) {
	outputColor = vec4(inputColor.rgb * adjust(vMask), 1.0);
}
`;

const vertex5B = /* glsl */`
varying float vMask;

void mainSupport() {
	vMask = 0.5;
}
`;

const fragment5B = /* glsl */`
varying float vMask;

void mainUv(inout vec2 uv) {
	uv += vMask;
}

void mainImage(const in vec4 inputColor, const in vec2 uv, out vec4 outputColor) {
	outputColor = vec4(inputColor.rgb * vMask, 1.0);
}
`;

const effect5A = new Effect("a", fragment5A, {
	vertexShader: vertex5A,
	uniforms: new Map([["scale", new Uniform(2.0)]])
});
const effect5B = new Effect("b", fragment5B, { vertexShader: vertex5B });
const pass5 = new EffectPass(null, effect5A, effect5B);
pass5.recompile();
const material5 = pass5.fullscreenMaterial;

// Each varying is renamed identically in both stages: the interfaces stay connected.
const vertexMasks = [...material5.vertexShader.matchAll(/varying float (e\d+VMask);/g)].map((m) => m[1]);
const fragmentMasks = [...material5.fragmentShader.matchAll(/varying float (e\d+VMask);/g)].map((m) => m[1]);
assert.equal(vertexMasks.length, 2);
assert.notEqual(vertexMasks[0], vertexMasks[1]);
assert.deepEqual([...vertexMasks].sort(), [...fragmentMasks].sort());

// The varying of the first effect is written in the vertex shader and read in the fragment shader.
const vMask5A = declaredName(material5.vertexShader, /varying float (e0VMask);/);
assert.ok(material5.vertexShader.includes(`${vMask5A} = uv.x * e0Scale;`));
const adjust5A = declaredName(material5.fragmentShader, /float (e\d+Adjust)\(const in float x\)/);
assert.ok(material5.fragmentShader.includes(`${adjust5A}(${vMask5A})`));

// The second effect reads its own varying in the fragment shader.
const vMask5B = declaredName(material5.fragmentShader, /varying float (e1VMask);/);
assert.ok(material5.fragmentShader.includes(`inputColor.rgb * ${vMask5B}`));

// mainSupport is renamed and invoked under the same name, with and without the uv parameter.
assert.ok(material5.vertexShader.includes("void e0MainSupport(const in vec2 uv)"));
assert.ok(material5.vertexShader.includes("e0MainSupport(vUv);"));
assert.ok(material5.vertexShader.includes("void e1MainSupport()"));
assert.ok(material5.vertexShader.includes("e1MainSupport();"));

// The uniform is renamed in the vertex shader and exposed under the same name.
assert.ok(material5.vertexShader.includes("uniform float e0Scale;"));
assert.doesNotMatch(material5.vertexShader, /\buniform float scale;/);
assert.equal(material5.uniforms.e0Scale, effect5A.uniforms.get("scale"));
assert.equal(material5.uniforms.e0Scale.value, 2.0);

// mainUv is renamed, invoked under the same name and keeps its varying reference.
assert.ok(material5.fragmentShader.includes("void e1MainUv(inout vec2 uv)"));
assert.ok(material5.fragmentShader.includes("uv += e1VMask;"));
assert.ok(material5.fragmentShader.includes("e1MainUv(UV);"));
assert.equal(material5.defines.UV, "transformedUv");

pass5.dispose();

// Group 6: Macro parameters, macro values, recompilation and composition order.

const fragment6A = /* glsl */`
const float limit = 0.8;
uniform float gain;

void mainImage(const in vec4 inputColor, const in vec2 uv, out vec4 outputColor) {
	float x = CLAMP_GAIN(inputColor.r);
	outputColor = vec4(vec3(x * LIMIT_SCALE), inputColor.a);
}
`;

const fragment6B = /* glsl */`
const float limit = 0.3;

void mainImage(const in vec4 inputColor, const in vec2 uv, out vec4 outputColor) {
	outputColor = vec4(vec3(limit * LIMIT_SCALE), inputColor.a);
}
`;

const effect6A = new Effect("a", fragment6A, {
	defines: new Map([
		["LIMIT_SCALE", "limit * gain"],
		["CLAMP_GAIN(limit)", "min((limit) * gain, 0.5)"]
	]),
	uniforms: new Map([["gain", new Uniform(1.5)]])
});
const effect6B = new Effect("b", fragment6B, {
	defines: new Map([["LIMIT_SCALE", "limit * 2.0"]])
});

const pass6 = new EffectPass(null, effect6A, effect6B);
pass6.recompile();
const material6 = pass6.fullscreenMaterial;

// Object-like macros are renamed and references in their replacement text follow the effect.
assert.equal(material6.defines.e0LIMIT_SCALE, "e0Limit * e0Gain");
assert.equal(material6.defines.e1LIMIT_SCALE, "e1Limit * 2.0");

// A function-like macro parameter shadows the global only inside the replacement text.
assert.equal(material6.defines["e0CLAMP_GAIN(limit)"], "min((limit) * e0Gain, 0.5)");

// Macro references in the shader source are renamed as well.
assert.ok(material6.fragmentShader.includes("float x = e0CLAMP_GAIN(inputColor.r);"));
assert.ok(material6.fragmentShader.includes("vec3(x * e0LIMIT_SCALE)"));
assert.ok(material6.fragmentShader.includes("vec3(e1Limit * e1LIMIT_SCALE)"));

// The uniform object keeps its identity.
assert.equal(material6.uniforms.e0Gain, effect6A.uniforms.get("gain"));
assert.equal(material6.uniforms.e0Gain.value, 1.5);

// Recompilation is idempotent: no double prefixing and no symbol table leakage.
const fragment6Before = material6.fragmentShader;
pass6.recompile();
assert.equal(material6.fragmentShader, fragment6Before);
assert.equal(material6.defines.e0LIMIT_SCALE, "e0Limit * e0Gain");
assert.equal(material6.defines["e0CLAMP_GAIN(limit)"], "min((limit) * e0Gain, 0.5)");
assert.ok(!material6.fragmentShader.includes("e0E0"));

// Swapping the composition order swaps the prefixes consistently.
const pass6B = new EffectPass(null, effect6B, effect6A);
pass6B.recompile();
const material6B = pass6B.fullscreenMaterial;
assert.equal(material6B.defines.e0LIMIT_SCALE, "e0Limit * 2.0");
assert.equal(material6B.defines.e1LIMIT_SCALE, "e1Limit * e1Gain");
assert.equal(material6B.defines["e1CLAMP_GAIN(limit)"], "min((limit) * e1Gain, 0.5)");
assert.equal(material6B.uniforms.e1Gain, effect6A.uniforms.get("gain"));
assert.ok(material6B.fragmentShader.includes("const float e0Limit = 0.3;"));
assert.ok(material6B.fragmentShader.includes("const float e1Limit = 0.8;"));

// The first pass is not affected by the second one.
pass6.recompile();
assert.equal(material6.fragmentShader, fragment6Before);
assert.equal(material6.defines.e0LIMIT_SCALE, "e0Limit * e0Gain");
assert.equal(material6.uniforms.e0Gain.value, 1.5);

pass6.dispose();
pass6B.dispose();

// Group 7: Built-in identifiers, template functions and comments are not effect-private.

const fragment7 = /* glsl */`
// A comment mentioning struct Sample and cutoff.
/* A block comment mentioning cutoff and e0Cutoff verbatim. */
// struct Fake { float x; };
const float cutoff = 0.5;

void mainImage(const in vec4 inputColor, const in vec2 uv, out vec4 outputColor) {
	float d = readDepth(uv);
	vec4 texel = texture2D(inputBuffer, uv);
	outputColor = vec4(texel.rgb * cutoff + d, inputColor.a);
}
`;

const effect7 = new Effect("shared", fragment7);
const pass7 = new EffectPass(null, effect7);
pass7.recompile();
const fragment7Out = pass7.fullscreenMaterial.fragmentShader;

// Built-in identifiers and functions provided by the material template keep their names.
assert.ok(fragment7Out.includes("float d = readDepth(uv);"));
assert.ok(fragment7Out.includes("vec4 texel = texture2D(inputBuffer, uv);"));
assert.doesNotMatch(fragment7Out, /\be0ReadDepth\b/);
assert.doesNotMatch(fragment7Out, /\be0Texture2D\b/);

// Identifiers in comments are not rewritten and declarations in comments are not parsed.
assert.ok(fragment7Out.includes("// A comment mentioning struct Sample and cutoff."));
assert.ok(fragment7Out.includes("/* A block comment mentioning cutoff and e0Cutoff verbatim. */"));
assert.ok(!fragment7Out.includes("e0Fake"));

// The real global is still renamed and its reference follows the declaration.
const cutoff7 = declaredName(fragment7Out, /const float (e\d+Cutoff) = 0\.5;/);
assert.ok(fragment7Out.includes(`texel.rgb * ${cutoff7} + d`));

pass7.dispose();

console.log("EFFECT_SCOPE_OK");
