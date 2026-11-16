//#region IMPORTS
import type { ITrack }  from './tracks/types';
import type Pose        from '../armature/Pose';
import type Clip        from './Clip';

import { CurveChainTrack, validateCurveChainSet, CurveChainError, CURVE_CHAIN_ERRORS } from './tracks/CurveChainTrack';
import CurveChainSolver                           from '../ikrig/solvers/CurveChainSolver';
import type { CurveChainCapture }                 from './tracks/CurveChainTrack';

import Maths            from '../maths/Maths';
//#endregion




/** Set of information to pass to Tracks for when animating a character. */
class FrameInfo{
    kt0 : number = 0;   // Keyframe Index for Start Tangent
    kt1 : number = 0;   // Keyframe Index for End Tangent
    k0  : number = -1;  // Keyframe Index Lerp Start
    k1  : number = -1;  // Keyframe Index Lerp End
    t   : number = 0;   // Lerp Time  
    ti  : number = 0;   // Lerp Time Inverse
}

/** Basic Animator for Armature */
class Animator{
    //#region MAIN
    frameInfo    : Array< FrameInfo > = [];  // Clips can have multiple TimeStamps, So need to compute Frame Data for each
    clock        : number             = 0;   // Running Animation Clock
    clip        !: Clip;                    // Animation to Run
    inPlace      = false;                    // Lock the forward movement of the animation when applied to pose
    inPlaceScale = [1,1,1];

    //constructor(){}
    //#endregion

    //#region METHODS
    
    resetClock(){ this.clock = 0; return this; }
    setClip( c: Clip ): this{ this.clip = c; return this; }

    /** Move Animation to the next possible frame. */
    update( deltaTime: number ): this{
        this.clock = ( this.clock + deltaTime ) % this.clip.duration;
        this._computeFrameInfo();
        return this;
    }

    applyPose( pose: Pose ): this{
        if( !this.clip ) return this;

        const hasChains = this.clip.curveChains && this.clip.curveChains.length > 0;

        //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        // Snapshot before anything is written so a channel failure can
        // restore local / world / offset to the state before this call.
        const snap = hasChains ? this._snapshotPose( pose ) : null;

        try{
            //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
            // Ordinary tracks first. A clip without curveChains keeps the
            // original behaviour exactly (no extra world update).
            let t : ITrack;
            for( t of this.clip.tracks ){
                t.apply( pose, this.frameInfo[ t.timeStampIndex ] );
            }

            //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
            // Curve chains: sample, freeze every guide point, then solve.
            if( hasChains ) this._applyCurveChains( pose );

            //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
            if( this.inPlace ){
                const bPos = pose.bones[ 0 ].local.pos;
                bPos[ 0 ] *= this.inPlaceScale[ 0 ];
                bPos[ 1 ] *= this.inPlaceScale[ 1 ];
                bPos[ 2 ] *= this.inPlaceScale[ 2 ];
            }
        }catch( e ){
            if( snap ) this._restorePose( pose, snap );
            throw e;
        }

        return this;
    }

    //#region CURVE CHAINS

    /**
     * Validate every channel against the whole clip, freeze all guide points
     * from the ordinary sampling result, then solve channels independently.
     * Guide points and root positions are frozen from the same pre-solve world,
     * so channel declaration order cannot influence the result.
     */
    _applyCurveChains( pose: Pose ): void{
        const chains = this.clip.curveChains;

        //---------------------------------------------------
        // Cross-channel rules first (bone overlap / ancestors).
        // Every entry must be its own channel: the same track object used
        // twice would legitimately share bones, so it is rejected as overlap.
        const ordered: CurveChainTrack[] = [];
        for( const tr of chains ){
            if( !tr || typeof tr.capture !== 'function' )
                throw new CurveChainError( CURVE_CHAIN_ERRORS.INVALID_CHAIN,
                    'curveChains must only contain CurveChainTrack instances' );
            ordered.push( tr );
        }
        if( !pose.arm )
            throw new CurveChainError( CURVE_CHAIN_ERRORS.INVALID_ARMATURE, 'Pose has no Armature' );
        validateCurveChainSet( ordered, pose.arm );

        //---------------------------------------------------
        // Single world update, then freeze every guide point.
        pose.updateWorld();
        const captures: CurveChainCapture[] = new Array( ordered.length );
        for( let i=0; i < ordered.length; i++ ) captures[ i ] = ordered[ i ].capture( pose );

        //---------------------------------------------------
        // Solve each chain from its frozen snapshot.
        for( const cap of captures ){
            new CurveChainSolver().resolve( cap, pose );
        }
    }

