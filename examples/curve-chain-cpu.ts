/**
 * Curve-chain CPU demo.
 *
 * Builds one animated parent bone, two non-overlapping chains that hang from
 * it, and guide bones whose sampled world origins form cubic Bezier curves
 * shared by both channels. The clip's ordinary tracks animate the parent, the
 * Animator freezes every guide point and solves both chains, and SkinMTX then
 * deforms a real vertex whose position is asserted analytically.
 *
 * Run with:
 *   node --import ./task_support/loader.mjs examples/curve-chain-cpu.ts
 * Prints CURVE_CHAIN_DEMO_OK on success.
 */
import assert from 'node:assert/strict';

import Armature       from '../src/armature/Armature';
import SkinMTX        from '../src/armature/skins/SkinMTX';
import Animator       from '../src/animation/Animator';
import Clip           from '../src/animation/Clip';
import Vec3Track      from '../src/animation/tracks/Vec3Track';
import QuatTrack      from '../src/animation/tracks/QuatTrack';
import CurveChainTrack from '../src/animation/tracks/CurveChainTrack';
import { IKChain }    from '../src/ikrig/rigs/IKChain';
import { quat, vec3 } from 'gl-matrix';

//#region BUILD THE ARMATURE =================================================
const arm = new Armature();

// Animated parent bone. Everything below it inherits its transform.
arm.addBone( 'base', -1, undefined, [ 0, 0, 0 ] );

// Left chain: root -> elbow -> tip (three links, each 1 unit at bind).
arm.addBone( 'l_root', 0, undefined, [ 0, 0, 0 ] );
arm.addBone( 'l_mid', 1, undefined, [ 0, 1, 0 ] );
arm.addBone( 'l_tip', 2, undefined, [ 0, 1, 0 ] );

// Right chain: a sibling of the left chain, sharing only the parent `base`.
arm.addBone( 'r_root', 0, undefined, [ 0, 0, 0 ] );
arm.addBone( 'r_mid', 4, undefined, [ 0, 1, 0 ] );
arm.addBone( 'r_tip', 5, undefined, [ 0, 1, 0 ] );

// Guide bones are children of `base`, so the ordinary animation drives them.
// Local origins below are positions relative to `base`.
const guidePositions: Record< string, [number, number, number] > = {
    g0: [  0.0, 0.0, 0 ],   // shared first guide (== both chain roots)
    gl1: [  0.2, 0.7, 0 ],
    gl2: [  0.5, 1.6, 0 ],
    gm:  [  0.8, 2.4, 0 ],  // shared junction guide
    gl4: [  0.7, 3.2, 0 ],
    gl5: [  0.4, 4.0, 0 ],
    gl6: [  0.0, 4.6, 0 ],
    gr1: [ -0.2, 0.7, 0 ],
    gr2: [ -0.5, 1.6, 0 ],
    gr4: [ -0.7, 3.2, 0 ],
    gr5: [ -0.4, 4.0, 0 ],
    gr6: [  0.0, 4.6, 0 ],
};

const guideIndex: Record< string, number > = {};
for( const name of Object.keys( guidePositions ) ){
    guideIndex[ name ] = arm.addBone( name, 0, undefined, guidePositions[ name ] ).idx;
}

// Bind once: SkinMTX captures every bone's rest-world inverse, and the leaf
// bones get the default length so every chain link is exactly 1.
arm.bind( SkinMTX );

// Both channels share guides g0 and gm; they use disjoint chain bones and
// neither chain contains an ancestor of the other, so they may coexist.
const leftChain  = new IKChain( [ 'l_root', 'l_mid', 'l_tip' ], arm );
const rightChain = new IKChain( [ 'r_root', 'r_mid', 'r_tip' ], arm );

const leftGuides  = [ 'g0', 'gl1', 'gl2', 'gm', 'gl4', 'gl5', 'gl6' ].map( n => guideIndex[ n ] );
const rightGuides = [ 'g0', 'gr1', 'gr2', 'gm', 'gr4', 'gr5', 'gr6' ].map( n => guideIndex[ n ] );

const leftTrack  = new CurveChainTrack( leftChain,  leftGuides,  { positionTolerance: 1e-6 } );
const rightTrack = new CurveChainTrack( rightChain, rightGuides, { positionTolerance: 1e-6 } );

//#endregion =================================================================

//#region ANIMATED CLIP ======================================================
const clip = new Clip();
clip.name     = 'curve-chain-demo';
clip.duration = 1;
clip.frameCount = 2;
clip.timeStamps = [ new Float32Array( [ 0, 1 ] ) ];

