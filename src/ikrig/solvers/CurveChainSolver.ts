/* eslint-disable @typescript-eslint/no-explicit-any */
//#region IMPORTS
import type Pose                 from '../../armature/Pose';
import type Armature             from '../../armature/Armature';
import type { vec3, quat }       from 'gl-matrix';
import type { CurveChainCapture } from '../../animation/tracks/CurveChainTrack';
import type CurveChainTrack      from '../../animation/tracks/CurveChainTrack';

import { vec3 as V3, quat as Q, mat3 as M3 } from 'gl-matrix';
import { CURVE_CHAIN_ERRORS, CurveChainError } from '../../animation/tracks/CurveChainTrack';
//#endregion

//#region TYPES
export interface CurveChainSolveOptions{
    /** Overrides the track's world-space position tolerance. */
    positionTolerance ?: number;
    /** Overrides the track's finite root-isolation budget. */
    workBudget        ?: number;
}

/** Work actually performed while solving one channel. */
export interface CurveChainWorkStats{
    /** Finite budget that was granted. */
    budget                  : number;
    /** Budget units consumed. */
    used                    : number;
    /** Polynomial (curve / derivative) evaluations. */
    polynomialEvaluations   : number;
    /** Bezier subdivision nodes visited during root isolation. */
    subdivisions            : number;
    /** Bisection steps used to refine a root. */
    bisections              : number;
    /** Number of cubic segments in the guide curve. */
    segments                : number;
    /** Number of chain links (bone origins plus tail). */
    links                   : number;
}

/** Everything the caller may want to inspect after a successful solve. */
export interface CurveChainSolveResult{
    track       : CurveChainTrack;
    /** Index of every chain bone, root first. */
    bones       : number[];
    /** Solved world points: one origin per bone followed by the chain tail. */
    points      : vec3[];
    /** Global curve parameter for each point in `points`. */
    parameters  : number[];
    /** Solved world rotation for every chain bone. */
    frames      : quat[];
    work        : CurveChainWorkStats;
}
//#endregion

//#region ERROR HELPERS
function fail( code: string, msg: string ): never{
    throw new CurveChainError( code, msg );
}
//#endregion

//#region POLYNOMIAL UTILITIES
/** Ascending power-basis coefficients: c[0] + c[1]*x + ... */
type Poly = number[];

function polyEval( c: Poly, x: number ): number{
    let v = 0;
    for( let i=c.length-1; i >= 0; i-- ) v = v * x + c[ i ];
    return v;
}


//#endregion

//#region WORK METER
class WorkMeter{
    budget : number;
    used   = 0;
    polynomialEvaluations = 0;
    subdivisions          = 0;
    bisections            = 0;

    constructor( budget: number ){
        this.budget = budget;
    }

    tick( n = 1 ): void{
        this.used += n;
        if( this.used > this.budget )
            fail( CURVE_CHAIN_ERRORS.WORK_BUDGET_EXCEEDED,
                `curve chain work budget of ${this.budget} exhausted` );
    }

    evalPoly( c: Poly, x: number ): number{
        this.tick();
        this.polynomialEvaluations++;
        return polyEval( c, x );
    }

    evalGeom(): void{
        this.tick();
        this.polynomialEvaluations++;
    }

    subdivide(): void{
        this.tick();
        this.subdivisions++;
    }

    bisect(): void{
        this.tick();
        this.bisections++;
    }
}
//#endregion

//#region GEOMETRY HELPERS
type Segment = [ vec3, vec3, vec3, vec3 ];

function bezierAt( c: Segment, t: number ): vec3{
    const mt = 1 - t;
    const a  = mt * mt * mt;
    const b  = 3 * mt * mt * t;
    const cc = 3 * mt * t * t;
    const d  = t * t * t;
    return [
        a * c[0][0] + b * c[1][0] + cc * c[2][0] + d * c[3][0],
        a * c[0][1] + b * c[1][1] + cc * c[2][1] + d * c[3][1],
        a * c[0][2] + b * c[1][2] + cc * c[2][2] + d * c[3][2],
    ];
}

