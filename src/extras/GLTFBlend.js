// Blend several GLTFAnimation clips into one simultaneous local pose.
//
// Unlike GLTFAnimation.update, which writes each clip straight onto the nodes
// (so call order changes the result and a clip missing a channel dilutes the
// others), GLTFBlend samples every clip independently and writes each node
// property exactly once per update. It only touches the plain channel data
// produced by GLTFLoader, so it needs neither a render frame nor a GL device.

const PROPERTIES = ['position', 'quaternion', 'scale'];

// Resolve the shared sampler time of an animation from its own elapsed/loop
// state without advancing anything. Mirrors the glTF clip timeline described
// by startTime/endTime.
function sampleTime(animation) {
    const { duration, startTime } = animation;
    if (!duration) return startTime;

    let local;
    if (animation.loop) {
        // Positive modulo so negative elapsed values wrap the same way.
        local = animation.elapsed % duration;
        if (local < 0) local += duration;
    } else {
        local = Math.min(Math.max(animation.elapsed, 0), duration);
    }
    return startTime + local;
}

function readKeyValue(values, keyIndex, size, interpolation) {
    const offset = interpolation === 'CUBICSPLINE' ? keyIndex * size * 3 + size : keyIndex * size;
    const out = new Array(size);
    for (let i = 0; i < size; i++) out[i] = values[offset + i];
    return out;
}

function lerpValues(a, b, alpha, size) {
    const out = new Array(size);
    for (let i = 0; i < size; i++) out[i] = a[i] + (b[i] - a[i]) * alpha;
    return out;
}

// Shortest-arc spherical interpolation; q and -q describe the same rotation.
function slerpQuat(a, b, alpha) {
    let bx = b[0],
        by = b[1],
        bz = b[2],
        bw = b[3];
    let cosom = a[0] * bx + a[1] * by + a[2] * bz + a[3] * bw;
    if (cosom < 0) {
        cosom = -cosom;
        bx = -bx;
        by = -by;
        bz = -bz;
        bw = -bw;
    }

    let scale0, scale1;
    if (1 - cosom > 1e-10) {
        const omega = Math.acos(cosom);
        const sinom = Math.sin(omega);
        scale0 = Math.sin((1 - alpha) * omega) / sinom;
        scale1 = Math.sin(alpha * omega) / sinom;
    } else {
        scale0 = 1 - alpha;
        scale1 = alpha;
    }

    return [scale0 * a[0] + scale1 * bx, scale0 * a[1] + scale1 * by, scale0 * a[2] + scale1 * bz, scale0 * a[3] + scale1 * bw];
}

// Sample one glTF channel at an already resolved absolute clip time.
// Endpoints are held outside the key range, single keys stay constant.
function sampleChannel(channel, time) {
    const { times, values, interpolation, transform } = channel;
    const size = transform === 'quaternion' ? 4 : 3;
    const keyCount = times.length;

    if (keyCount === 1 || time <= times[0]) return readKeyValue(values, 0, size, interpolation);
    if (time >= times[keyCount - 1]) return readKeyValue(values, keyCount - 1, size, interpolation);

    // Times are strictly increasing: find the first key after the sample.
    let nextIndex = 1;
    while (times[nextIndex] <= time) nextIndex++;
    const prevIndex = nextIndex - 1;
    const segment = times[nextIndex] - times[prevIndex];
    const alpha = (time - times[prevIndex]) / segment;

    if (interpolation === 'STEP') {
        return readKeyValue(values, prevIndex, size, interpolation);
    }

    if (interpolation === 'CUBICSPLINE') {
        const base = prevIndex * size * 3;
        const nextBase = nextIndex * size * 3;
        const prevValue = new Array(size);
        const prevOutTangent = new Array(size);
        const nextInTangent = new Array(size);
        const nextValue = new Array(size);
        for (let i = 0; i < size; i++) {
            prevValue[i] = values[base + size + i];
            prevOutTangent[i] = values[base + size * 2 + i];
            nextInTangent[i] = values[nextBase + i];
            nextValue[i] = values[nextBase + size + i];
        }

        // glTF Hermite basis; the segment duration in seconds scales the tangents.
        const t2 = alpha * alpha;
        const t3 = t2 * alpha;
        const s0 = 2 * t3 - 3 * t2 + 1;
        const s1 = t3 - 2 * t2 + alpha;
        const s2 = -2 * t3 + 3 * t2;
        const s3 = t3 - t2;

        const out = new Array(size);
        for (let i = 0; i < size; i++) {
            out[i] =
                s0 * prevValue[i] +
                s1 * segment * prevOutTangent[i] +
                s2 * nextValue[i] +
                s3 * segment * nextInTangent[i];
        }
        if (size === 4) normalizeQuat(out);
        return out;
    }

    // LINEAR
    const prev = readKeyValue(values, prevIndex, size, interpolation);
    const next = readKeyValue(values, nextIndex, size, interpolation);
    if (size === 4) return slerpQuat(prev, next, alpha);
    return lerpValues(prev, next, alpha, size);
}