// Animate the parent bone: translate it up and rotate it about Z.
const posTrack = new Vec3Track();
posTrack.timeStampIndex = 0;
posTrack.boneIndex      = 0;
posTrack.values         = new Float32Array( [ 0,0,0, 0.5,0.75,0 ] );

const rotTrack = new QuatTrack();
rotTrack.timeStampIndex = 0;
rotTrack.boneIndex      = 0;
const q0 = quat.create();
const q1 = quat.setAxisAngle( quat.create(), [ 0, 0, 1 ], 0.35 );
rotTrack.values = new Float32Array( [ ...Array.from( q0 ), ...Array.from( q1 ) ] );

clip.tracks.push( posTrack, rotTrack );

// Channels are solved after the tracks, in the declared order.
clip.curveChains.push( leftTrack, rightTrack );

const pose     = arm.newPose( true );
const animator = new Animator().setClip( clip ).update( 0.5 );

animator.applyPose( pose );

//#endregion =================================================================

//#region ASSERTIONS =========================================================
function dist( a: ArrayLike<number>, b: ArrayLike<number> ): number{
    return Math.hypot( a[0]-b[0], a[1]-b[1], a[2]-b[2] );
}

// The ordinary tracks really moved the parent (so we are not in the bind pose).
assert.ok(
    dist( pose.bones[0].local.pos, [0,0,0] ) > 0.1 ||
    Math.abs( pose.bones[0].local.rot[2] ) > 0.01,
    'parent bone was animated by the clip',
);

// A clip without curveChains must not disturb the ordinary sampling result.
// The chains never touch the animated parent, so a plain-clip pose must agree
// with the curve-chain pose on every non-chain bone.
{
    const plain = new Clip();
    plain.duration  = clip.duration;
    plain.timeStamps = clip.timeStamps;
    plain.tracks    = clip.tracks;
    const plainPose = arm.newPose( true );
    new Animator().setClip( plain ).update( 0.5 ).applyPose( plainPose );

    const chainBones = new Set< number >();
    for( const ch of [ leftChain, rightChain ] ) for( const l of ch.links ) chainBones.add( l.idx );

    for( let i=0; i < arm.bones.length; i++ ){
        if( chainBones.has( i ) ) continue;
        const pa = plainPose.bones[ i ];
        const pb = pose.bones[ i ];
        for( let k=0; k < 3; k++ ){
            assert.ok( Math.abs( pa.local.pos[k] - pb.local.pos[k] ) <= 1e-7,
                `non-chain bone ${i} local pos unaffected (${k})` );
        }
        for( let k=0; k < 4; k++ ){
            assert.ok( Math.abs( pa.local.rot[k] - pb.local.rot[k] ) <= 1e-6,
                `non-chain bone ${i} local rot unaffected (${k})` );
        }
    }
}

// Verify both solved chains: unit link lengths, first guide at the root, and
// the tail sitting exactly on the sampled/frozen curve.
function checkChain( chain: IKChain, track: CurveChainTrack, label: string ): void{
    const links = chain.links;
    const pts: Array< [number, number, number] > = [];

    const root = pose.bones[ links[ 0 ].idx ].world.pos;
    const g0   = pose.bones[ track.guideBones[ 0 ] ].world.pos;
    assert.ok( dist( root, g0 ) <= 1e-6, `${label}: first guide == chain root` );

    for( let i=0; i < links.length; i++ ){
        const b = pose.bones[ links[ i ].idx ];
        pts.push( [ b.world.pos[0], b.world.pos[1], b.world.pos[2] ] );
    }

    // Bone origins must be exactly one link length apart.
    for( let i=0; i + 1 < pts.length; i++ ){
        const d = dist( pts[i], pts[i+1] );
        assert.ok( Math.abs( d - links[i].len ) <= 1e-5, `${label}: link ${i} length ${d}` );
    }

    // Each world +Y must point at the next solved point.
    for( let i=0; i + 1 < pts.length; i++ ){
        const b = pose.bones[ links[ i ].idx ].world;
        const yWorld = transformDir( b.rot, [ 0, 1, 0 ] );
        const want   = [ pts[i+1][0]-pts[i][0], pts[i+1][1]-pts[i][1], pts[i+1][2]-pts[i][2] ];
        const wl     = Math.hypot( want[0], want[1], want[2] );
        const dot    = ( yWorld[0]*want[0] + yWorld[1]*want[1] + yWorld[2]*want[2] ) / wl;
        assert.ok( dot > 1 - 1e-6, `${label}: bone ${i} +Y aims at next point` );
    }

    // The tail must lie on the frozen guide curve: check the shortest distance
    // from the tail to a dense sampling of the exact Bezier segments.
    const tail = transformDir( pose.bones[ links[ links.length-1 ].idx ].world.rot, [ 0, 1, 0 ] );
    const tb   = pose.bones[ links[ links.length-1 ].idx ].world;
    const tailP = [
        tb.pos[0] + tail[0] * links[ links.length-1 ].len,
        tb.pos[1] + tail[1] * links[ links.length-1 ].len,
        tb.pos[2] + tail[2] * links[ links.length-1 ].len,
    ];
    const segs = track.guideBones.length;
    let best = Infinity;
    for( let s=0; s < ( segs - 1 ) / 3; s++ ){
        const c = [ 0,1,2,3 ].map( k => {
            const wp = pose.bones[ track.guideBones[ s*3 + k ] ].world.pos;
            return [ wp[0], wp[1], wp[2] ];
        });
        for( let i=0; i <= 400; i++ ){
            const t = i / 400;
            const mt = 1 - t;
            const p = [ 0,1,2 ].map( k =>
                mt*mt*mt*c[0][k] + 3*mt*mt*t*c[1][k] + 3*mt*t*t*c[2][k] + t*t*t*c[3][k] );
            best = Math.min( best, dist( tailP, p ) );
        }
    }
    assert.ok( best <= 5e-3, `${label}: tail lies on the guide curve (${best})` );
}