function distSqr( a: vec3, b: vec3 ): number{
    const x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x*x + y*y + z*z;
}

function dist( a: vec3, b: vec3 ): number{
    return Math.sqrt( distSqr( a, b ) );
}

//#endregion

/**
 * Solves a single curve-chain channel captured by CurveChainTrack.
 *
 * ## Geometry
 *
 * The 3m+1 guide points form m cubic Bezier segments. Starting from the sampled
 * chain root, the first bone origin is placed at the *first* curve parameter
 * whose point is exactly one bind link length away from the root. The search
 * then continues from that point for the next link, and so on. The final
 * intersection is the chain tail. Segment/parameter order is strictly ascending
 * and shared segment endpoints are only examined once.
 *
 * ## Root isolation
 *
 * The sphere/curve intersection is solved as the real roots of a degree-6
 * polynomial in the segment parameter. Root isolation is derivative based: the
 * derivative's roots split the interval into monotonic pieces, so every simple
 * root is found by bisection (never a fixed-step scan) and every tangency
 * (even-multiplicity root) is recovered as a derivative root whose original
 * polynomial value is zero. Unit-multiplicity roots run to machine precision.
 *
 * ## Orientation
 *
 * Each bone's world +Y points at the next solved point. The first frame takes
 * its +Z from the pre-solve sampled world +Z of the chain root (orthogonalised
 * against +Y, falling back to the least-parallel world axis, ties X then Y then
 * Z). Later frames transport the previous +Z by the minimal rotation between
 * consecutive +Y directions; exact reversals use the previous +Z as the axis of
 * a half turn. X = Y x Z completes a right-handed orthonormal basis.
 *
 * ## Failure
 *
 * Every failure throws CurveChainError with one of CURVE_CHAIN_ERRORS and the
 * chain is never stretched, partially solved or left half-written. The Animator
 * is responsible for restoring the whole Pose if any channel fails.
 */
class CurveChainSolver{
    positionTolerance ?: number;
    workBudget        ?: number;

    constructor( options: CurveChainSolveOptions = {} ){
        this.positionTolerance = options.positionTolerance;
        this.workBudget        = options.workBudget;
    }

    /**
     * Convenience entry point: validate + capture the track, then resolve it.
     * The Pose must already have up-to-date world transforms.
     */
    static solve( track: CurveChainTrack, pose: Pose, options: CurveChainSolveOptions = {} ): CurveChainSolveResult{
        const capture = track.capture( pose );
        return new CurveChainSolver( options ).resolve( capture, pose );
    }

