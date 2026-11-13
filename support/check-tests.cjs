const fs=require('node:fs');
const [file,prefix]=process.argv.slice(2);let roots=new Map();let skipped=false;
for(const l of fs.readFileSync(file,'utf8').split('\n')){if(!l.trim())continue;const x=JSON.parse(l);if(x.Output)process.stdout.write(x.Output);if(x.Test&&x.Test.startsWith(prefix)&&x.Action==='skip')skipped=true;if(x.Test&&x.Test.startsWith(prefix)&&!x.Test.includes('/')){if(x.Action==='run')roots.set(x.Test,'run');if(['pass','fail','skip'].includes(x.Action))roots.set(x.Test,x.Action);}}
if(skipped||!roots.size||[...roots.values()].some(x=>x!=='pass')){console.error('Target must execute non-skipped passing tests:',[...roots]);process.exit(1)}
console.log('verified executed tests:',roots.size);
