import {createRequire} from 'node:module';
import {mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {spawnSync} from 'node:child_process';
const require = createRequire(new URL('./deps/package.json', import.meta.url));
const {build} = require('esbuild');
const [entry, marker] = process.argv.slice(2);
if (!entry || !marker) throw Error('entry and success marker required');
const dir = await mkdtemp(join(tmpdir(), 'effect-check-'));
try {
 const outfile = join(dir, 'check.mjs');
 await build({entryPoints:[resolve(entry)], outfile, bundle:true, platform:'node', format:'esm', nodePaths:[resolve('checks/deps/node_modules')], loader:{'.frag':'text','.vert':'text'}});
 const result = spawnSync(process.execPath,[outfile],{encoding:'utf8'});
 process.stdout.write(result.stdout ?? ''); process.stderr.write(result.stderr ?? '');
 if(result.status !== 0 || !result.stdout.includes(marker)) process.exitCode = 1;
} finally {await rm(dir,{recursive:true,force:true});}
