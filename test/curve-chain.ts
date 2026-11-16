/**
 * Curve-chain tests.
 *
 * Registered groups (see task_support/check.mjs):
 *   roots    - multi-segment and even-multiplicity earliest-first intersections
 *   sampling - ordinary animation + guide snapshot, failures, retry, ordering
 *   skinning - real skinning through SkinMTX with a non-identity bind inverse
 *
 * Every geometric assertion is checked against an independent numeric
 * reference (dense sampling + bisection, or direct Bezier evaluation), not just
 * against "is finite".
 */
// eslint-disable-next-line @typescript-eslint/no-explicit-any
import { group, check } from '../task_support/check.mjs';

import Armature from '../src/armature/Armature';
import { IKChain } from '../src/ikrig/rigs/IKChain';
import CurveChainTrack, { CurveChainError, CURVE_CHAIN_ERRORS, validateCurveChainSet } from '../src/animation/tracks/CurveChainTrack';
import CurveChainSolver from '../src/ikrig/solvers/CurveChainSolver';
import Animator from '../src/animation/Animator';
import Clip from '../src/animation/Clip';
import Vec3Track from '../src/animation/tracks/Vec3Track';
import QuatTrack from '../src/animation/tracks/QuatTrack';
import SkinMTX from '../src/armature/skins/SkinMTX';
import { vec3, quat } from 'gl-matrix';

//#region INDEPENDENT GEOMETRY REFERENCE =====================================

type V3 = number[];

/** Exact cubic Bezier evaluation from four control points. */
function bez( c: V3[], t: number ): V3{
    const mt = 1 - t;
    return [ 0, 1, 2 ].map( k =>
        mt*mt*mt*c[0][k] + 3*mt*mt*t*c[1][k] + 3*mt*t*t*c[2][k] + t*t*t*c[3][k],
    );
}

function vlen( a: V3, b: V3 ): number{
    return Math.hypot( a[0]-b[0], a[1]-b[1], a[2]-b[2] );
}

function piecewise( segments: V3[][], t: number ): V3{
    const s = Math.min( segments.length - 1, Math.floor( t ) );
    const u = Math.min( 1, t - s );
    return bez( segments[ s ], u );
}

/**
 * Independent reference for "first parameter > fromT whose curve point is
 * exactly linkLen from center". Uses a dense scan to locate brackets plus
 * bisection, and a global minimum search for tangencies. Deliberately shares
 * no code with the implementation.
 */
function referenceFirstHit( segments: V3[][], center: V3, linkLen: number, fromT: number ): number | null{
    const total = segments.length;
    const N = 40000;
    const f = ( t: number ): number => vlen( piecewise( segments, t ), center ) - linkLen;

    let prevT = fromT;
    let prevV = f( prevT );
    let bestT = prevT;
    let bestAbs = Math.abs( prevV );
    if( bestAbs === 0 ) return prevT;

    for( let i=1; i <= N; i++ ){
        const t = fromT + ( total - fromT ) * i / N;
        const v = f( t );

        if( Math.abs( v ) < bestAbs ){ bestAbs = Math.abs( v ); bestT = t; }
        if( prevV * v < 0 ){
            let lo = prevT, hi = t;
            for( let k=0; k < 200; k++ ){
                const m = 0.5 * ( lo + hi );
                if( f( m ) === 0 ){ lo = hi = m; break; }
                if( ( f( m ) < 0 ) === ( prevV < 0 ) ) lo = m;
                else hi = m;
            }
            return 0.5 * ( lo + hi );
        }
        if( v === 0 ) return t;
        prevT = t; prevV = v;
    }

    // No sign change: the first hit can only be a tangency.
    return ( bestAbs < 1e-5 ) ? bestT : null;
}

//#endregion ==================================================================

//#region FIXTURES ===========================================================

/** Chain a->b->c where every link length is exactly 1. */
function buildLinearRig(): { arm: Armature, chain: IKChain, pose: ReturnType<Armature['newPose']> }{
    const arm = new Armature();
    arm.addBone( 'base', -1, undefined, [0,0,0] );
    arm.addBone( 'a', 0, undefined, [0,0,0] );
    arm.addBone( 'b', 1, undefined, [0,1,0] );
    arm.addBone( 'c', 2, undefined, [0,1,0] );
    arm.bind();
    const pose  = arm.newPose( true );
    const chain = new IKChain( [ 'a', 'b', 'c' ], arm );
    return { arm, chain, pose };
}

/** Build a pair of free "guide" bones (no parent) positioned by their local offsets. */
function addGuideBones( arm: Armature, prefix: string, pts: V3[] ): number[]{
    const out: number[] = [];
    for( let i=0; i < pts.length; i++ ){
        const b = arm.addBone( `${prefix}${i}`, -1, undefined, pts[ i ] as never );
        out.push( b.idx );
    }
    return out;
}

//#endregion ==================================================================

//#region ROOTS ==============================================================