    /**
     * Solve one frozen capture into `pose`.
     *
     * @param capture Result of CurveChainTrack.capture() taken after the clip's
     *                ordinary tracks have run.
     * @param pose    Pose to write chain local rotations / translations into.
     * @returns Solved points, parameters, frames and work statistics.
     */
    resolve( capture: CurveChainCapture, pose: Pose ): CurveChainSolveResult{
        const track = capture.track;
        const poseArm: Armature | undefined = pose ? pose.arm : undefined;
        if( !poseArm ) fail( CURVE_CHAIN_ERRORS.INVALID_ARMATURE, 'Pose has no Armature' );

        // Full structural validation (also done by capture; repeated here so a
        // hand-built capture or a direct solver call cannot bypass it).
        track.validate( pose );

        const tol = ( this.positionTolerance !== undefined ) ? this.positionTolerance : track.positionTolerance;
        if( typeof tol !== 'number' || !Number.isFinite( tol ) || tol <= 0 )
            fail( CURVE_CHAIN_ERRORS.INVALID_TOLERANCE, `positionTolerance must be finite and > 0, got ${tol}` );

        const budget = ( this.workBudget !== undefined ) ? this.workBudget : track.workBudget;
        if( typeof budget !== 'number' || !Number.isFinite( budget ) || budget <= 0 )
            fail( CURVE_CHAIN_ERRORS.INVALID_BUDGET, `workBudget must be finite and > 0, got ${budget}` );

        const links = track.chain.links;
        if( !links || links.length === 0 )
            fail( CURVE_CHAIN_ERRORS.INVALID_CHAIN, 'chain must contain at least one link' );

        //-----------------------------------------------------------
        // First guide point must coincide with the sampled chain root.
        const rootPos: vec3 = [
            capture.rootPos[0], capture.rootPos[1], capture.rootPos[2],
        ];
        const firstGuide = capture.guidePoints[ 0 ];
        if( !firstGuide ) fail( CURVE_CHAIN_ERRORS.INVALID_GUIDE_COUNT, 'guidePoints is empty' );

        const d0 = dist( rootPos, firstGuide );
        if( !Number.isFinite( d0 ) || d0 > tol )
            fail( CURVE_CHAIN_ERRORS.FIRST_GUIDE_MISMATCH,
                `guide point 0 is ${d0} from the chain root, tolerance is ${tol}` );

        //-----------------------------------------------------------
        // Build Bezier segments from the frozen guide points.
        const gp = capture.guidePoints;
        const segCount = ( gp.length - 1 ) / 3;
        if( segCount < 1 || !Number.isInteger( segCount ) )
            fail( CURVE_CHAIN_ERRORS.INVALID_GUIDE_COUNT, 'guide point count must be 3m+1' );

        const segments: Segment[] = new Array( segCount );
        for( let s=0; s < segCount; s++ ){
            segments[ s ] = [
                [ gp[ s*3   ][0], gp[ s*3   ][1], gp[ s*3   ][2] ],
                [ gp[ s*3+1 ][0], gp[ s*3+1 ][1], gp[ s*3+1 ][2] ],
                [ gp[ s*3+2 ][0], gp[ s*3+2 ][1], gp[ s*3+2 ][2] ],
                [ gp[ s*3+3 ][0], gp[ s*3+3 ][1], gp[ s*3+3 ][2] ],
            ];
        }

        const meter = new WorkMeter( budget );

        //-----------------------------------------------------------
        // Walk the curve, one link at a time.
        const points: vec3[] = [ rootPos ];
        const parameters: number[] = [ 0 ];
        let prevT = 0;
        let cur   = rootPos;

        // Curve domain is [0, segCount]. `_findNext` restricts the search to
        // parameters strictly greater than the start, so shared segment
        // endpoints are only examined once.
        for( let li=0; li < links.length; li++ ){
            const r = links[ li ].len;
            if( !Number.isFinite( r ) || r <= 0 )
                fail( CURVE_CHAIN_ERRORS.INVALID_LINK_LENGTH, `link ${li} length must be finite and > 0` );

            const hit = this._findNext( segments, cur, r, prevT, tol, meter );
            if( !hit )
                fail( CURVE_CHAIN_ERRORS.CURVE_TOO_SHORT,
                    `curve ran out before link ${li} (length ${r}) could be placed` );

            points.push( hit.point );
            parameters.push( hit.t );
            cur   = hit.point;
            prevT = hit.t;
        }

        //-----------------------------------------------------------
        // Build world frames.
        const frames: quat[] = new Array( links.length );
        const rootZ: vec3 = [ capture.rootZ[0], capture.rootZ[1], capture.rootZ[2] ];
        let prevY: vec3 | null = null;
        let prevZ: vec3 | null = null;

        for( let i=0; i < links.length; i++ ){
            const dir: vec3 = [
                points[ i+1 ][0] - points[ i ][0],
                points[ i+1 ][1] - points[ i ][1],
                points[ i+1 ][2] - points[ i ][2],
            ];
            const dl = Math.hypot( dir[0], dir[1], dir[2] );
            if( dl <= 1e-15 )
                fail( CURVE_CHAIN_ERRORS.DEGENERATE_CURVE, `chain segment ${i} has zero direction` );
            const y: vec3 = [ dir[0]/dl, dir[1]/dl, dir[2]/dl ];

            let z: vec3;
            if( i === 0 ){
                z = this._seedZ( y, rootZ );
            }else{
                z = this._transportZ( prevY as vec3, prevZ as vec3, y );
            }

            const frame = this._orthoFrame( y, z );
            frames[ i ] = frame.q;
            prevY = y;
            prevZ = frame.z;
        }

        //-----------------------------------------------------------
        // Convert world frames to local transforms, preserving the root
        // displacement and every bind segment length.
        const rootLnk = links[ 0 ];
        const parentRot: quat = [
            capture.parentRot[0], capture.parentRot[1], capture.parentRot[2], capture.parentRot[3],
        ];

        const invParent: quat = [ 0, 0, 0, 1 ];
        const localQ: quat    = [ 0, 0, 0, 1 ];

        for( let i=0; i < links.length; i++ ){
            const lnk = links[ i ];
            const b   = pose.bones[ lnk.idx ];
            if( !b ) fail( CURVE_CHAIN_ERRORS.INVALID_BONE_INDEX, `pose bone ${lnk.idx} is missing` );

            if( i === 0 ){
                // Root rotation relative to its unchanged parent; position untouched.
                Q.conjugate( invParent, parentRot as any );
                Q.normalize( invParent, invParent );
                Q.mul( localQ, invParent, frames[ i ] as any );
                Q.normalize( localQ, localQ );
            }else{
                Q.conjugate( invParent, frames[ i - 1 ] as any );
                Q.normalize( invParent, invParent );
                Q.mul( localQ, invParent, frames[ i ] as any );
                Q.normalize( localQ, localQ );

                // Child bind displacement is along the parent's +Y by the
                // parent link length. Copying the bind transform verbatim keeps
                // the exact bind segment length and is equivalent to
                // (0, len, 0) (validated to be along +Y already).
                V3.copy( b.local.pos, lnk.bind.pos );
            }

            Q.copy( b.local.rot, localQ );
        }

        //-----------------------------------------------------------
        // Refresh the whole pose world so skinning can consume it directly.
        pose.updateWorld();

        const work: CurveChainWorkStats = {
            budget                : budget,
            used                  : meter.used,
            polynomialEvaluations : meter.polynomialEvaluations,
            subdivisions          : meter.subdivisions,
            bisections            : meter.bisections,
            segments              : segCount,
            links                 : links.length,
        };

        return {
            track       : track,
            bones       : links.map( l => l.idx ),
            points      : points.map( p => [ p[0], p[1], p[2] ] ),
            parameters  : parameters,
            frames      : frames.map( q => [ q[0], q[1], q[2], q[3] ] ),
            work,
        };
    }

