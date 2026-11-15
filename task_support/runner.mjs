import assert from 'node:assert/strict';
import {execute} from './check.mjs';
const api = await import('../src/ossos.ts');
const track = await import('../src/animation/tracks/CurveChainTrack.ts');
const solver = await import('../src/ikrig/solvers/CurveChainSolver.ts');
for (const [name, m] of [['CurveChainTrack',track],['CurveChainSolver',solver]]) {
 assert.equal(typeof api[name],'function',`public export ${name}`);
 assert.ok(api[name] === m[name] || api[name] === m.default,`production binding ${name}`);
}
await import('../test/curve-chain.ts');
await execute(['roots','sampling','skinning']);
