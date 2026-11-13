import {execFileSync} from 'node:child_process';
import {resolve} from 'node:path';
const rubyImage='ruby@sha256:2aa77cb4221aa5ab991b85ff1cd202d6ef74f2d1a578b6f8e7783a391b45dd68';
const chromeImage='chromedp/headless-shell@sha256:d0c5b37a80f6180f28b2824d8844bf0c10bdf54ebaaf1f97514b66e5a20bef4f';
const root=resolve('.'), prefix=`check-${process.pid}-${Date.now()}`;
const network=prefix+'-net', browser=prefix+'-browser', ruby=prefix+'-ruby';
const docker=args=>execFileSync('docker',args,{encoding:'utf8',timeout:60000}).trim();
try {
 docker(['network','create','--internal',network]);
 if(docker(['network','inspect','-f','{{.Internal}}',network])!=='true')throw Error('network must be internal');
 console.log('CONTAINER_NETWORK_INTERNAL=true');
 docker(['run','-d','--rm','--pull=never','--network',network,'--name',browser,'-v',`${root}:${root}`,chromeImage]);
 docker(['run','-d','--rm','--pull=never','--network',network,'--name',ruby,'-v',`${root}:${root}`,'-w',root,
 '-e',`GEM_HOME=${root}/.gems`,'-e',`GEM_PATH=${root}/.gems`,rubyImage,'sleep','infinity']);
 const browserAddress=docker(['inspect','-f','{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}',browser]);
 let info, lastError;
 for(let i=0;i<60;i++) {
  try {info=JSON.parse(docker(['exec',ruby,'ruby','-rnet/http','-e',`print Net::HTTP.get(URI('http://${browserAddress}:9222/json/version'))`]));break;}
  catch (error) {lastError=error;await new Promise(r=>setTimeout(r,250));}
 }
 if(!info)throw Error('Chrome discovery failed: '+String(lastError?.stderr||lastError));
 console.log(`BROWSER_VERSION=${info.Browser}`);
 const endpoint=new URL(info.webSocketDebuggerUrl);endpoint.hostname=browserAddress;endpoint.port='9222';
 const script=process.argv.includes('--contract')?'support/chooser-contract.rb':'support/baseline.rb';
 const output=docker(['exec','-e',`FERRUM_WS_URL=${endpoint}`,'-e',`FERRUM_FIXTURE_URL=http://${ruby}:8080/`,
 ruby,'ruby','-Ilib','support/offline-fixture.rb',script]);
 console.log(output);
} finally {
 for(const name of [ruby,browser]){try{docker(['rm','-f',name]);}catch{}}
 try{docker(['network','rm',network]);}catch{}
}
