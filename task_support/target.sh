#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s task_support/script-contract.test.mjs
sh task_support/build.sh
mkdir -p .build
node --test --test-reporter=tap task_support/script-contract.test.mjs > .build/script-contract.tap 2>&1 || { cat .build/script-contract.tap; exit 1; }
node --input-type=module -e 'import fs from "node:fs"; const t=fs.readFileSync(".build/script-contract.tap","utf8"); for(const k of ["fail","cancelled","skipped","todo"]) if(!new RegExp("# "+k+" 0(?:\\s|$)","m").test(t)) throw Error(k); const n=Number(t.match(/# tests (\d+)/)?.[1]); if(!(n>=8)) throw Error("need >=8 actual tests"); console.log("SCRIPT_CONTRACT_OK tests="+n);'
