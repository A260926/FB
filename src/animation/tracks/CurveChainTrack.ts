/* eslint-disable @typescript-eslint/no-explicit-any */
//#region IMPORTS
import type { IKChain, IKLink } from '../../ikrig/rigs/IKChain';
import type Pose                 from '../../armature/Pose';
import type Armature             from '../../armature/Armature';
import type { vec3 }             from 'gl-matrix';
//#endregion

/**
 * Error codes used by CurveChainTrack / CurveChainSolver.
 *
 * These are stable strings so a caller can tell *why* an applyPose failed:
 *  - INVALID_ARMATURE        Pose has no source armature.
 *  - INVALID_CHAIN           Empty / malformed chain object.
 *  - INVALID_BONE_INDEX      A chain or guide bone index is not a valid skeleton index.
 *  - NON_CONTIGUOUS_CHAIN    Chain links are not a straight parent -> child path.
 *  - INVALID_GUIDE_COUNT     Guide bone count is not 3m+1 (with m >= 1).
 *  - INVALID_LINK_LENGTH     A link length is not finite and strictly positive.
 *  - INVALID_TOLERANCE       positionTolerance is not finite and strictly positive.
 *  - INVALID_BUDGET          workBudget is not finite and strictly positive.
 *  - NON_FINITE_VALUE        A transform / bind / sampled value is NaN or Infinity.
 *  - UNSUPPORTED_SCALING     Pose offset, chain bone or chain ancestor scale is not unit.
 *  - INVALID_BIND_OFFSET     A child's bind translation is not along its parent's +Y.
 *  - INVALID_BIND_LENGTH     A link length disagrees with the armature bind data.
 *  - FIRST_GUIDE_MISMATCH    Guide point 0 is not within tolerance of the sampled chain root.
 *  - CHANNEL_BONE_OVERLAP    Two channels use the same chain bone.
 *  - CHANNEL_ANCESTOR_CONFLICT
 *                            A bone in one channel is an ancestor of a bone in another channel.
 *  - CURVE_TOO_SHORT         The curve cannot provide the next full link length.
 *  - WORK_BUDGET_EXCEEDED    Root isolation exceeded the channel's finite work budget.
 *  - DEGENERATE_CURVE        The solved chain produced a zero-length direction.
 */
export const CURVE_CHAIN_ERRORS = {
    INVALID_ARMATURE              : 'INVALID_ARMATURE',
    INVALID_CHAIN                 : 'INVALID_CHAIN',
    INVALID_BONE_INDEX            : 'INVALID_BONE_INDEX',
    NON_CONTIGUOUS_CHAIN          : 'NON_CONTIGUOUS_CHAIN',
    INVALID_GUIDE_COUNT           : 'INVALID_GUIDE_COUNT',
    INVALID_LINK_LENGTH           : 'INVALID_LINK_LENGTH',
    INVALID_TOLERANCE             : 'INVALID_TOLERANCE',
    INVALID_BUDGET                : 'INVALID_BUDGET',
    NON_FINITE_VALUE              : 'NON_FINITE_VALUE',
    UNSUPPORTED_SCALING           : 'UNSUPPORTED_SCALING',
    INVALID_BIND_OFFSET           : 'INVALID_BIND_OFFSET',
    INVALID_BIND_LENGTH           : 'INVALID_BIND_LENGTH',
    FIRST_GUIDE_MISMATCH          : 'FIRST_GUIDE_MISMATCH',
    CHANNEL_BONE_OVERLAP          : 'CHANNEL_BONE_OVERLAP',
    CHANNEL_ANCESTOR_CONFLICT     : 'CHANNEL_ANCESTOR_CONFLICT',
    CURVE_TOO_SHORT               : 'CURVE_TOO_SHORT',
    WORK_BUDGET_EXCEEDED          : 'WORK_BUDGET_EXCEEDED',
    DEGENERATE_CURVE              : 'DEGENERATE_CURVE',
} as const;

export type CurveChainErrorCode = typeof CURVE_CHAIN_ERRORS[ keyof typeof CURVE_CHAIN_ERRORS ];

/** Error thrown for any curve-chain validation / solve failure. */
export class CurveChainError extends Error{
    code : string;