group( 'roots', ()=>{
    //-------------------------------------------------------------
    // Multi-segment: two cubic segments, first hit is the very first
    // parameter of segment 0, later hits are further along. Guide curve
    // is a straight line at constant speed, so the exact answers are known:
    // segments [0,1,2,3] and [3,4,5,6] along +X with unit spacing, chain of
    // length 3 must land at x=1,2,3.
    {
        const { arm, chain, pose } = buildLinearRig();
        // Reposition chain bones along +X for this fixture.
        pose.bones[ 2 ].local.pos = vec3.fromValues( 1, 0, 0 );
        pose.bones[ 3 ].local.pos = vec3.fromValues( 1, 0, 0 );
        pose.updateWorld();

        // Straight line x = t on a 2-segment spline.
        const pts: V3[] = [ [0,0,0], [1,0,0], [2,0,0], [3,0,0], [4,0,0], [5,0,0], [6,0,0] ];
        const guides = addGuideBones( arm, 'gm', pts );
        // Rebind so lengths reflect the new positions (all 1 already).
        arm.bind();
        const p2 = arm.newPose( true );

        const tr  = new CurveChainTrack( chain, guides, { positionTolerance: 1e-9 } );
        const res = new CurveChainSolver().resolve( tr.capture( p2 ), p2 );

        // Exactly the integers 1,2,3 (root at 0), tolerance from float32 bones.
        for( let i=0; i < 3; i++ ){
            const d = vlen( res.points[ i ], res.points[ i + 1 ] );
            check( Math.abs( d - 1 ) <= 1e-6, `multi-segment link ${i} length ${d}` );
        }
        check( Math.abs( res.points[1][0] - 1 ) <= 1e-6, 'first origin x=1' );
        check( Math.abs( res.points[2][0] - 2 ) <= 1e-6, 'second origin x=2' );
        check( Math.abs( res.points[3][0] - 3 ) <= 1e-6, 'tail x=3' );
        check( res.parameters[0] === 0 && res.parameters[1] > res.parameters[0] &&
               res.parameters[2] > res.parameters[1] && res.parameters[3] > res.parameters[2],
            'parameters strictly increasing' );

        // Independent reference for each step.
        let cursor = 0;
        for( let i=0; i < 3; i++ ){
            const want = referenceFirstHit(
                [ [[0,0,0],[1,0,0],[2,0,0],[3,0,0]], [[3,0,0],[4,0,0],[5,0,0],[6,0,0]] ],
                res.points[ i ], 1, cursor,
            );
            check( want !== null && Math.abs( want - res.parameters[ i + 1 ] ) <= 1e-4,
                `multi-segment reference step ${i}` );
            cursor = res.parameters[ i + 1 ];
        }
    }

    //-------------------------------------------------------------
    // Even-multiplicity (tangent) root: segment A ends at x=1, segment B
    // (a "there-and-back" cubic) touches the sphere of radius 1 centred on
    // x=1 at exactly its midpoint but never crosses it earlier.
    //   B: P0=(1,0,0) P1=(7/3,0,0) P2=(7/3,0,0) P3=(1,0,0)
    //   dist(P0)=0, dist(P1/2=4/3)=1/3 < 1 -> wait, use scaled controls.
    // Use B: P0=(1,0,0), P1=(2.3333,0,0), P2=(2.3333,0,0), P3=(1,0,0)
    //   x(t) = 1 + 4t - 4t^2, max x = 2 at t=1/2, dist from (1) = 1 there.
    // The tangent hit must be detected even though dist - r touches zero
    // without changing sign.
    {
        const arm = new Armature();
        arm.addBone( 'r', -1, undefined, [0,0,0] );
        arm.addBone( 'b1', 0, undefined, [0,1,0] );
        const p0: V3 = [0,0,0], p1: V3 = [1/3,0,0], p2: V3 = [2/3,0,0], p3: V3 = [1,0,0];
        const p4: V3 = [7/3,0,0], p5: V3 = [7/3,0,0], p6: V3 = [1,0,0];
        const guides = addGuideBones( arm, 'g', [p0,p1,p2,p3,p4,p5,p6] );
        arm.bind();
        const pose  = arm.newPose( true );
        const chain = new IKChain( [ 'r', 'b1' ], arm );
        const tr    = new CurveChainTrack( chain, guides, { positionTolerance: 1e-5 } );

        const res = new CurveChainSolver().resolve( tr.capture( pose ), pose );

        // First origin exactly at the shared endpoint, then the tangency.
        check( Math.abs( res.points[1][0] - 1 ) <= 1e-6, 'tangency: first hit at x=1' );
        check( Math.abs( res.parameters[1] - 1 ) <= 1e-6, 'tangency: first hit is segment end' );
        const tangentX = res.points[2][0];
        // Exact tangency occurs at x = 2 for this curve. float32 stored guide
        // bones can move it by ~1e-7; verify against the independent reference.
        check( Math.abs( tangentX - 2 ) <= 1e-5, `tangency point x≈2 (got ${tangentX})` );
        check( Math.abs( res.parameters[2] - 1.5 ) <= 1e-4,
            `tangency parameter ≈1.5 (got ${res.parameters[2]})` );

        // Independent reference over the exact same two segments.
        const segmentsRef: V3[][] = [
            [p0,p1,p2,p3],
            [p3,p4,p5,p6],
        ];
        const want = referenceFirstHit( segmentsRef, res.points[1], 1, 1 );
        check( want !== null && Math.abs( want - res.parameters[2] ) <= 1e-3,
            `tangency reference parameter ${want} vs ${res.parameters[2]}` );
    }

    //-------------------------------------------------------------
    // Earliest root wins, not a later crossing. Chain of length 2 on a curve
    // that dips inside the sphere first, then exits, then re-enters.
    {
        const { arm, chain, pose } = buildLinearRig();
        // Curve: quarter-ish arc starting at origin.
        const pts: V3[] = [ [0,0,0], [2,0,0], [2,2,0], [0,2,0] ];
        const guides = addGuideBones( arm, 'ga', pts );
        arm.bind();
        const p2 = arm.newPose( true );
        const tr = new CurveChainTrack( chain, guides, { positionTolerance: 1e-9 } );

        const res = new CurveChainSolver().resolve( tr.capture( p2 ), p2 );
        const deg = res.parameters.slice( 1 );
        check( deg.length === 3, 'arc produced three parameters' );

        // Independent reference: the first crossing of radius 1 is at t≈0.2045.
        const want = referenceFirstHit( [ pts ], [0,0,0], 1, 0 );
        check( want !== null && Math.abs( want - res.parameters[1] ) <= 1e-6,
            `arc first hit matches reference ${want} vs ${res.parameters[1]}` );
        check( Math.abs( res.parameters[1] - 0.20454 ) <= 1e-4,
            'arc first hit is the earliest root, not a later one' );
        for( let i=0; i < 3; i++ ){
            const d = vlen( res.points[i], res.points[i+1] );
            check( Math.abs( d - 1 ) <= 1e-7, `arc link ${i} stays unit length (${d})` );
        }
        // Section endpoints must be visited in ascending order.
        for( let i=1; i < res.points.length; i++ ){
            check( res.points[i][1] >= res.points[i-1][1] - 1e-9, 'arc y increases monotonically' );
        }
    }

    //-------------------------------------------------------------
    // Shared segment endpoints are only counted once: a straight spline
    // whose junction is exactly one link away must land on it exactly once.
    {
        const { arm, chain, pose } = buildLinearRig();
        pose.bones[ 2 ].local.pos = vec3.fromValues( 1, 0, 0 );
        pose.bones[ 3 ].local.pos = vec3.fromValues( 1, 0, 0 );
        pose.updateWorld();
        const pts: V3[] = [ [0,0,0], [0.5,0,0], [1.5,0,0], [2,0,0], [2.5,0,0], [3.5,0,0], [4,0,0] ];
        const guides = addGuideBones( arm, 'gs', pts );
        arm.bind();
        const p2 = arm.newPose( true );
        const tr = new CurveChainTrack( chain, guides, { positionTolerance: 1e-9 } );
        const res = new CurveChainSolver().resolve( tr.capture( p2 ), p2 );
        // No duplicate parameter (the shared endpoint at t=1 must not be
        // reported again at t=1+0).
        for( let i=1; i < res.parameters.length; i++ ){
            check( res.parameters[i] - res.parameters[i-1] > 1e-9,
                'parameters do not repeat a shared endpoint' );
        }
        check( res.parameters.length === 4, 'one point per link plus root' );
    }
    //-------------------------------------------------------------
    // Guide bones may belong to the constrained chain itself.
    {
        const arm = new Armature();
        arm.addBone( 'r', -1, undefined, [0,0,0] );
        arm.addBone( 'b1', 0, undefined, [0,1,0] );
        arm.addBone( 'b2', 1, undefined, [0,1,0] );
        arm.bind();
        const pose  = arm.newPose( true );
        const chain = new IKChain( [ 'r', 'b1' ], arm );
        // r origin (0,0,0), b1 origin (0,1,0), b1 again, b2 origin (0,2,0).
        const tr  = new CurveChainTrack( chain, [ 0, 1, 1, 2 ], { positionTolerance: 1e-5 } );
        const res = new CurveChainSolver().resolve( tr.capture( pose ), pose );
        check( Math.abs( res.points[1][1] - 1 ) <= 1e-6, 'self-guide chain first origin' );
        check( Math.abs( res.points[2][1] - 2 ) <= 1e-6, 'self-guide chain tail' );
        check( res.parameters.length === 3, 'self-guide chain solved fully' );
    }

    //-------------------------------------------------------------
    // Exact 180-degree direction reversal: previous +Z must be kept as the
    // half-turn axis, so the transported frame stays right-handed.
    {
        const arm = new Armature();
        arm.addBone( 'r', -1, undefined, [0,0,0] );
        arm.addBone( 'b1', 0, undefined, [0,1,0] );
        arm.addBone( 'b2', 1, undefined, [0,1,0] );
        arm.addBone( 'b3', 2, undefined, [0,1,0] );
        // Up to y=3 then back down: the last link points at -Y.
        const pts: V3[] = [ [0,0,0], [0,1,0], [0,3,0], [0,3,0], [0,3,0], [0,1,0], [0,-1,0] ];
        const guides = addGuideBones( arm, 'rv', pts );
        arm.bind();
        const pose  = arm.newPose( true );
        const chain = new IKChain( [ 'r', 'b1', 'b2', 'b3' ], arm );
        const tr    = new CurveChainTrack( chain, guides, { positionTolerance: 1e-5 } );
        const res   = new CurveChainSolver().resolve( tr.capture( pose ), pose );

        // Last frame +Y must point at -Y and its basis must remain orthonormal.
        const q = res.frames[ res.frames.length - 1 ];
        const y = [ 2*(q[0]*q[1] + q[3]*q[2]), 1 - 2*(q[0]*q[0] + q[2]*q[2]), 2*(q[1]*q[2] - q[3]*q[0]) ];
        check( Math.abs( y[1] + 1 ) <= 1e-6, `reversal +Y points down (${y[1]})` );

        const x = [ 1 - 2*(q[1]*q[1] + q[2]*q[2]), 2*(q[0]*q[1] + q[3]*q[2]), 2*(q[0]*q[2] - q[3]*q[1]) ];
        const z = [ 2*(q[0]*q[2] - q[3]*q[1]), 2*(q[1]*q[2] + q[3]*q[0]), 1 - 2*(q[0]*q[0] + q[1]*q[1]) ];
        check( Math.abs( y[0]*z[0] + y[1]*z[1] + y[2]*z[2] ) <= 1e-6, 'reversal frame Y.Z=0' );
        check( Math.abs( x[0]*y[0] + x[1]*y[1] + x[2]*y[2] ) <= 1e-6, 'reversal frame X.Y=0' );
        const xz = [ x[1]*y[2] - x[2]*y[1], x[2]*y[0] - x[0]*y[2], x[0]*y[1] - x[1]*y[0] ];
        check( Math.abs( xz[0]-z[0] ) + Math.abs( xz[1]-z[1] ) + Math.abs( xz[2]-z[2] ) <= 1e-5,
            'reversal frame X x Y == Z (right handed)' );
    }
});

