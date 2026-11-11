const assert = require('node:assert/strict');
const {test} = require('node:test');
const mqtt = require('../');
// Independent wire bytes: v5 PUBLISH, topic a / alias 1 / payload 1, then alias-only payload 2.
const wire = Buffer.from('30080001610323000131300700000323000132', 'hex');
function read(chunks, version=5) {
  const packets=[]; const p=mqtt.parser({protocolVersion:version});
  p.on('packet', x => packets.push(x)); p.on('error', e => {throw e;});
  for (const chunk of chunks) p.parse(chunk);
  return packets.map(x => ({topic:x.topic, payload:x.payload.toString(), alias:x.properties?.topicAlias}));
}
test('collector baseline: raw parser preserves alias-only topic, all split boundaries agree', () => {
  const expected=[{topic:'a',payload:'1',alias:1},{topic:'',payload:'2',alias:1}];
  assert.deepEqual(read([wire]), expected);
  for(let i=0;i<=wire.length;i++) assert.deepEqual(read([wire.subarray(0,i),wire.subarray(i)]), expected);
  assert.deepEqual(read([...wire].map(b=>Buffer.from([b]))),expected);
});
test('collector baseline: v4 has no properties-length byte', () => {
  assert.deepEqual(read([Buffer.from('300400016178','hex')],4),[{topic:'a',payload:'x',alias:undefined}]);
});
test('collector baseline: public generator and independent packet fixture agree', () => {
  const encoded=mqtt.generate({cmd:'publish',qos:0,topic:'a',payload:'1',properties:{topicAlias:1}},{protocolVersion:5});
  assert.deepEqual(encoded, wire.subarray(0,10));
});