    constructor( code: string, message: string ){
        super( `[ ${code} ] ${message}` );
        this.name = 'CurveChainError';
        this.code = code;
    }
}

/** Options accepted by a CurveChainTrack. */
export interface CurveChainTrackOptions{
    /** Maximum allowed world-space error, also used as the sphere intersection tolerance. Default 1e-6. */
    positionTolerance ?: number;
    /** Finite work budget (root isolation evaluation / bisection units). Default 100000. */
    workBudget        ?: number;
}

/** Read-only snapshot taken after ordinary animation sampling and before any chain is solved. */
export interface CurveChainCapture{
    track       : CurveChainTrack;
    /** Guide bones' sampled world positions, copied. */
    guidePoints : vec3[];
    /** Chain root bone index. */
    rootIndex   : number;
    /** Sampled world position of the chain root (the curve start). */
    rootPos     : vec3;
    /** Sampled world +Z of the chain root, normalised. */
    rootZ       : vec3;
    /** Pre-solve world rotation of the root's parent (pose offset when the root has none). */
    parentRot   : quat;
}

//#region INTERNAL HELPERS
const UNIT_SCALE_TOL = 1e-6;
const BIND_TOL       = 1e-5;

function isFiniteNum( v: number ): boolean{
    return typeof v === 'number' && Number.isFinite( v );
}

function isFiniteVec( v: ArrayLike<number> | null | undefined ): boolean{
    if( !v || v.length < 3 ) return false;
    return isFiniteNum( v[ 0 ] ) && isFiniteNum( v[ 1 ] ) && isFiniteNum( v[ 2 ] );
}

function isFiniteQuat( q: ArrayLike<number> | null | undefined ): boolean{
    if( !q || q.length < 4 ) return false;
    return isFiniteNum( q[ 0 ] ) && isFiniteNum( q[ 1 ] ) && isFiniteNum( q[ 2 ] ) && isFiniteNum( q[ 3 ] );
}

function isUnitScale( v: ArrayLike<number> | null | undefined ): boolean{
    if( !isFiniteVec( v ) ) return false;
    return Math.abs( v[ 0 ] - 1 ) <= UNIT_SCALE_TOL &&
           Math.abs( v[ 1 ] - 1 ) <= UNIT_SCALE_TOL &&
           Math.abs( v[ 2 ] - 1 ) <= UNIT_SCALE_TOL;
}

function err( code: string, msg: string ): never{
    throw new CurveChainError( code, msg );
}
//#endregion

/**
 * A single curve-chain channel for a Clip.
 *
 * A channel ties one contiguous IKChain to 3m+1 guide bones. Every group of
 * four consecutive guide points is one cubic Bezier segment; the resulting
 * piecewise curve is sampled after the Clip's normal tracks have run. The
 * chain is then solved so every bone origin and the final tail sit on that
 * curve, with each chord between consecutive origins equal to the link's bind
 * length.
 *
 * The declaration order of channels inside Clip.curveChains never affects the
 * result: all guide points are frozen before any channel is solved.
 */
export class CurveChainTrack{
    /** Chain to solve. Its links are read-only from this channel's point of view. */
    chain        : IKChain;
    /** 3m+1 bone indices whose sampled world origins are the Bezier control points. */
    guideBones   : number[];
    /** World-space tolerance for guide #0 vs. chain root and for curve/link intersections. */
    positionTolerance : number;
    /** Finite root-isolation work budget. */
    workBudget        : number;

    constructor( chain: IKChain, guideBones: number[], options: CurveChainTrackOptions = {} ){
        this.chain              = chain;
        this.guideBones         = guideBones;
        this.positionTolerance  = ( options.positionTolerance !== undefined ) ? options.positionTolerance : 1e-6;
        this.workBudget         = ( options.workBudget !== undefined ) ? options.workBudget : 100000;
    }

    /** Number of cubic Bezier segments described by the guide bones. */
    get segmentCount(): number{
        return ( this.guideBones.length - 1 ) / 3;
    }