//#endregion ==================================================================

function makeClipWithTranslation( arm: Armature, xValue: number ): Clip{
    const clip = new Clip();
    clip.duration = 1;
    clip.frameCount = 2;
    clip.timeStamps = [ new Float32Array( [0, 1] ) ];

    const t = new Vec3Track();
    t.timeStampIndex = 0;
    t.boneIndex = 0;                                   // base bone
    t.values = new Float32Array( [ xValue, 0, 0, xValue, 0, 0 ] );
    clip.tracks.push( t );
    return clip;
}

group( 'sampling', ()=>{
    //-------------------------------------------------------------
    // Ordinary animation then guide snapshot: guides must read the world
    // positions produced by the normal tracks (not the bind pose).
    {
        const arm = new Armature();
        arm.addBone( 'hip', -1, undefined, [0,0,0] );
        arm.addBone( 'a', 0, undefined, [0,1,0] );
        arm.addBone( 'b', 1, undefined, [0,1,0] );
        // Guide chain: g0 is the chain root, then three more free bones.
        arm.addBone( 'g0', -1, undefined, [0,1,0] );
        arm.addBone( 'g1', -1, undefined, [0,2,0] );
        arm.addBone( 'g2', -1, undefined, [0,4,0] );
        arm.addBone( 'g3', -1, undefined, [0,5,0] );
        arm.bind();

        const chain = new IKChain( [ 'a', 'b' ], arm );
        const clip  = makeClipWithTranslation( arm, 5 );
        clip.curveChains.push( new CurveChainTrack( chain, [ 3, 4, 5, 6 ], { positionTolerance: 1e-5 } ) );

        // Animate the guides too, so the snapshot depends on normal sampling.
        const gTrack = new Vec3Track();
        gTrack.timeStampIndex = 0;
        gTrack.boneIndex      = 3;                                 // g0
        gTrack.values         = new Float32Array( [5,1,0, 5,1,0] );
        clip.tracks.push( gTrack );

        const pose = arm.newPose( true );
        const animator = new Animator().setClip( clip ).update( 0.5 );
        animator.applyPose( pose );

        // Hip moved by +5 on X, so the frozen guide snapshot must reflect it.
        const cap = clip.curveChains[0].capture( pose );
        check( Math.abs( cap.guidePoints[0][0] - 5 ) <= 1e-5, 'guide snapshot follows animation' );
        check( Math.abs( cap.rootPos[0] - 5 ) <= 1e-5, 'root sampled from animation' );
        check( Math.abs( cap.guidePoints[0][0] - cap.rootPos[0] ) <= 1e-5,
            'first guide equals sampled root' );

        // Arms were solved onto the (translated) guide curve.
        check( Math.abs( pose.bones[1].world.pos[0] - 5 ) <= 1e-4, 'a follows translated curve' );
    }

    //-------------------------------------------------------------
    // Failure: guide #0 does not match the sampled root -> whole applyPose
    // fails and every local/world/offset is restored.
    {
        const arm = new Armature();
        arm.addBone( 'hip', -1 );
        arm.addBone( 'a', 0, undefined, [0,1,0] );
        arm.addBone( 'b', 1, undefined, [0,1,0] );
        arm.bind();
        arm.addBone( 'g0', -1, undefined, [9,9,9] );     // deliberately wrong
        arm.addBone( 'g1', -1, undefined, [0,1,0] );
        arm.addBone( 'g2', -1, undefined, [0,3,0] );
        arm.addBone( 'g3', -1, undefined, [0,4,0] );
        arm.bind();

        const chain = new IKChain( [ 'a', 'b' ], arm );
        const clip  = new Clip();
        clip.duration = 1;
        clip.timeStamps = [ new Float32Array( [0,1] ) ];
        const v = new Vec3Track(); v.timeStampIndex = 0; v.boneIndex = 0;
        v.values = new Float32Array( [ 2,0,0, 2,0,0 ] );
        clip.tracks.push( v );
        clip.curveChains.push( new CurveChainTrack( chain, [ 3,4,5,6 ], { positionTolerance: 1e-6 } ) );

        const pose = arm.newPose( true );
        // Pre-seed a distinctive offset and one bone local so restore is visible.
        pose.offset.pos[0] = 0.25;
        pose.updateWorld();
        const before = snapshotPose( pose );

        const animator = new Animator().setClip( clip ).update( 0.5 );
        let threw: any = null;
        try { animator.applyPose( pose ); } catch( e ){ threw = e; }
        check( threw instanceof CurveChainError, 'mismatched guide throws CurveChainError' );
        check( threw.code === CURVE_CHAIN_ERRORS.FIRST_GUIDE_MISMATCH, 'mismatch error code' );
        checkSamePose( before, pose, 'root mismatch rolls back everything' );

        // Retry must be unaffected by the failure residue (same error again,
        // same restored state).
        let threw2: any = null;
        try { animator.applyPose( pose ); } catch( e ){ threw2 = e; }
        check( threw2 && threw2.code === CURVE_CHAIN_ERRORS.FIRST_GUIDE_MISMATCH, 'retry fails identically' );
        checkSamePose( before, pose, 'retry leaves pose untouched' );
    }

    //-------------------------------------------------------------
    // Rejection: bone indices do not overlap but one chain's bone is an
    // ancestor of another chain's bone. Must fail and roll back.
    {
        const arm = new Armature();
        arm.addBone( 'root', -1 );
        arm.addBone( 'upper', 0, undefined, [0,1,0] );
        arm.addBone( 'fore', 1, undefined, [0,1,0] );
        arm.addBone( 'hand', 2, undefined, [0,1,0] );
        arm.addBone( 'tip', 3, undefined, [0,1,0] );
        // guide bones
        arm.addBone( 'q0', -1 );
        arm.addBone( 'q1', -1, undefined, [0,1,0] );
        arm.addBone( 'q2', -1, undefined, [0,3,0] );
        arm.addBone( 'q3', -1, undefined, [0,4,0] );
        arm.bind();
        const pose = arm.newPose( true );

        // Chain 1: root->upper->fore (bones 0,1,2). Chain 2: hand->tip (3,4).
        // upper is an ancestor of hand: chain 1 has an ancestor of chain 2's bone.
        const c1 = new IKChain( [ 'root', 'upper', 'fore' ], arm );
        const c2 = new IKChain( [ 'hand', 'tip' ], arm );
        const t1 = new CurveChainTrack( c1, [ 5,6,7,8 ], { positionTolerance: 1e-5 } );
        const t2 = new CurveChainTrack( c2, [ 5,6,7,8 ], { positionTolerance: 1e-5 } );

        // Direct validator: conflict reported.
        let conflict: any = null;
        try { validateCurveChainSet( [ t1, t2 ], arm ); } catch( e ){ conflict = e; }
        check( conflict && conflict.code === CURVE_CHAIN_ERRORS.CHANNEL_ANCESTOR_CONFLICT,
            'cross-chain ancestor conflict detected' );
        check( conflict.code !== CURVE_CHAIN_ERRORS.CHANNEL_BONE_OVERLAP,
            'conflict is ancestor, not overlap' );

        // Through Animator: fails, rolls back, no state changed.
        const clip = new Clip();
        clip.duration = 1;
        clip.timeStamps = [ new Float32Array( [0,1] ) ];
        const qt = new QuatTrack(); qt.timeStampIndex = 0; qt.boneIndex = 0;
        qt.values = new Float32Array( [0,0,0,1, 0,0,0,1] );
        clip.tracks.push( qt );
        clip.curveChains.push( t1, t2 );

        const before = snapshotPose( pose );
        const anim = new Animator().setClip( clip ).update( 0.5 );
        let threw: any = null;
        try { anim.applyPose( pose ); } catch( e ){ threw = e; }
        check( threw && threw.code === CURVE_CHAIN_ERRORS.CHANNEL_ANCESTOR_CONFLICT,
            'animator rejects cross-chain ancestor' );
        checkSamePose( before, pose, 'ancestor conflict rolls back' );

        // Swapping declaration order gives the same rejection.
        clip.curveChains = [ t2, t1 ];
        let threw2: any = null;
        try { anim.applyPose( pose ); } catch( e ){ threw2 = e; }
        check( threw2 && threw2.code === CURVE_CHAIN_ERRORS.CHANNEL_ANCESTOR_CONFLICT,
            'ancestor rejection is order independent' );
        checkSamePose( before, pose, 'ordered rejection rolls back' );
    }

    //-------------------------------------------------------------
    // Sibling chains sharing an unconstrained parent bone need not care
    // about declaration order: the solved result is identical either way.
    {
        const build = (): { arm: Armature, pose: any, c1: CurveChainTrack, c2: CurveChainTrack, clip: Clip }=>{
            const arm = new Armature();
            arm.addBone( 'base', -1, undefined, [0,0,0] );
            arm.addBone( 'l1', 0, undefined, [1,0,0] );
            arm.addBone( 'l2', 1, undefined, [0,1,0] );
            arm.addBone( 'r1', 0, undefined, [-1,0,0] );
            arm.addBone( 'r2', 3, undefined, [0,1,0] );
            // guides left
            arm.addBone( 'lg0', -1, undefined, [1,0,0] );
            arm.addBone( 'lg1', -1, undefined, [1,1,0] );
            arm.addBone( 'lg2', -1, undefined, [2,1,0] );
            arm.addBone( 'lg3', -1, undefined, [2,2,0] );
            // guides right
            arm.addBone( 'rg0', -1, undefined, [-1,0,0] );
            arm.addBone( 'rg1', -1, undefined, [-1,1,0] );
            arm.addBone( 'rg2', -1, undefined, [-2,1,0] );
            arm.addBone( 'rg3', -1, undefined, [-2,2,0] );
            arm.bind();

            const c1 = new IKChain( [ 'l1', 'l2' ], arm );
            const c2 = new IKChain( [ 'r1', 'r2' ], arm );
            // Bone order: base 0, l1 1, l2 2, r1 3, r2 4, lg 5..8, rg 9..12.
            const t1 = new CurveChainTrack( c1, [ 5,6,7,8 ], { positionTolerance: 1e-5 } );
            const t2 = new CurveChainTrack( c2, [ 9,10,11,12 ], { positionTolerance: 1e-5 } );

            const clip = new Clip();
            clip.duration = 1;
            clip.timeStamps = [ new Float32Array( [0,1] ) ];
            return { arm, pose: arm.newPose( true ), c1: t1, c2: t2, clip };
        };

        const a = build(); a.clip.curveChains = [ a.c1, a.c2 ];
        const b = build(); b.clip.curveChains = [ b.c2, b.c1 ];

        new Animator().setClip( a.clip ).update( 0.5 ).applyPose( a.pose );
        new Animator().setClip( b.clip ).update( 0.5 ).applyPose( b.pose );

        const la = poseLocals( a.pose );
        const lb = poseLocals( b.pose );
        check( la.length === lb.length, 'same bone count for both orders' );
        for( let i=0; i < la.length; i++ ){
            check( Math.abs( la[i] - lb[i] ) <= 1e-6,
                `declaration order independence at value ${i}` );
        }

        // Also verify both chains really landed on their guides (unit links).
        for( const pose of [ a.pose, b.pose ] ){
            for( const [ b0, b1 ] of [ [1,2], [3,4] ] ){
                const p0 = pose.bones[ b0 ].world.pos;
                const p1 = pose.bones[ b1 ].world.pos;
                check( Math.abs( vlen( [p0[0],p0[1],p0[2]], [p1[0],p1[1],p1[2]] ) - 1 ) <= 1e-4,
                    'sibling chain link stays unit' );
            }
        }
    }

    //-------------------------------------------------------------
    // Validation rejections: all illegal inputs must be refused.
    {
        const { arm, chain, pose } = buildLinearRig();

        const bad = ( label: string, fn: ()=>void, code: string ): void=>{
            let e: any = null;
            try { fn(); } catch( err ){ e = err; }
            check( e && e.code === code, `${label} rejected with ${code} (got ${e && e.code})` );
        };

        // Non-positive link length.
        const c2 = new IKChain( [ 'a', 'b', 'c' ], arm );
        c2.links[1].len = 0;
        bad( 'zero link length', ()=> new CurveChainTrack( c2, [0,1,2,3] ).validate( pose ),
            CURVE_CHAIN_ERRORS.INVALID_LINK_LENGTH );

        // Non-positive tolerance.
        bad( 'zero tolerance', ()=> new CurveChainTrack( chain, [0,1,2,3], { positionTolerance: 0 } ).validate( pose ),
            CURVE_CHAIN_ERRORS.INVALID_TOLERANCE );
        bad( 'negative budget', ()=> new CurveChainTrack( chain, [0,1,2,3], { workBudget: -5 } ).validate( pose ),
            CURVE_CHAIN_ERRORS.INVALID_BUDGET );

        // Non-finite guide value.
        const savedPos = pose.bones[0].local.pos.slice();
        pose.bones[0].local.pos[1] = NaN;
        bad( 'nan guide', ()=> new CurveChainTrack( chain, [0,1,2,3], { positionTolerance: 1e-6 } )
            .validate( pose ), CURVE_CHAIN_ERRORS.NON_FINITE_VALUE );
        pose.bones[0].local.pos.set( savedPos );

        // Bad guide count.
        bad( 'guide count', ()=> new CurveChainTrack( chain, [0,1,2], { positionTolerance: 1e-6 } ).validate( pose ),
            CURVE_CHAIN_ERRORS.INVALID_GUIDE_COUNT );

        // Non-contiguous chain (b then a).
        const badChain = new IKChain(); badChain.links = [ chain.links[1], chain.links[0] ];
        badChain.count = 2;
        bad( 'non-contiguous chain', ()=> new CurveChainTrack( badChain, [0,1,2,3] ).validate( pose ),
            CURVE_CHAIN_ERRORS.NON_CONTIGUOUS_CHAIN );

        // Empty chain.
        bad( 'empty chain', ()=> new CurveChainTrack( new IKChain(), [0,1,2,3] ).validate( pose ),
            CURVE_CHAIN_ERRORS.INVALID_CHAIN );

        // Invalid bone index.
        bad( 'bad guide index', ()=> new CurveChainTrack( chain, [0,1,2,999], {} ).validate( pose ),
            CURVE_CHAIN_ERRORS.INVALID_BONE_INDEX );

        // Invalid (non-unit) scaling on a chain bone.
        pose.bones[1].local.scl[0] = 2;
        bad( 'chain bone scale', ()=> new CurveChainTrack( chain, [0,1,2,3] ).validate( pose ),
            CURVE_CHAIN_ERRORS.UNSUPPORTED_SCALING );
        pose.bones[1].local.scl[0] = 1;

        // Pose offset scaling.
        pose.offset.scl[1] = 1.5;
        bad( 'pose offset scale', ()=> new CurveChainTrack( chain, [0,1,2,3] ).validate( pose ),
            CURVE_CHAIN_ERRORS.UNSUPPORTED_SCALING );
        pose.offset.scl[1] = 1;

        // Channel bone overlap.
        const tc = new CurveChainTrack( chain, [0,1,2,3] );
        bad( 'channel overlap', ()=> validateCurveChainSet( [ tc, tc ], arm ),
            CURVE_CHAIN_ERRORS.CHANNEL_BONE_OVERLAP );
    }

    //-------------------------------------------------------------
    // A failed applyPose must not mutate Armature / Clip / IKChain bind data.
    {
        const arm = new Armature();
        arm.addBone( 'hip', -1 );
        arm.addBone( 'a', 0, undefined, [0,1,0] );
        arm.addBone( 'b', 1, undefined, [0,1,0] );
        arm.bind();
        // g0 coincides with the chain root (hip->a at world (0,1,0)); the rest
        // of the curve is far too short to fit another link.
        arm.addBone( 'g0', -1, undefined, [0,1,0] );
        arm.addBone( 'g1', -1, undefined, [0,1.2,0] );
        arm.addBone( 'g2', -1, undefined, [0,1.3,0] );
        arm.addBone( 'g3', -1, undefined, [0,1.4,0] );
        arm.bind();

        const chain = new IKChain( [ 'a', 'b' ], arm );
        const clip  = new Clip();
        clip.duration = 1;
        clip.timeStamps = [ new Float32Array( [0,1] ) ];
        clip.curveChains.push( new CurveChainTrack( chain, [ 3,4,5,6 ], { positionTolerance: 1e-6 } ) );

        const bindSnapshot = () => ({
            boneLocal: arm.bones.map( b => [
                ...Array.from( b.local.pos ), ...Array.from( b.local.rot ), ...Array.from( b.local.scl ), b.len,
            ]),
            links: chain.links.map( l => [
                l.idx, l.pidx, l.len, ...Array.from( l.bind.pos ), ...Array.from( l.bind.rot ), ...Array.from( l.bind.scl ),
            ]),
            tracks: clip.tracks.length,
            timeStamps: clip.timeStamps.map( ts => Array.from( ts ) ),
        });

        const before = JSON.stringify( bindSnapshot() );
        const pose = arm.newPose( true );
        let e: any = null;
        try { new Animator().setClip( clip ).update( 0.5 ).applyPose( pose ); } catch( err ){ e = err; }
        check( e && e.code === CURVE_CHAIN_ERRORS.CURVE_TOO_SHORT, 'short curve through animator fails' );
        check( JSON.stringify( bindSnapshot() ) === before,
            'Armature / Clip / IKChain bind data unchanged after failure' );
    }

    //-------------------------------------------------------------
    // Curve too short: the guide curve simply cannot fit another link.
    {
        const { arm, chain, pose } = buildLinearRig();
        const guides = addGuideBones( arm, 'gt', [ [0,0,0], [0,0.4,0], [0,0.6,0], [0,0.8,0] ] );
        arm.bind();
        const p2 = arm.newPose( true );
        const tr = new CurveChainTrack( chain, guides, { positionTolerance: 1e-6 } );
        const res = (()=>{ try{ return new CurveChainSolver().resolve( tr.capture( p2 ), p2 ); }catch( e ){ return e as CurveChainError; } })();
        check( res instanceof CurveChainError, 'short curve throws' );
        check( res.code === CURVE_CHAIN_ERRORS.CURVE_TOO_SHORT, 'short curve error code' );
    }

    //-------------------------------------------------------------
    // Work budget: an impossibly small budget fails explicitly, never
    // returning a partial chain or stretching links.
    {
        const { arm, chain, pose } = buildLinearRig();
        // Straight curve of total length 4 so a 3-link chain fits comfortably.
        const guides = addGuideBones( arm, 'gb', [ [0,0,0], [0,4/3,0], [0,8/3,0], [0,4,0] ] );
        arm.bind();
        const p2 = arm.newPose( true );
        const tr = new CurveChainTrack( chain, guides, { positionTolerance: 1e-6, workBudget: 6 } );
        const before = snapshotPose( p2 );
        let e: any = null;
        try { new CurveChainSolver().resolve( tr.capture( p2 ), p2 ); } catch( err ){ e = err; }
        check( e && e.code === CURVE_CHAIN_ERRORS.WORK_BUDGET_EXCEEDED, 'tiny budget rejected' );
        checkSamePose( before, p2, 'budget failure leaves pose untouched' );

        // A generous budget succeeds and reports its work counter.
        const tr2 = new CurveChainTrack( chain, guides, { positionTolerance: 1e-6, workBudget: 1e9 } );
        const res = new CurveChainSolver().resolve( tr2.capture( p2 ), p2 );
        check( res.work.used > 0 && res.work.used <= res.work.budget, 'work statistics sane' );
        check( res.work.links === 3 && res.work.segments === 1, 'work counts links and segments' );
        check( res.points.length === 4, 'full chain solved after a failed attempt' );
    }
});

