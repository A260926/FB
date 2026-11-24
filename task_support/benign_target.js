"use strict";
let phase=0;
exports.fuzz=async function fuzz(buf){
 const command=buf.toString('utf8');
 if(command==='begin'){phase=1;return;}
 if(command==='advance'){if(phase===1)phase=2;return;}
 if(command==='finish'){if(phase===2)phase=3;return;}
 return;
};