    //#region INTERNAL : SEARCH
    /** Find the first curve point at distance `r` from `center`, after `tStart`. */
    _findNext( segments: Segment[], center: vec3, r: number, tStart: number, tol: number, meter: WorkMeter ):
        { point: vec3, t: number } | null
    {
        const m = segments.length;
        if( tStart >= m - 1e-12 ) return null;      // Already at (or past) the curve end.

        const firstSeg  = Math.max( 0, Math.floor( tStart ) );
        const local0    = Math.max( 0, tStart - firstSeg );

        for( let seg=firstSeg; seg < m; seg++ ){
            const start = ( seg === firstSeg ) ? local0 : 0;
            const tau = this._firstOnSegment( segments[ seg ], center, r, start, tol, meter );
            if( tau !== null ){
                return {
                    point : bezierAt( segments[ seg ], tau ),
                    t     : seg + tau,
                };
            }
        }
        return null;
    }

    /** Earliest local parameter in [start, 1] where |Bezier(t) - center| == r (within tol). */
    _firstOnSegment( ctrl: Segment, center: vec3, r: number, start: number, tol: number, meter: WorkMeter ): number | null{
        const gTol = 2 * r * tol + tol * tol + 1e-12 * Math.max( 1, r * r, distSqr( center, [0,0,0] ) );

        const gAt = ( t: number ): number=>{
            meter.evalGeom();
            const p = bezierAt( ctrl, t );
            return distSqr( p, center ) - r * r;
        };

        // Squared-distance polynomial in the segment parameter.
        // |P(t) - center|^2 - r^2 = e + c t + b t^2 + a t^3, squared.
        const q0 = ctrl[0], q1 = ctrl[1], q2 = ctrl[2], q3 = ctrl[3];
        const a: vec3 = [ -q0[0]+3*q1[0]-3*q2[0]+q3[0], -q0[1]+3*q1[1]-3*q2[1]+q3[1], -q0[2]+3*q1[2]-3*q2[2]+q3[2] ];
        const b: vec3 = [ 3*q0[0]-6*q1[0]+3*q2[0], 3*q0[1]-6*q1[1]+3*q2[1], 3*q0[2]-6*q1[2]+3*q2[2] ];
        const cc: vec3 = [ -3*q0[0]+3*q1[0], -3*q0[1]+3*q1[1], -3*q0[2]+3*q1[2] ];
        const d: vec3 = [ q0[0]-center[0], q0[1]-center[1], q0[2]-center[2] ];

        const dot = ( u: vec3, v: vec3 ): number => u[0]*v[0] + u[1]*v[1] + u[2]*v[2];
        const g: Poly = [
            dot( d, d ) - r * r,
            2 * dot( cc, d ),
            dot( cc, cc ) + 2 * dot( b, d ),
            2 * dot( a, d ) + 2 * dot( b, cc ),
            dot( b, b ) + 2 * dot( a, cc ),
            2 * dot( a, b ),
            dot( a, a ),
        ];

        // Critical points of g split [start,1] into monotonic pieces. Inside a
        // monotonic piece a root is simple, so bisection finds it; a tangency
        // (even multiplicity) is a critical point whose value is zero for the
        // *original* g, so it is recovered by evaluating the critical points.
        const dg: Poly = [ g[1], 2*g[2], 3*g[3], 4*g[4], 5*g[5], 6*g[6] ];
        const crit: number[] = [];
        this._allPolyRoots( dg, start, 1, crit, meter, 0 );

        crit.sort( ( x, y ) => x - y );
        const knots: number[] = [];
        for( let t of crit ){
            if( t <= start ) continue;
            if( t >= 1 ) continue;
            if( knots.length && t - knots[ knots.length - 1 ] < 1e-13 ) continue;
            knots.push( t );
        }

        let best: number | null = null;
        const better = ( t: number ): void=>{
            if( best === null || t < best ) best = t;
        };
        const acceptIfClose = ( t: number, gt: number ): void=>{
            if( Math.abs( gt ) <= gTol ) better( t );
        };

        // Endpoints and tangency candidates.
        const bounds: number[] = [ start, ...knots, 1 ];
        const vals  : number[] = bounds.map( ( t )=> gAt( t ) );
        for( let i=0; i < bounds.length; i++ ) acceptIfClose( bounds[ i ], vals[ i ] );

        // Genuine sign changes inside each monotonic piece. A bracketed sign
        // change is a real crossing by construction; bisection drives it to
        // machine precision regardless of the absolute polynomial scale.
        for( let i=0; i + 1 < bounds.length; i++ ){
            const a = bounds[ i ], b = bounds[ i + 1 ];
            const ga = vals[ i ], gb = vals[ i + 1 ];
            if( ga * gb < 0 ){
                const t = this._bisectG( gAt, a, b, ga, gb, gTol, meter );
                better( t );
            }
        }

        if( best === null ) return null;
        return Math.min( 1, Math.max( start, best ) );
    }

