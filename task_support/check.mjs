const groups=new Map();
let active=null;
let broken=false;
export function group(name,fn){
 if(groups.has(name)||typeof fn!=='function'){broken=true;throw Error('invalid/duplicate group');}
 groups.set(name,fn);
}
export function check(value,message='content check'){
 if(!active){broken=true;throw Error('assertion outside group');}
 active.count++;
 if(!value){active.failed=true;broken=true;throw Error(message);}
}
export async function execute(expected){
 if(broken||groups.size!==expected.length||expected.some(n=>!groups.has(n)))throw Error('missing/unexpected registered group');
 for(const name of expected){
  active={count:0,failed:false};
  await groups.get(name)();
  if(broken||active.failed||active.count<1)throw Error('group failed or contained no content checks: '+name);
  console.log('TASK_GROUP_OK '+name+' checks='+active.count);
 }
 active=null;
 console.log('TASK_TARGET_COMPLETE');
}
