import assert from 'node:assert/strict';
import GLSL from '../index.js';
const samples=[
 [`void bump(inout float x){x+=3.;} void main(){vec2 a[2];a[0]=vec2(1,2);a[1]=vec2(3,4);int k=0;bump(a[k++].y);gl_FragColor=vec4(a[0],float(k),a[1].y);}`,[1,5,1,4]],
 [`struct S{vec2 c;}; S touch(S s){s.c.x=9.;return s;} void main(){S a=S(vec2(1,2));S b=touch(a);b.c.y=7.;gl_FragColor=vec4(a.c,b.c);}`,[1,2,9,7]],
 [`float f(inout vec2 v){v.y+=2.;if(v.x>0.)return 7.;return 3.;} void main(){vec2 v=vec2(1,4);float r=f(v);gl_FragColor=vec4(v,r,1);}`,[1,6,7,1]]
];
for(const optimize of [false,true]) for(const [source,expected] of samples){
 const js=GLSL({optimize})(source);
 const actual=new Function('let gl_FragColor;'+js+';main();return Array.from(gl_FragColor);')();
 assert.deepEqual(actual,expected);
}
console.log('GLSL_BASELINE_OK shaders=3 executions=6 optimized-and-unoptimized=equal');