    /**
     * Validate everything that does not require sampled world positions.
     *
     * Checks performed:
     *  - tolerance / budget are finite and > 0
     *  - chain exists, is non-empty, contiguous in the *skeleton's* real parent
     *    graph, has unique bones and does not fold back into itself
     *  - guide count is 3m+1 and every index is a valid skeleton bone
     *  - link lengths are finite, positive and equal to the armature bind lengths
     *  - child bind translations are (0, len, 0) along the parent's +Y
     *  - pose offset / chain / chain-ancestor scales are unit
     *  - all relevant transforms are finite
     */
    validate( pose: Pose ): this{
        const arm: Armature | undefined = pose ? pose.arm : undefined;
        if( !arm ) err( CURVE_CHAIN_ERRORS.INVALID_ARMATURE, 'Pose has no Armature' );

        if( !isFiniteNum( this.positionTolerance ) || this.positionTolerance <= 0 )
            err( CURVE_CHAIN_ERRORS.INVALID_TOLERANCE, `positionTolerance must be finite and > 0, got ${this.positionTolerance}` );

        if( !isFiniteNum( this.workBudget ) || this.workBudget <= 0 )
            err( CURVE_CHAIN_ERRORS.INVALID_BUDGET, `workBudget must be finite and > 0, got ${this.workBudget}` );

        //-----------------------------------------------------------
        // Guide indices
        if( !Array.isArray( this.guideBones ) )
            err( CURVE_CHAIN_ERRORS.INVALID_GUIDE_COUNT, 'guideBones must be an array' );

        const gCnt = this.guideBones.length;
        if( gCnt < 4 || ( gCnt - 1 ) % 3 !== 0 )
            err( CURVE_CHAIN_ERRORS.INVALID_GUIDE_COUNT, `guideBones must contain 3m+1 points (m >= 1), got ${gCnt}` );

        for( const gi of this.guideBones ){
            if( !Number.isInteger( gi ) || gi < 0 || gi >= arm.bones.length )
                err( CURVE_CHAIN_ERRORS.INVALID_BONE_INDEX, `invalid guide bone index ${gi}` );

            // A guide bone whose sampled world origin is not finite can never
            // produce a usable curve; reject it before solving.
            const gb = pose.bones[ gi ];
            if( !gb )
                err( CURVE_CHAIN_ERRORS.INVALID_BONE_INDEX, `pose guide bone ${gi} is missing` );
            if( !isFiniteVec( gb.local.pos ) || !isFiniteQuat( gb.local.rot ) || !isFiniteVec( gb.local.scl ) )
                err( CURVE_CHAIN_ERRORS.NON_FINITE_VALUE, `guide bone ${gi} local transform is not finite` );
        }

        //-----------------------------------------------------------
        // Chain shape
        const ch = this.chain;
        if( !ch || !Array.isArray( ch.links ) || ch.links.length === 0 )
            err( CURVE_CHAIN_ERRORS.INVALID_CHAIN, 'chain must contain at least one link' );

        const links = ch.links;
        const cnt   = links.length;
        const seen  = new Set< number >();

        for( let i=0; i < cnt; i++ ){
            const lnk: IKLink = links[ i ];
            if( !lnk )
                err( CURVE_CHAIN_ERRORS.INVALID_CHAIN, `chain link ${i} is missing` );

            if( !Number.isInteger( lnk.idx ) || lnk.idx < 0 || lnk.idx >= arm.bones.length )
                err( CURVE_CHAIN_ERRORS.INVALID_BONE_INDEX, `invalid chain bone index ${lnk.idx}` );

            if( seen.has( lnk.idx ) )
                err( CURVE_CHAIN_ERRORS.NON_CONTIGUOUS_CHAIN, `chain repeats bone index ${lnk.idx}` );
            seen.add( lnk.idx );

            if( !isFiniteNum( lnk.len ) || lnk.len <= 0 )
                err( CURVE_CHAIN_ERRORS.INVALID_LINK_LENGTH, `link ${i} length must be finite and > 0, got ${lnk.len}` );

            const boneLen = arm.bones[ lnk.idx ].len;
            const lenTol  = BIND_TOL * Math.max( 1, Math.abs( lnk.len ), Math.abs( boneLen ) );
            if( !isFiniteNum( boneLen ) || Math.abs( lnk.len - boneLen ) > lenTol )
                err( CURVE_CHAIN_ERRORS.INVALID_BIND_LENGTH,
                    `link ${i} length ${lnk.len} does not match armature bind length ${boneLen}` );

            // Bind data must exist and be finite.
            if( !lnk.bind )
                err( CURVE_CHAIN_ERRORS.INVALID_CHAIN, `link ${i} has no bind transform` );
            if( !isFiniteVec( lnk.bind.pos ) || !isFiniteQuat( lnk.bind.rot ) || !isFiniteVec( lnk.bind.scl ) )
                err( CURVE_CHAIN_ERRORS.NON_FINITE_VALUE, `link ${i} bind transform is not finite` );
            if( !isUnitScale( lnk.bind.scl ) )
                err( CURVE_CHAIN_ERRORS.UNSUPPORTED_SCALING, `link ${i} bind scale must be (1,1,1)` );

            // Actual skeleton parent relationship, not a stale link.pidx.
            const realPidx = arm.bones[ lnk.idx ].pidx;
            if( !Number.isInteger( realPidx ) || realPidx < -1 || realPidx >= arm.bones.length )
                err( CURVE_CHAIN_ERRORS.INVALID_BONE_INDEX, `bone ${lnk.idx} has invalid parent index ${realPidx}` );

            if( lnk.pidx !== realPidx )
                err( CURVE_CHAIN_ERRORS.NON_CONTIGUOUS_CHAIN,
                    `link ${i} parent ${lnk.pidx} does not match skeleton parent ${realPidx}` );

            // The chain must be straight in the real skeleton: each link's
            // skeleton parent is the previous link.
            if( i > 0 && realPidx !== links[ i - 1 ].idx )
                err( CURVE_CHAIN_ERRORS.NON_CONTIGUOUS_CHAIN,
                    `bone ${lnk.idx} is not a child of bone ${links[i-1].idx}` );

            // Children must be attached along the parent's +Y by the bind length.
            if( i > 0 ){
                const prevLen = links[ i - 1 ].len;
                const offTol  = BIND_TOL * Math.max( 1, Math.abs( prevLen ) );
                if( Math.abs( lnk.bind.pos[ 0 ] ) > offTol || Math.abs( lnk.bind.pos[ 2 ] ) > offTol )
                    err( CURVE_CHAIN_ERRORS.INVALID_BIND_OFFSET,
                        `link ${i} bind translation must be along the parent's +Y` );
                if( Math.abs( lnk.bind.pos[ 1 ] - prevLen ) > offTol )
                    err( CURVE_CHAIN_ERRORS.INVALID_BIND_OFFSET,
                        `link ${i} bind +Y offset ${lnk.bind.pos[1]} does not match parent length ${prevLen}` );
            }
        }

        // Real skeleton contiguity, root parent must be outside the chain.
        for( let i=1; i < cnt; i++ ){
            if( arm.bones[ links[ i ].idx ].pidx !== links[ i - 1 ].idx )
                err( CURVE_CHAIN_ERRORS.NON_CONTIGUOUS_CHAIN,
                    `bone ${links[i].idx} is not a child of bone ${links[i-1].idx}` );
        }
        const rootParent = arm.bones[ links[ 0 ].idx ].pidx;
        if( rootParent !== -1 && seen.has( rootParent ) )
            err( CURVE_CHAIN_ERRORS.NON_CONTIGUOUS_CHAIN, 'chain root parent is inside the chain' );

        //-----------------------------------------------------------
        // Scaling rules for this path.
        if( !isFiniteVec( pose.offset.pos ) || !isFiniteQuat( pose.offset.rot ) || !isFiniteVec( pose.offset.scl ) )
            err( CURVE_CHAIN_ERRORS.NON_FINITE_VALUE, 'Pose offset is not finite' );
        if( !isUnitScale( pose.offset.scl ) )
            err( CURVE_CHAIN_ERRORS.UNSUPPORTED_SCALING, 'Pose offset scale must be (1,1,1)' );

        for( let i=0; i < cnt; i++ ){
            const b = pose.bones[ links[ i ].idx ];
            if( !b )
                err( CURVE_CHAIN_ERRORS.INVALID_BONE_INDEX, `pose bone ${links[i].idx} is missing` );
            if( !isFiniteVec( b.local.pos ) || !isFiniteQuat( b.local.rot ) || !isFiniteVec( b.local.scl ) )
                err( CURVE_CHAIN_ERRORS.NON_FINITE_VALUE, `chain bone ${links[i].idx} local transform is not finite` );
            if( !isUnitScale( b.local.scl ) )
                err( CURVE_CHAIN_ERRORS.UNSUPPORTED_SCALING, `chain bone ${links[i].idx} scale must be (1,1,1)` );
        }

        // Walk the real ancestor chain outside the root. Rotations / translations
        // are allowed there, scaling is not.
        let pidx = rootParent;
        let guard = 0;
        while( pidx !== -1 ){
            if( !Number.isInteger( pidx ) || pidx < 0 || pidx >= arm.bones.length || ++guard > arm.bones.length )
                err( CURVE_CHAIN_ERRORS.INVALID_BONE_INDEX, `invalid or cyclic ancestor index ${pidx}` );

            const ab = pose.bones[ pidx ];
            if( !ab )
                err( CURVE_CHAIN_ERRORS.INVALID_BONE_INDEX, `pose ancestor bone ${pidx} is missing` );
            if( !isFiniteVec( ab.local.pos ) || !isFiniteQuat( ab.local.rot ) || !isFiniteVec( ab.local.scl ) )
                err( CURVE_CHAIN_ERRORS.NON_FINITE_VALUE, `ancestor bone ${pidx} local transform is not finite` );
            if( !isUnitScale( ab.local.scl ) )
                err( CURVE_CHAIN_ERRORS.UNSUPPORTED_SCALING, `ancestor bone ${pidx} scale must be (1,1,1)` );

            pidx = arm.bones[ pidx ].pidx;
        }

        return this;
    }

