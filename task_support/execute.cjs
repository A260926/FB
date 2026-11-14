"use strict";
const fs=require('node:fs');
const {pathToFileURL}=require('node:url');
const path=require('node:path');
const config=require('./target.json');
(async()=>{
 await import(pathToFileURL(path.resolve(config.target)).href);
 const report=await require('./check.cjs').execute(config.groups);
 fs.writeFileSync('.task-build/report.json',JSON.stringify(report));
})().catch(e=>{console.error(e.stack || e);process.exitCode=1;});