//#endregion ==================================================================

//#region SKINNING ===========================================================

group( 'skinning', ()=>{
    //-------------------------------------------------------------
    // Non-identity bind inverse: the armature has a rotated/translated rest
    // pose so bind matrices are not identities. After solving, SkinMTX must
    // drive a real vertex onto the analytically expected deformed position.
    {
        const arm = new Armature();

        // Rest pose: chain along +Y, armature offset places it away from origin.
        arm.addBone( 'base', -1, undefined, [0,0,0] );
        arm.addBone( 'j1', 0, quat.fromValues( 0,0,0,1 ), [0,2,0] );
        arm.addBone( 'j2', 1, quat.fromValues( 0,0,0,1 ), [0,1,0] );
        arm.addBone( 'j3', 2, quat.fromValues( 0,0,0,1 ), [0,1,0] );
        arm.bind( SkinMTX );

        // Non-identity bind inverse: rotate the whole rest skeleton +90deg
        // about Z and shift it, so world(rest) != local(rest).
        arm.offset.rot = quat.setAxisAngle( quat.create(), [0,0,1], Math.PI / 2 );
        arm.offset.pos = vec3.fromValues( 10, -3, 2 );
        arm.bind( SkinMTX );

        // Rebuild the chain from the *armature bind data* (bone local).
        const chain = new IKChain( [ 'j1', 'j2', 'j3' ], arm );

        // Guides are root bones, so their world position is offset * local.
        const guides: number[] = [];
        for( let i=0; i < 4; i++ ) guides.push( arm.addBone( `sg${i}`, -1, undefined, [0,0,0] ).idx );
        // Rebind so the skin knows about the guide bones (bind data of the
        // chain bones is unchanged: bind() never rewrites bone.local).
        arm.bind( SkinMTX );
        const pose = arm.newPose( true );

        // +90° Z offset maps local (0,2,0) to world (10,-3,2) + (-2,0,0).
        const rootW = pose.bones[1].world.pos;
        check( Math.abs( rootW[0] - 8 ) <= 1e-5 &&
               Math.abs( rootW[1] + 3 ) <= 1e-5 &&
               Math.abs( rootW[2] - 2 ) <= 1e-5,
            `rest world root from bind+offset (${rootW[0]},${rootW[1]},${rootW[2]})` );

        // Desired world guide points: straight +X line from the root.
        const gWorld: V3[] = [
            [rootW[0], rootW[1], rootW[2]],
            [rootW[0] + 1, rootW[1], rootW[2]],
            [rootW[0] + 2.5, rootW[1], rootW[2]],
            [rootW[0] + 4, rootW[1], rootW[2]],
        ];
        for( let i=0; i < 4; i++ ){
            const l = worldToRootLocal( arm.offset, gWorld[i] );
            pose.bones[ guides[i] ].local.pos.set( l );
        }
        pose.updateWorld();

        const tr  = new CurveChainTrack( chain, guides, { positionTolerance: 1e-5 } );
        const res = new CurveChainSolver().resolve( tr.capture( pose ), pose );

        // Bone lengths are exact in world after solving.
        for( let i=0; i < 3; i++ ){
            const d = vlen( res.points[i], res.points[i+1] );
            check( Math.abs( d - 1 ) <= 1e-4, `skinning link ${i} length ${d}` );
        }
        // First bone origin is still the bind root position.
        check( vlen( res.points[0], [rootW[0], rootW[1], rootW[2]] ) <= 1e-5,
            'skinning root stays at sampled position' );

        // Skin a vertex that is rigidly bound to bone j3 with weight 1.
        // Vertex rest position = a point 0.5 along j3 at rest, in world space.
        const offsets = arm.updateSkinFromPose( pose ) as Float32Array[];
        check( offsets && offsets.length === 1, 'skin offsets available' );

        // Analytic expectation: the skinning matrix for a bone maps
        // rest-world -> posed-world. Build rest world for j3 from the
        // armature bind data and compare against the pose world.
        const restWorld = restWorldMatrix( arm, 3 );
        const posWorld  = pose.bones[3].world;

        const vLocal: V3 = [0, 0.5, 0];                 // in j3 local space
        const expect  = transformPoint( posWorld, vLocal );
        const actual  = applySkinMatrix( offsets[0], 3, transformPoint( restWorld, vLocal ) );

        for( let k=0; k < 3; k++ ){
            check( Math.abs( actual[k] - expect[k] ) <= 1e-4,
                `skinned vertex component ${k}: ${actual[k]} vs ${expect[k]}` );
        }
        check( Math.abs( expect[1] - res.points[2][1] - 0.5 * Math.sin( Math.PI / 2 ) ) >= 0,
            'analytic expectation is non-degenerate' );

        // The bind inverse is genuinely non-identity (not the identity case).
        const m = offsets[0].slice( 3 * 16, 3 * 16 + 16 ) as unknown as number[];
        check( Math.abs( m[12] ) + Math.abs( m[13] ) + Math.abs( m[14] ) > 1e-3,
            'bind matrix inverse is non-identity' );
    }

    //-------------------------------------------------------------
    // Skin a mesh vertex through all chain bones: deformation drives the
    // real offset buffer after the solver refreshes world transforms.
    {
        const arm = new Armature();
        arm.addBone( 'base', -1, undefined, [0,0,0] );
        arm.addBone( 'a', 0, undefined, [0,1,0] );
        arm.addBone( 'b', 1, undefined, [0,1,0] );
        arm.addBone( 'c', 2, undefined, [0,1,0] );
        arm.bind( SkinMTX );

        const chain = new IKChain( [ 'a', 'b', 'c' ], arm );
        const guideBones = addGuideBones( arm, 'wg', [
            [0,0,0], [0,1,0], [0,2,0], [0,3,0],
        ] );
        // Rebind so SkinMTX knows about every bone (chain bind data unchanged).
        arm.bind( SkinMTX );
        const pose  = arm.newPose( true );
        const rootW = pose.bones[1].world.pos;

        // A curve that bends the chain into a quarter turn: starts +Y and
        // ends toward +X. Guides are free roots, so set locals directly.
        const pts: V3[] = [
            [rootW[0], rootW[1], rootW[2]],
            [rootW[0], rootW[1]+1, rootW[2]],
            [rootW[0]+1, rootW[1]+2.2, rootW[2]],
            [rootW[0]+2.2, rootW[1]+2.2, rootW[2]],
        ];
        for( let i=0; i < 4; i++ ) pose.bones[ guideBones[i] ].local.pos.set( pts[i] );
        pose.updateWorld();

        const tr = new CurveChainTrack( chain, guideBones, { positionTolerance: 1e-5 } );
        const res = new CurveChainSolver().resolve( tr.capture( pose ), pose );
        arm.updateSkinFromPose( pose );
        const offs = arm.getSkinOffsets() as Float32Array[];

        // Every solved world point must match pose world positions.
        for( let i=0; i < 3; i++ ){
            const w = pose.bones[1+i].world.pos;
            check( vlen( [w[0],w[1],w[2]], res.points[i] ) <= 1e-5,
                `world refreshed for bone ${i}` );
        }
        // Tail = last bone origin + world +Y * length.
        const lastB = pose.bones[3].world;
        const tail = transformPoint( lastB, [0,1,0] );
        check( vlen( tail, res.points[3] ) <= 1e-5, 'chain tail matches solved point' );

        // Skinned point on the last bone matches pose transform.
        const v: V3 = [0, 1, 0];
        const expect = transformPoint( lastB, v );
        const got = applySkinMatrix( offs[0], 3, transformPoint( restWorldMatrix( arm, 3 ), v ) );
        for( let k=0; k < 3; k++ ){
            check( Math.abs( got[k] - expect[k] ) <= 1e-4, `mesh vertex ${k} matches skinning` );
        }
    }
});

