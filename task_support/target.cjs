const {spawnSync}=require('node:child_process');const fs=require('node:fs');
const file='test/ordered_target.mjs';if(!fs.existsSync(file)||!fs.readFileSync(file,'utf8').trim())throw Error('missing target tests');
const r=spawnSync(process.execPath,['--no-warnings','--loader','./task_support/loader.mjs','--test','--test-reporter=tap',file],{encoding:'utf8',maxBuffer:16*1024*1024});
process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');if(r.error||r.status!==0)process.exit(r.status||1);
for(const name of ['ordered dispatch','ordered failure','ordered lifecycle'])if(!(r.stdout||'').split('\n').some(x=>new RegExp('^ok [0-9]+ - '+name+'$').test(x)))throw Error('missing passing test '+name);
if(!/^# pass [1-9][0-9]*$/m.test(r.stdout)||/^# (skipped|todo) [1-9]/m.test(r.stdout)||/# SKIP|# TODO/.test(r.stdout))throw Error('zero or skipped tests');
