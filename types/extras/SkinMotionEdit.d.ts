import type { GLTFAnimation } from './GLTFAnimation';
import type { GLTFSkin } from './GLTFSkin';
import type { Transform } from '../core/Transform';

export interface SkinMotionProbe {
    triangle: number;
    barycentric: ArrayLike<number>;
}

export interface SkinMotionObservation {
    sample: number;
    probe: number;
    target: ArrayLike<number>;
    weight: number;
}

export interface SkinMotionEditOptions {
    skin: GLTFSkin;
    root: Transform;
    joints: Transform[];
    times: ArrayLike<number>;
    probes: SkinMotionProbe[];
    observations: SkinMotionObservation[];
    lambda: number;
    smoothness: number;
    maxAngle: number;
    tolerance?: number;
    maxIterations?: number;
}

export interface SkinMotionEditResult {
    animation: GLTFAnimation;
    rotations: Float64Array;
    gradient: Float64Array;
    objective: number;
    residual: number;
    iterations: number;
    converged: boolean;
    history: number[];
    errors: Float64Array;
}

export function editSkinMotion(animation: GLTFAnimation, options: SkinMotionEditOptions): SkinMotionEditResult;