    /**
     * All real roots of `power` strictly inside (a,b), using derivative-based
     * root isolation. Each call recursively isolates the roots of the
     * derivative, which split the domain into intervals where the polynomial is
     * monotonic. A root in such an interval is simple, so bisection is exact to
     * machine precision; even-multiplicity tangencies are exactly the critical
     * points where the polynomial itself vanishes, so they are not skipped.
     */
    _allPolyRoots( power: Poly, a: number, b: number, out: number[], meter: WorkMeter, depth: number ): void{
        if( depth > 16 ) return;

        // Drop numerically-zero leading coefficients.
        let deg = power.length - 1;
        let scale = 1;
        for( const v of power ) scale = Math.max( scale, Math.abs( v ) );
        const leadEps = 1e-14 * scale;
        while( deg > 0 && Math.abs( power[ deg ] ) <= leadEps ) deg--;
        if( deg <= 0 ) return;

        const p: Poly = power.slice( 0, deg + 1 );

        if( deg === 1 ){
            const r = -p[ 0 ] / p[ 1 ];
            if( Number.isFinite( r ) && r > a && r < b ) out.push( r );
            return;
        }

        // Roots of the derivative are the critical points of p.
        const dp: Poly = new Array( deg );
        for( let i=1; i <= deg; i++ ) dp[ i - 1 ] = p[ i ] * i;

        const crit: number[] = [];
        this._allPolyRoots( dp, a, b, crit, meter, depth + 1 );
        crit.sort( ( x, y ) => x - y );

        const knots: number[] = [];
        for( const t of crit ){
            if( t <= a || t >= b ) continue;
            if( knots.length && t - knots[ knots.length - 1 ] < 1e-13 ) continue;
            knots.push( t );
        }

        const bounds: number[] = [ a, ...knots, b ];
        const vals  : number[] = bounds.map( ( t )=> meter.evalPoly( p, t ) );
        const tol   = this._polyTol( p );

        // Tangency / touch roots: a critical point where p itself is zero.
        for( let i=1; i + 1 < bounds.length; i++ ){
            if( Math.abs( vals[ i ] ) <= tol ) out.push( bounds[ i ] );
        }

        // Simple roots between consecutive critical points.
        for( let i=0; i + 1 < bounds.length; i++ ){
            if( vals[ i ] * vals[ i + 1 ] < 0 ){
                out.push( this._bisectPolyVal( p, bounds[ i ], bounds[ i + 1 ], vals[ i ], meter ) );
            }
        }
    }

