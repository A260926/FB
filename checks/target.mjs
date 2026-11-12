import {spawnSync} from 'node:child_process';
const target='checks/float_contract.test.mjs';
const run=spawnSync(process.execPath,['--test','--test-reporter=tap',target],{encoding:'utf8'});
const out=(run.stdout??'')+(run.stderr??'');process.stdout.write(out);
if(run.error||run.status!==0)process.exit(run.status||1);
const count=/^# tests (\d+)$/m.exec(out);
if(!count||Number(count[1])<3||/^# skipped [1-9]/m.test(out)||/# SKIP|# TODO/.test(out))throw Error('EMPTY_OR_SKIPPED_CONTRACT');
for(const name of ['float_chunky','float_planar','float_reuse']) {
 if(!new RegExp('^ok \\d+ - '+name+'$', 'm').test(out))throw Error('MISSING_CONTRACT '+name);
}
console.log('UTIF_FLOAT_CONTRACT_EXECUTED');