function dot4(a, b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

function normalizeQuat(q) {
    const len = Math.hypot(q[0], q[1], q[2], q[3]) || 1;
    q[0] /= len;
    q[1] /= len;
    q[2] /= len;
    q[3] /= len;
    return q;
}

function quatMultiply(a, b) {
    return [
        a[0] * b[3] + a[3] * b[0] + a[1] * b[2] - a[2] * b[1],
        a[1] * b[3] + a[3] * b[1] + a[2] * b[0] - a[0] * b[2],
        a[2] * b[3] + a[3] * b[2] + a[0] * b[1] - a[1] * b[0],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    ];
}

// Logarithm of a unit quaternion into its tangent space at the identity.
function quatLog(q) {
    const r = Math.hypot(q[0], q[1], q[2]);
    if (r < 1e-12) return [q[0], q[1], q[2]];
    const halfAngle = Math.atan2(r, q[3]);
    const scale = halfAngle / r;
    return [q[0] * scale, q[1] * scale, q[2] * scale];
}

function quatExp(v) {
    const r = Math.hypot(v[0], v[1], v[2]);
    if (r < 1e-12) return [v[0], v[1], v[2], 1];
    const scale = Math.sin(r) / r;
    return [v[0] * scale, v[1] * scale, v[2] * scale, Math.cos(r)];
}

// Intrinsic weighted mean on SO(3): minimise the weighted sum of squared
// shortest-arc geodesic distances. Every sample is within pi/2 of the static
// rotation, so this is the unique mean region. Karcher iterations drive the
// weighted mean log residual to zero; neither sequential slerp nor a
// normalized weighted sum is used.
function quaternionMean(weightedSamples) {
    const reference = weightedSamples[0].q;

    // Initial estimate: normalized weighted sum with sign ambiguity resolved
    // against the reference so equivalent quaternions share a hemisphere.
    const mean = [0, 0, 0, 0];
    for (const { q, weight } of weightedSamples) {
        const sign = dot4(q, reference) < 0 ? -1 : 1;
        for (let i = 0; i < 4; i++) mean[i] += weight * sign * q[i];
    }
    normalizeQuat(mean);

    for (let iteration = 0; iteration < 100; iteration++) {
        const logSum = [0, 0, 0];
        for (const { q, weight } of weightedSamples) {
            let oriented = q;
            if (dot4(mean, q) < 0) oriented = [-q[0], -q[1], -q[2], -q[3]];
            // log(mean^-1 * q): shortest tangent vector from the current mean.
            const relative = quatMultiply([-mean[0], -mean[1], -mean[2], mean[3]], oriented);
            const log = quatLog(relative);
            logSum[0] += weight * log[0];
            logSum[1] += weight * log[1];
            logSum[2] += weight * log[2];
        }

        const residual = Math.hypot(logSum[0], logSum[1], logSum[2]);
        if (residual < 1e-12) break;

        const correction = quatExp(logSum);
        const next = quatMultiply(mean, correction);
        mean[0] = next[0];
        mean[1] = next[1];
        mean[2] = next[2];
        mean[3] = next[3];
        normalizeQuat(mean);
    }

    return mean;
}

function blendVector(staticValue, samples, totalWeight) {
    const out = [0, 0, 0];
    if (totalWeight <= 1) {
        const staticWeight = 1 - totalWeight;
        for (let i = 0; i < 3; i++) out[i] = staticWeight * staticValue[i];
        for (const { weight, value } of samples) {
            for (let i = 0; i < 3; i++) out[i] += weight * value[i];
        }
    } else {
        const inverseTotal = 1 / totalWeight;
        for (const { weight, value } of samples) {
            const normalized = weight * inverseTotal;
            for (let i = 0; i < 3; i++) out[i] += normalized * value[i];
        }
    }
    return out;
}

function blendRotation(staticValue, samples, totalWeight) {
    const weighted = [];
    if (totalWeight <= 1) {
        const staticWeight = 1 - totalWeight;
        if (staticWeight > 0) weighted.push({ q: staticValue, weight: staticWeight });
        for (const { weight, value } of samples) weighted.push({ q: value, weight });
    } else {
        const inverseTotal = 1 / totalWeight;
        for (const { weight, value } of samples) weighted.push({ q: value, weight: weight * inverseTotal });
    }

    if (!weighted.length) return staticValue.slice();
    return quaternionMean(weighted);
}

function capture(value) {
    return typeof value.toArray === 'function' ? value.toArray() : Array.from(value);
}

export class GLTFBlend {
    constructor(animations) {
        // Fix the set of clips at construction; later caller mutations of the
        // passed array cannot change what an update considers.
        this.animations = Array.from(animations);

        // node identity -> { position?, quaternion?, scale? }
        // Each record keeps the captured static value and every owning channel.
        this.targets = new Map();

        for (const animation of this.animations) {
            for (const channel of animation.data) {
                const { node, transform } = channel;
                if (!PROPERTIES.includes(transform)) continue;

                let nodeState = this.targets.get(node);
                if (!nodeState) {
                    nodeState = {};
                    this.targets.set(node, nodeState);
                }

                if (!nodeState[transform]) {
                    nodeState[transform] = {
                        static: capture(node[transform]),
                        channels: [],
                    };
                }
                nodeState[transform].channels.push({ animation, channel });
            }
        }
    }

    update() {
        // Sample everything first; nodes are written once at the end, so clip
        // order can never leak into the resulting pose.
        const writes = [];

        for (const [node, nodeState] of this.targets) {
            for (const transform of PROPERTIES) {
                const record = nodeState[transform];
                if (!record) continue;

                const samples = [];
                let totalWeight = 0;

                for (const { animation, channel } of record.channels) {
                    const weight = animation.weight;
                    // Zero-weight clips take no part in sampling or normalization.
                    if (!(weight > 0)) continue;

                    const time = sampleTime(animation);
                    samples.push({ weight, value: sampleChannel(channel, time) });
                    totalWeight += weight;
                }

                const value =
                    transform === 'quaternion'
                        ? blendRotation(record.static, samples, totalWeight)
                        : blendVector(record.static, samples, totalWeight);

                writes.push({ target: node[transform], value });
            }
        }

        for (const { target, value } of writes) {
            if (typeof target.set === 'function') target.set(value);
            else for (let i = 0; i < value.length; i++) target[i] = value[i];
        }

        return this;
    }
}
