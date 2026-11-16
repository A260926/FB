import type { SkinMotionEditOptions, SkinMotionEditResult } from './SkinMotionEdit';

export interface GLTFAnimationData {
    node: any;
    transform: any;
    interpolation: any;
    times: any;
    values: any;
}

/**
 * A class for animation.
 * @see {@link https://github.com/oframe/ogl/blob/master/src/extras/GLTFAnimation.js | Source}
 */
export class GLTFAnimation {
    data: GLTFAnimationData[];
    elapsed: number;
    weight: number;
    loop: boolean;
    startTime: number;
    endTime: number;
    duration: number;

    constructor(data: GLTFAnimationData[], weight?: number);

    update(totalWeight?: number, isSet?: boolean): void;

    cubicSplineInterpolate(t: number, prevVal: any, prevTan: any, nextTan: any, nextVal: any): any;

    /**
     * Jointly solves local rotation corrections over the whole clip so that
     * skinned surface probes match world-space observations. The original mesh,
     * skin weights, channels and playback state are never modified.
     */
    editSkinMotion(options: SkinMotionEditOptions): SkinMotionEditResult;
}