    _snapshotPose( pose: Pose ): { locals: Float32Array, worlds: Float32Array, offset: Float32Array }{
        const n = pose.bones.length;
        const locals  = new Float32Array( n * 10 );
        const worlds  = new Float32Array( n * 10 );

        for( let i=0; i < n; i++ ){
            const b  = pose.bones[ i ];
            const lo = i * 10;
            locals[ lo ]      = b.local.pos[0]; locals[ lo + 1 ] = b.local.pos[1]; locals[ lo + 2 ] = b.local.pos[2];
            locals[ lo + 3 ]  = b.local.rot[0]; locals[ lo + 4 ] = b.local.rot[1]; locals[ lo + 5 ] = b.local.rot[2]; locals[ lo + 6 ] = b.local.rot[3];
            locals[ lo + 7 ]  = b.local.scl[0]; locals[ lo + 8 ] = b.local.scl[1]; locals[ lo + 9 ] = b.local.scl[2];

            worlds[ lo ]      = b.world.pos[0]; worlds[ lo + 1 ] = b.world.pos[1]; worlds[ lo + 2 ] = b.world.pos[2];
            worlds[ lo + 3 ]  = b.world.rot[0]; worlds[ lo + 4 ] = b.world.rot[1]; worlds[ lo + 5 ] = b.world.rot[2]; worlds[ lo + 6 ] = b.world.rot[3];
            worlds[ lo + 7 ]  = b.world.scl[0]; worlds[ lo + 8 ] = b.world.scl[1]; worlds[ lo + 9 ] = b.world.scl[2];
        }

        const offset = new Float32Array( 10 );
        offset[0] = pose.offset.pos[0]; offset[1] = pose.offset.pos[1]; offset[2] = pose.offset.pos[2];
        offset[3] = pose.offset.rot[0]; offset[4] = pose.offset.rot[1]; offset[5] = pose.offset.rot[2]; offset[6] = pose.offset.rot[3];
        offset[7] = pose.offset.scl[0]; offset[8] = pose.offset.scl[1]; offset[9] = pose.offset.scl[2];

        return { locals, worlds, offset };
    }

    _restorePose( pose: Pose, snap: { locals: Float32Array, worlds: Float32Array, offset: Float32Array } ): void{
        const n = pose.bones.length;
        for( let i=0; i < n; i++ ){
            const b  = pose.bones[ i ];
            const lo = i * 10;
            b.local.pos[0] = snap.locals[ lo ];     b.local.pos[1] = snap.locals[ lo + 1 ]; b.local.pos[2] = snap.locals[ lo + 2 ];
            b.local.rot[0] = snap.locals[ lo + 3 ]; b.local.rot[1] = snap.locals[ lo + 4 ]; b.local.rot[2] = snap.locals[ lo + 5 ]; b.local.rot[3] = snap.locals[ lo + 6 ];
            b.local.scl[0] = snap.locals[ lo + 7 ]; b.local.scl[1] = snap.locals[ lo + 8 ]; b.local.scl[2] = snap.locals[ lo + 9 ];

            b.world.pos[0] = snap.worlds[ lo ];     b.world.pos[1] = snap.worlds[ lo + 1 ]; b.world.pos[2] = snap.worlds[ lo + 2 ];
            b.world.rot[0] = snap.worlds[ lo + 3 ]; b.world.rot[1] = snap.worlds[ lo + 4 ]; b.world.rot[2] = snap.worlds[ lo + 5 ]; b.world.rot[3] = snap.worlds[ lo + 6 ];
            b.world.scl[0] = snap.worlds[ lo + 7 ]; b.world.scl[1] = snap.worlds[ lo + 8 ]; b.world.scl[2] = snap.worlds[ lo + 9 ];
        }

        pose.offset.pos[0] = snap.offset[0]; pose.offset.pos[1] = snap.offset[1]; pose.offset.pos[2] = snap.offset[2];
        pose.offset.rot[0] = snap.offset[3]; pose.offset.rot[1] = snap.offset[4]; pose.offset.rot[2] = snap.offset[5]; pose.offset.rot[3] = snap.offset[6];
        pose.offset.scl[0] = snap.offset[7]; pose.offset.scl[1] = snap.offset[8]; pose.offset.scl[2] = snap.offset[9];
    }

