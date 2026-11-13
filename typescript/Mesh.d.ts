import { Renderable } from './Renderable';
import { Skeleton } from './Skeleton';
import { Matrix4 } from './math/Matrix4';
import { BoundingBox } from './math/BoundingBox';
import { IRenderableOption } from './Renderable';

export interface IMeshOption extends IRenderableOption {
    skeleton?: Skeleton;
    joints?: number[];
}

export class Mesh extends Renderable {

    constructor(option?: IMeshOption);

    joints: number[];

    skeleton: Skeleton;

    offsetMatrix: Matrix4;

    isSkinnedMesh(): boolean;

    /**
     * World positions of every vertex after the current skinning,
     * three components per vertex in vertex order. Recomputed on each call,
     * the shared geometry is not modified.
     */
    getSkinnedWorldPositions(): Float32Array;

    /**
     * World space bounding box enclosing the current skinned vertices.
     * Recomputed on each call.
     */
    getSkinnedWorldBoundingBox(): BoundingBox;

    clone(): Mesh;

    // Enums
    static POINTS: number;
    static LINES: number;
    static LINE_LOOP: number;
    static LINE_STRIP: number;
    static TRIANGLES: number;
    static TRIANGLE_STRIP: number;
    static TRIANGLE_FAN: number;

    static BACK: number;
    static FRONT: number;
    static FRONT_AND_BACK: number;
    static CW: number;
    static CCW: number;
}