    /** Absolute tolerance for calling a polynomial value zero. */
    _polyTol( c: Poly ): number{
        let scale = 1;
        for( const v of c ) scale = Math.max( scale, Math.abs( v ) );
        return 1e-12 * scale;
    }

    /** Bisect a sign-changing bracket of a power-basis polynomial. */
    _bisectPolyVal( c: Poly, lo: number, hi: number, flo: number, meter: WorkMeter ): number{
        for( let i=0; i < 200; i++ ){
            meter.bisect();
            const mid = 0.5 * ( lo + hi );
            if( ( hi - lo ) <= 1e-15 ) return mid;
            const fm = meter.evalPoly( c, mid );
            if( fm === 0 ) return mid;
            if( ( fm < 0 ) === ( flo < 0 ) ){ lo = mid; flo = fm; }
            else                           { hi = mid; }
        }
        return 0.5 * ( lo + hi );
    }

    /** Bisect a sign-changing bracket of g, returning the root to machine precision. */
    _bisectG(
        gAt: ( t: number ) => number,
        lo: number, hi: number,
        glo: number, _ghi: number,
        gTol: number, meter: WorkMeter,
    ): number{
        let best = 0.5 * ( lo + hi );
        let bestAbs = Infinity;

        for( let i=0; i < 200; i++ ){
            meter.bisect();
            const mid = 0.5 * ( lo + hi );
            const gm = gAt( mid );

            const abs = Math.abs( gm );
            if( abs < bestAbs ){ bestAbs = abs; best = mid; }
            if( gm === 0 || abs <= gTol * 1e-3 ) return mid;
            if( ( hi - lo ) <= 1e-15 ) return mid;

            if( ( gm < 0 ) === ( glo < 0 ) ){ lo = mid; glo = gm; }
            else                           { hi = mid; }
        }
        return best;
    }

    //#endregion

