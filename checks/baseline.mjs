import assert from 'node:assert/strict';
import UTIF from '../UTIF.js';
import {floatTiff} from './tiff_fixture.mjs';
// A 32-bit first conversion through genuine directory / strip decoding.
for(const compression of [1,8,32773]){
const buffer=floatTiff({compression,width:2,height:1,rows:1,values:[0,0.25,0.5,1,0.75,0.125]});
const ifds=UTIF.decode(buffer);UTIF.decodeImage(buffer,ifds[0],ifds);
const rgba=UTIF.toRGBA8(ifds[0]);
assert.deepEqual([...rgba],[0,137,188,255,255,225,99,255]);
console.log('UTIF_FLOAT_FIRST_BASELINE_OK compression='+compression+' rgba='+[...rgba].join(','));
}
// Ordinary 8-bit RGB independently constructed to keep integer behavior visible.
const raw={width:2,height:1,t258:[8,8,8],t277:[3],t262:[2],data:new Uint8Array([12,34,56,78,90,123])};
assert.deepEqual([...UTIF.toRGBA8(raw)],[12,34,56,255,78,90,123,255]);
console.log('UTIF_RGB8_BASELINE_OK');
