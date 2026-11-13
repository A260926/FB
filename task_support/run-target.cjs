const fs=require('node:fs');const {spawnSync}=require('node:child_process');
const f='test/request-identity.cjs';if(!fs.existsSync(f))throw new Error('missing '+f);
const r=spawnSync(process.execPath,['--test',f],{encoding:'utf8'});process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');
if(r.status!==0)process.exit(r.status||1);
for(const name of ['Request identity','Redirect method','Persisted bodies'])
 if(!new RegExp('^ok [0-9]+ - '+name+'$','m').test(r.stdout))throw new Error('missing passing test '+name);
const b=spawnSync(process.execPath,['task_support/baseline.cjs'],{stdio:'inherit'});process.exit(b.status||0);