    //#region INTERNAL : FRAMES
    /** Seed +Z for the first frame, orthonormalising the pre-solve world +Z against +Y. */
    _seedZ( y: vec3, rootZ: vec3 ): vec3{
        const dot = y[0]*rootZ[0] + y[1]*rootZ[1] + y[2]*rootZ[2];
        let z: vec3 = [ rootZ[0] - dot*y[0], rootZ[1] - dot*y[1], rootZ[2] - dot*y[2] ];
        let len = Math.hypot( z[0], z[1], z[2] );

        if( len <= 1e-8 ){
            // Degenerate seed: least-parallel world axis, ties X then Y then Z.
            const axes: vec3[] = [ [1,0,0], [0,1,0], [0,0,1] ];
            let best = Infinity;
            let axis: vec3 = axes[ 0 ];
            for( const ax of axes ){
                const dd = Math.abs( y[0]*ax[0] + y[1]*ax[1] + y[2]*ax[2] );
                if( dd < best ){ best = dd; axis = ax; }
            }
            const da = y[0]*axis[0] + y[1]*axis[1] + y[2]*axis[2];
            z = [ axis[0] - da*y[0], axis[1] - da*y[1], axis[2] - da*y[2] ];
            len = Math.hypot( z[0], z[1], z[2] );
        }

        if( len <= 1e-15 ) fail( CURVE_CHAIN_ERRORS.DEGENERATE_CURVE, 'unable to seed a tangent frame' );
        return [ z[0]/len, z[1]/len, z[2]/len ];
    }

    /** Transport +Z by the minimal rotation between consecutive +Y directions. */
    _transportZ( prevY: vec3, prevZ: vec3, curY: vec3 ): vec3{
        const dot = Math.max( -1, Math.min( 1, prevY[0]*curY[0] + prevY[1]*curY[1] + prevY[2]*curY[2] ) );

        if( dot >= 1 - 1e-12 ) return [ prevZ[0], prevZ[1], prevZ[2] ];

        if( dot <= -1 + 1e-12 ){
            // Exact reversal: half turn about the previous +Z leaves +Z unchanged.
            return [ prevZ[0], prevZ[1], prevZ[2] ];
        }

        const q: quat = [ 0, 0, 0, 1 ];
        Q.rotationTo( q, prevY as any, curY as any );
        const z: vec3 = [ 0, 0, 0 ];
        V3.transformQuat( z, prevZ as any, q );

        const zd = z[0]*curY[0] + z[1]*curY[1] + z[2]*curY[2];
        z[0] -= zd * curY[0];
        z[1] -= zd * curY[1];
        z[2] -= zd * curY[2];

        const l = Math.hypot( z[0], z[1], z[2] );
        if( l <= 1e-15 ) return [ prevZ[0], prevZ[1], prevZ[2] ];
        return [ z[0]/l, z[1]/l, z[2]/l ];
    }

    _orthoFrame( y: vec3, z: vec3 ): { z: vec3, q: quat }{
        // X = Y x Z, then re-derive Z = X x Y for exact right-handedness.
        let x: vec3 = [
            y[1]*z[2] - y[2]*z[1],
            y[2]*z[0] - y[0]*z[2],
            y[0]*z[1] - y[1]*z[0],
        ];
        let xl = Math.hypot( x[0], x[1], x[2] );
        if( xl <= 1e-15 ) fail( CURVE_CHAIN_ERRORS.DEGENERATE_CURVE, 'unable to build a tangent basis' );
        x = [ x[0]/xl, x[1]/xl, x[2]/xl ];

        const zz: vec3 = [
            x[1]*y[2] - x[2]*y[1],
            x[2]*y[0] - x[0]*y[2],
            x[0]*y[1] - x[1]*y[0],
        ];

        // gl-matrix mat3 is column-major: columns are X, Y, Z basis vectors.
        const m = M3.set( M3.create(),
            x[0],  x[1],  x[2],
            y[0],  y[1],  y[2],
            zz[0], zz[1], zz[2],
        );

        const q: quat = [ 0, 0, 0, 1 ];
        Q.fromMat3( q, m );
        Q.normalize( q, q );

        return { z: zz, q };
    }
    //#endregion
}

export { CurveChainSolver, CurveChainError, CURVE_CHAIN_ERRORS };
export default CurveChainSolver;