    //#endregion

    /** Set Animator on a specific frame */
    atKey( n: number ): this{
        // TODO : Revisit when have animation examples of multiple TimeStamp Arrays
        if( !this.clip )    return this;
        if( n < 0 )         n = 0;
        this._genFrameInfo();

        //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        const aryTs = this.clip.timeStamps;
        const aryFi = this.frameInfo;
        let ts      : Float32Array;    // TimeStamp;
        let fi      : FrameInfo;
        let tsLen   : number;          // TimeStamp Length;

        //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        for( let i=0; i < aryTs.length; i++ ){
            ts      = aryTs[ i ];
            fi      = aryFi[ i ];
            tsLen   = ts.length - 1;

            fi.t    = 1;
            fi.ti   = 0;
            fi.k0   = ( n <= tsLen )? n : tsLen;
            fi.k1   = fi.k0;
            fi.kt0  = fi.k0;
            fi.kt1  = fi.k0;
        }

        return this;
    }

    //#endregion

    //#region INTERNAL

    /** Make sure we have enough frame info objects to handle all the timestamps of the clip  */
    _genFrameInfo(){
        const aryFi = this.frameInfo;
        const aryTs = this.clip.timeStamps;
        if( aryFi.length < aryTs.length ){
            for( let i=aryFi.length; i < aryTs.length; i++ ) aryFi.push( new FrameInfo );
        }
    }

    _computeFrameInfo() : void{
        if( !this.clip ) return;
        this._genFrameInfo();

        //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        const aryFi = this.frameInfo;
        const aryTs = this.clip.timeStamps;
        const time  = this.clock;

        let ts      : Float32Array;    // TimeStamp;
        let fi      : FrameInfo;
        let tLen    : number;          // TimeStamp Length;

        for( let i=0; i < aryTs.length; i++ ){
            //-------------------------------------
            // Get our Main bits of Data
            ts      = aryTs[ i ];
            fi      = aryFi[ i ];
            tLen    = ts.length;

            //-------------------------------------
            // Some Animations have a timestamp with just one frame
            // this is to set specific bones at the start of the animation but
            // doesn't change during the course of the animation.
            if( tLen == 0 ){
                //console.log( 'Single Keyframe' );
                fi.t    = 1;
                fi.ti   = 0;
                fi.k0   = 0;
                fi.k1   = 0;
                fi.kt0  = 0;
                fi.kt1  = 0;
                continue;
            }

            //-------------------------------------
            if( fi.k0 != -1 && time >= ts[ fi.k0 ] && time <= ts[ fi.k1 ] ){
                // If the clock still exists between the previous keyframes,
                // then just save time by just computing its T values.
                
                //console.log( 'Reuse Keyframes' );
                //continue;
            }else{
                // Find the Start/End Keyframes that the clock lives between
                //console.log( 'Find Keyframes' );
                //console.log( 'Clock', time );
                //console.log( "TimeStamp", ts );

                // Find the Index of timestamp that is the first one to be greater then time.
                let imin = 0, mi = 0, imax = ts.length - 1;
                while( imin < imax ){                     // Once Min Crosses or Equals Max, Stop Loop.
                    mi = (imin + imax) >>> 1              // Compute Mid Index
                    if( time < ts[ mi ] ) imax = mi;      // Time is LT Timestamp, use mid as new Max Range
                    else                  imin = mi + 1;  // Time is GTE TimeStamp, move min to one after mid.
                }

                // The starting / ending Keyframe Indices
                if( imax <= 0 ){    fi.k0 = 0;          fi.k1 = 1; }        // Can't go negative, so set to main frame
                else{               fi.k0 = imax - 1;   fi.k1 = imax; }

                // Tangent Keyframe Indices that loop around
                fi.kt0 = Maths.mod( fi.k0 - 1, tLen );
                fi.kt1 = Maths.mod( fi.k1 + 1, tLen );
            }

            //-------------------------------------
            // Lerp Time & its inverse.
            fi.t    = ( time - ts[ fi.k0 ] ) / ( ts[ fi.k1 ] - ts[ fi.k0 ] ); // Map Time between the Two Time Stamps
            fi.ti   = 1 - fi.t;
        }
    }
    //#endregion
}

export default Animator;
export { FrameInfo };