checkChain( leftChain,  leftTrack,  'left' );
checkChain( rightChain, rightTrack, 'right' );

// Skin a vertex rigidly attached to the left tip: its posed position must
// equal the analytic pose transform of its rest-local position.
arm.updateSkinFromPose( pose );
const offsets = arm.getSkinOffsets() as Float32Array[];
assert.ok( offsets && offsets.length === 1, 'SkinMTX produced an offset buffer' );

const tipIdx = leftChain.links[ 2 ].idx;
const restPoint = restWorldPoint( arm, tipIdx, [ 0, 0.5, 0 ] );
const skinPoint = applySkin( offsets[0], tipIdx, restPoint );

const tipWorld = pose.bones[ tipIdx ].world;
const expect   = posePoint( tipWorld, [ 0, 0.5, 0 ] );

for( let k=0; k < 3; k++ ){
    assert.ok( Math.abs( skinPoint[k] - expect[k] ) <= 1e-4,
        `skinned vertex ${k}: ${skinPoint[k]} vs ${expect[k]}` );
}

// Also verify the deformed vertex actually moved away from its rest position,
// so the assertion above cannot pass trivially.
assert.ok( dist( skinPoint, restPoint ) > 0.05,
    'skinning deformed the vertex away from its rest position' );

//#endregion =================================================================

//#region HELPERS ============================================================
function transformDir( q: any, v: number[] ): number[]{
    const out = vec3.transformQuat( vec3.create(), v as never, q );
    return [ out[0], out[1], out[2] ];
}

function posePoint( t: any, v: number[] ): number[]{
    const out = vec3.mul( vec3.create(), v as never, t.scl );
    vec3.transformQuat( out, out, t.rot );
    vec3.add( out, out, t.pos );
    return [ out[0], out[1], out[2] ];
}

/** Rest-world point under the SkinMTX bind convention (bone locals only). */
function restWorldPoint( a: Armature, idx: number, v: number[] ): number[]{
    const chain: any[] = [];
    let i = idx;
    while( i !== -1 ){
        chain.unshift( a.bones[ i ] );
        i = a.bones[ i ].pidx;
    }

    let point = v.slice();
    for( let k=chain.length-1; k >= 0; k-- ){
        const local = chain[ k ].local;
        const out   = vec3.mul( vec3.create(), point as never, local.scl );
        vec3.transformQuat( out, out, local.rot );
        vec3.add( out, out, local.pos );
        point = [ out[0], out[1], out[2] ];
    }
    return point;
}

function applySkin( buf: Float32Array, idx: number, p: number[] ): number[]{
    const o = idx * 16;
    return [
        buf[o+0]*p[0] + buf[o+4]*p[1] + buf[o+8]*p[2]  + buf[o+12],
        buf[o+1]*p[0] + buf[o+5]*p[1] + buf[o+9]*p[2]  + buf[o+13],
        buf[o+2]*p[0] + buf[o+6]*p[1] + buf[o+10]*p[2] + buf[o+14],
    ];
}
//#endregion

console.log( 'CURVE_CHAIN_DEMO_OK' );
