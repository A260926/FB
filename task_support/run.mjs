import {fork,spawnSync} from 'node:child_process';
import {statSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const self=fileURLToPath(import.meta.url), cwd=fileURLToPath(new URL('../',import.meta.url));
const target=new URL('../tests/skin-motion-edit.test.mjs',import.meta.url);
const groups=['surface','optimization','temporal','playback'];
if(process.argv[2]==='worker'){
    const name=process.argv[3];let checks=0;
    try{
        assert(statSync(target).size>0,'empty target');
        const {cases}=await import(target.href);
        assert(cases&&typeof cases[name]==='function',`missing group ${name}`);
        await cases[name]((condition,message)=>{checks++;assert.equal(typeof condition,'boolean','check needs boolean');assert(condition,message);});
        assert(checks>0,`zero checks ${name}`);
        process.send({completed:name,checks});process.disconnect();
    }catch(e){console.error(e);process.exitCode=1;if(process.connected)process.disconnect();}
}else{
    const mode=process.argv[2]??'baseline';assert(['baseline','target'].includes(mode),'unknown mode');
    if(mode==='target'){
        const production=new URL('../src/extras/SkinMotionEdit.js',import.meta.url);
        assert(statSync(production).size>0,'missing or empty production');
        const api=await import(new URL('../src/index.js',import.meta.url));
        assert.equal(typeof api.GLTFAnimation?.prototype.editSkinMotion,'function','missing public editSkinMotion method');
        assert.equal(typeof api.GLTFSkin?.prototype.sampleSurface,'function','missing public sampleSurface method');
        await import(production.href);
        assert(statSync(target).size>0,'missing or empty target');
        for(const group of groups){
            await new Promise((resolve,reject)=>{
                let report=null;const child=fork(self,['worker',group],{cwd,stdio:['ignore','inherit','inherit','ipc']});
                const timer=setTimeout(()=>{child.kill('SIGKILL');reject(Error(`timeout ${group}`));},120000);
                child.on('message',m=>{report=m;});child.on('error',reject);
                child.on('exit',(code,signal)=>{clearTimeout(timer);if(code===0&&!signal&&report?.completed===group&&report.checks>0){console.log(`GROUP ${group} checks=${report.checks} COMPLETE`);resolve();}else reject(Error(`incomplete group ${group}: ${code}/${signal}`));});
            });
        }
    }
    const run=spawnSync(process.execPath,['task_support/baseline.mjs'],{cwd,stdio:'inherit'});
    assert.equal(run.status,0,'production baseline failed');assert.equal(run.signal,null);
    console.log(mode==='target'?'TARGET_COMPLETE groups=4':'BASELINE_COMPLETE');
}
