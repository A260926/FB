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

    /**
     * Get the world space vertex positions of this skinned mesh, deformed by
     * the current skeleton pose, the local joint palette and offsetMatrix.
     * Recomputed on every call, geometry attributes are never written.
     */
    getSkinnedWorldPositions(): Float32Array;

    /**
     * Get the bounding box enclosing the positions returned by
     * getSkinnedWorldPositions, in world space. Recomputed on every call.
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