    /**
     * Freeze the sampled world-space guide points and the chain root's pre-solve
     * orientation. The returned data contains copies only; later solving is
     * guaranteed not to write back into it.
     */
    capture( pose: Pose ): CurveChainCapture{
        this.validate( pose );

        const arm   = pose.arm as Armature;
        const links = this.chain.links;
        const root  = links[ 0 ];
        const rootB = pose.bones[ root.idx ];

        if( !isFiniteVec( rootB.world.pos ) || !isFiniteQuat( rootB.world.rot ) )
            err( CURVE_CHAIN_ERRORS.NON_FINITE_VALUE, 'chain root world transform is not finite' );

        const guidePoints: vec3[] = new Array( this.guideBones.length );
        for( let i=0; i < this.guideBones.length; i++ ){
            const b = pose.bones[ this.guideBones[ i ] ];
            if( !b || !isFiniteVec( b.world.pos ) )
                err( CURVE_CHAIN_ERRORS.NON_FINITE_VALUE, `guide bone ${this.guideBones[i]} has a non-finite world position` );
            guidePoints[ i ] = [ b.world.pos[ 0 ], b.world.pos[ 1 ], b.world.pos[ 2 ] ];
        }

        // World +Z of the first chain bone before solving. The caller has already
        // run the ordinary tracks and updated the pose world transforms.
        const q  = rootB.world.rot;
        const zx = 2 * ( q[ 0 ] * q[ 2 ] + q[ 3 ] * q[ 1 ] );
        const zy = 2 * ( q[ 1 ] * q[ 2 ] - q[ 3 ] * q[ 0 ] );
        const zz = 1 - 2 * ( q[ 0 ] * q[ 0 ] + q[ 1 ] * q[ 1 ] );
        const zl = Math.hypot( zx, zy, zz ) || 1;

        // World rotation of the root's parent, frozen before solving so no
        // channel can influence another through the shared hierarchy.
        let parentRot: quat;
        if( root.pidx === -1 ){
            parentRot = [ pose.offset.rot[0], pose.offset.rot[1], pose.offset.rot[2], pose.offset.rot[3] ];
        }else{
            const pb = pose.bones[ root.pidx ];
            if( !pb || !isFiniteQuat( pb.world.rot ) )
                err( CURVE_CHAIN_ERRORS.NON_FINITE_VALUE, `root parent bone ${root.pidx} has a non-finite world rotation` );
            parentRot = [ pb.world.rot[0], pb.world.rot[1], pb.world.rot[2], pb.world.rot[3] ];
        }

        return {
            track       : this,
            guidePoints : guidePoints,
            rootIndex   : root.idx,
            rootPos     : [ rootB.world.pos[ 0 ], rootB.world.pos[ 1 ], rootB.world.pos[ 2 ] ],
            rootZ       : [ zx / zl, zy / zl, zz / zl ],
            parentRot   : parentRot,
        };
    }
}

