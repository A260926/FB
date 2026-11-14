import { GLTFAnimation } from './GLTFAnimation.js';

/**
 * Blends a fixed set of GLTFAnimation clips into one simultaneous local pose.
 *
 * Clips are sampled independently from their own elapsed/loop/weight state and
 * each node property is written once per update, so the result is independent
 * of clip order and repeated calls. Position and scale use weighted vector
 * sums; rotation uses the intrinsic weighted mean on SO(3).
 * @see {@link https://github.com/oframe/ogl/blob/master/src/extras/GLTFBlend.js | Source}
 */
export class GLTFBlend {
    animations: GLTFAnimation[];

    constructor(animations: GLTFAnimation[]);

    update(): this;
}