//#endregion ==================================================================

//#region HELPERS ============================================================

function snapshotPose( pose: any ){
    const n = pose.bones.length;
    const out: number[] = [];
    for( let i=0; i < n; i++ ){
        const b = pose.bones[ i ];
        out.push( ...Array.from( b.local.pos ), ...Array.from( b.local.rot ), ...Array.from( b.local.scl ) );
        out.push( ...Array.from( b.world.pos ), ...Array.from( b.world.rot ), ...Array.from( b.world.scl ) );
    }
    out.push( ...Array.from( pose.offset.pos ), ...Array.from( pose.offset.rot ), ...Array.from( pose.offset.scl ) );
    return out;
}

function checkSamePose( before: number[], pose: any, label: string ){
    const after = snapshotPose( pose );
    check( before.length === after.length, `${label}: length` );
    for( let i=0; i < before.length; i++ ){
        if( Math.abs( before[i] - after[i] ) > 1e-6 ){
            check( false, `${label}: value ${i} changed ${before[i]} -> ${after[i]}` );
        }
    }
}

function poseLocals( pose: any ): number[]{
    const out: number[] = [];
    for( const b of pose.bones ) out.push( ...Array.from( b.local.pos ), ...Array.from( b.local.rot ) );
    return out;
}

/**
 * Recompute the rest world matrix a bone has in the SkinMTX bind pose.
 *
 * SkinMTX.init() builds bind matrices from the armature bone locals without
 * applying `arm.offset` (the offset is applied later, at pose time), so this
 * reference mirrors that exact convention.
 */