export default CurveChainTrack;

//#region CHANNEL SET VALIDATION
/**
 * Validate cross-channel rules for every CurveChainTrack declared on one Clip.
 *
 * Rules:
 *  - Two channels may not use the same chain bone (CHANNEL_BONE_OVERLAP).
 *  - No chain bone of one channel may be an ancestor of any chain bone of
 *    another channel, tested through the skeleton's real parent indices
 *    (CHANNEL_ANCESTOR_CONFLICT).
 *  - Ancestors that are not part of any constrained chain may be shared freely.
 *
 * Declaration order is irrelevant: the check is pairwise and symmetric.
 */
export function validateCurveChainSet( tracks: CurveChainTrack[], arm: Armature ): void{
    if( !arm ) err( CURVE_CHAIN_ERRORS.INVALID_ARMATURE, 'Armature is required to validate curve chains' );
    if( !Array.isArray( tracks ) ) err( CURVE_CHAIN_ERRORS.INVALID_CHAIN, 'curveChains must be an array' );

    const boneCnt = arm.bones.length;
    const chainSets: Array< Set< number > > = new Array( tracks.length );

    //-----------------------------------------------------------
    // Collect chain bones and reject duplicates / literal overlap.
    const owner = new Map< number, number >();          // bone -> first channel index
    for( let ci=0; ci < tracks.length; ci++ ){
        const tr = tracks[ ci ];
        if( !tr || !tr.chain || !Array.isArray( tr.chain.links ) || tr.chain.links.length === 0 )
            err( CURVE_CHAIN_ERRORS.INVALID_CHAIN, `channel ${ci} has an empty chain` );

        const set = new Set< number >();
        for( const lnk of tr.chain.links ){
            const bi = lnk ? lnk.idx : -1;
            if( !Number.isInteger( bi ) || bi < 0 || bi >= boneCnt )
                err( CURVE_CHAIN_ERRORS.INVALID_BONE_INDEX, `channel ${ci} has invalid bone index ${bi}` );
            if( set.has( bi ) )
                err( CURVE_CHAIN_ERRORS.NON_CONTIGUOUS_CHAIN, `channel ${ci} repeats bone ${bi}` );

            const prior = owner.get( bi );
            if( prior !== undefined )
                err( CURVE_CHAIN_ERRORS.CHANNEL_BONE_OVERLAP,
                    `bone ${bi} is used by channels ${prior} and ${ci}` );
            owner.set( bi, ci );
            set.add( bi );
        }
        chainSets[ ci ] = set;
    }

    //-----------------------------------------------------------
    // Ancestor test through the real parent graph.
    const ancestorsOf = ( boneIdx: number ): Set< number >=>{
        const out = new Set< number >();
        let pidx = arm.bones[ boneIdx ].pidx;
        let guard = 0;
        while( pidx !== -1 ){
            if( ++guard > boneCnt || pidx < 0 || pidx >= boneCnt ) break;
            out.add( pidx );
            pidx = arm.bones[ pidx ].pidx;
        }
        return out;
    };

    for( let a=0; a < tracks.length; a++ ){
        for( let b=0; b < tracks.length; b++ ){
            if( a === b ) continue;
            for( const bone of chainSets[ b ] ){
                const anc = ancestorsOf( bone );
                for( const cand of chainSets[ a ] ){
                    if( anc.has( cand ) )
                        err( CURVE_CHAIN_ERRORS.CHANNEL_ANCESTOR_CONFLICT,
                            `chain bone ${cand} of channel ${a} is an ancestor of chain bone ${bone} of channel ${b}` );
                }
            }
        }
    }
}
//#endregion
