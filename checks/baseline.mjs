import './historical-baseline.mjs';
import assert from 'node:assert/strict';
import { GraphNode } from '../src/scene/graph-node.js';
const log=[];
class Observed extends GraphNode {
  _onHierarchyStateChanged(value) { super._onHierarchyStateChanged(value);log.push([this.name,value]); }
}
const root=new Observed('root'), on=new Observed('on'), off=new Observed('off'), item=new Observed('item'), tip=new Observed('tip');
root._onHierarchyStateChanged(true);root.addChild(on);root.addChild(off);on.addChild(item);item.addChild(tip);
on.setLocalPosition(10,0,0);off.setLocalPosition(-4,0,0);item.setLocalPosition(1,0,0);tip.setLocalPosition(0,2,0);off.enabled=false;
root.syncHierarchy();assert.deepEqual(tip.getPosition().toArray(),[11,2,0]);
log.length=0;const events=[];
item.on('remove',parent=>events.push('remove:'+parent.name));item.on('insert',parent=>events.push('insert:'+parent.name));
item.reparent(off);
assert.equal(item.enabled,false);assert.equal(tip.enabled,false);assert.equal(item.graphDepth,2);assert.equal(tip.graphDepth,3);
assert.deepEqual(log,[['item',false],['tip',false]]);assert.deepEqual(events,['remove:on','insert:off']);
assert.deepEqual(tip.getPosition().toArray(),[-3,2,0]);
console.log('GRAPH_PRODUCTION_BASELINE_OK hooks=2 events=2 tip=-3,2,0');