function restWorldMatrix( arm: any, idx: number ): any {
    const b = arm.bones[ idx ];
    const local = { rot: b.local.rot, pos: b.local.pos, scl: b.local.scl };
    if( b.pidx === -1 ) return {
        rot: [ b.local.rot[0], b.local.rot[1], b.local.rot[2], b.local.rot[3] ],
        pos: [ b.local.pos[0], b.local.pos[1], b.local.pos[2] ],
        scl: [ b.local.scl[0], b.local.scl[1], b.local.scl[2] ],
    };
    return composeT( restWorldMatrix( arm, b.pidx ), local );
}

function composeT( p: any, c: any ){
    const r = quat.mul( quat.create(), p.rot, c.rot );
    const v = vec3.mul( vec3.create(), p.scl, c.pos );
    vec3.transformQuat( v, v, p.rot );
    vec3.add( v, v, p.pos );
    return {
        rot: r,
        pos: v,
        scl: vec3.mul( vec3.create(), p.scl, c.scl ),
    };
}

function transformPoint( t: any, v: V3 ): V3 {
    const tmp = vec3.mul( vec3.create(), v as never, t.scl );
    vec3.transformQuat( tmp, tmp, t.rot );
    vec3.add( tmp, tmp, t.pos );
    return [ tmp[0], tmp[1], tmp[2] ];
}

/** Invert a rigid Transform (unit scale) and apply it to a world point. */
function worldToRootLocal( t: any, p: V3 ): V3{
    const q = quat.invert( quat.create(), t.rot );
    const v = vec3.sub( vec3.create(), p as never, t.pos );
    vec3.transformQuat( v, v, q );
    return [ v[0], v[1], v[2] ];
}

/** Apply offsetBuffer[idx] to a rest-world point. */
function applySkinMatrix( buf: Float32Array, idx: number, p: V3 ): V3{
    const o = idx * 16;
    const x = p[0], y = p[1], z = p[2];
    // Column-major mat4.
    return [
        buf[o+0]*x + buf[o+4]*y + buf[o+8]*z  + buf[o+12],
        buf[o+1]*x + buf[o+5]*y + buf[o+9]*z  + buf[o+13],
        buf[o+2]*x + buf[o+6]*y + buf[o+10]*z + buf[o+14],
    ];
}

//#endregion
