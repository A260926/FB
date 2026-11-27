# Joint arc length animation retiming

`RetimingByJointArcLength` (declared in
`Jolt/Skeleton/ArcLengthRetimer.h`) is an offline tool that re-times a
`SkeletalAnimation` so that a point attached to one joint travels equal model
space arc length in equal output time. The same source-to-output time mapping
is applied to every joint track, so the whole character keeps its relative
timing while the measured point moves at constant speed *at the produced key
times*.

It does not straighten the motion into a line between endpoints and it does not
only move the root bone: the measured curve is the genuine model space path

```
P(t) = M_joint(t) * localPoint
```

integrated along the full ancestor chain of the joint (`SkeletonPose` root
offset excluded).

## Building and running

```bash
bash support/run.sh target
```

This first builds and runs the unchanged `skeleton_baseline` program, then the
`skeleton_target` verification program in `support/target.cpp`. A successful run
ends with `JOLT_ARCLENGTH_OK`.

## Calling the function

```cpp
#include <Jolt/Skeleton/ArcLengthRetimer.h>

ArcLengthSettings settings;
settings.mJointIndex     = hand_joint_index;   // Joint carrying the measured point
settings.mLocalPoint     = Vec3(0.1f, 0, 0);   // Point in that joint's local space
settings.mSampleCount    = 17;                 // Output keys (>= 2)
settings.mOutputDuration = 1.5f;               // Output clip length in seconds (> 0)
settings.mLengthTolerance = 1.0e-4f;           // Arc length tolerance (see below)
settings.mMaxEvaluations = 200000;             // Pose evaluation budget for integration

ArcLengthResult result;
if (RetimingByJointArcLength(*source_clip, rest_pose, settings, result))
{
	// result.mAnimation     : non looping clip, one track per skeleton joint,
	//                          joints kept in skeleton order, all rotation and
	//                          translation channels present
	// result.mSourceTimes[i]: source time that output key i was sampled from
	// result.mTotalLength    : total model space arc length of P(t) on [0, D]
}
```

The source animation, the rest pose and their looping flags are never
modified. On any failure the function returns `false` and `result` is left
exactly as it was before the call.

Each source time is evaluated from scratch starting from the rest pose local
joint states, applying the clip's own rules (linear translation, shortest path
SLERP) and rebuilding the model transforms up the parent chain. Joints without
a track therefore keep their rest pose local state instead of inheriting a
value from an earlier sample. Even when the input clip is looping it is
evaluated on the closed interval `[0, D]`, so the final output key holds the
genuine end pose rather than the wrapped start pose.

## Stationary segments and the rightmost solution

Output key `i` corresponds to cumulative length `q * L(D)` where
`q = i / (sampleCount - 1)`. When the point does not move for a while, `L(t)`
is flat over that interval and a single length value has many solutions. The
retimer always takes the **rightmost** solution, i.e. it jumps to the *end* of
the stationary interval instead of lingering at its start.

Example: a hand swings for `t in [0, 0.3]`, holds completely still for
`t in [0.3, 0.6]`, then swings again. Half of the total arc length is reached
at `t = 0.3`, so the midpoint output key (`q = 0.5`) is assigned source time
`0.6` (the end of the pause), and the surrounding keys fall on either side of
it. A fully stationary curve (`L(D) == 0`) has no meaningful arc parameter, in
which case the uniform mapping `sourceTimes[i] = q * D` is used.

## Error units and what is guaranteed

- `mLengthTolerance` and `ArcLengthResult::mTotalLength` are expressed in the
  same **model space length units** as the joint translations and
  `mLocalPoint` (e.g. metres if the asset is authored in metres). They are not
  angles and not normalized.
- Arc length is computed with adaptive dyadic chord sums. The internal table is
  built to one eighth of the requested tolerance, and the total length plus the
  cumulative length at every internal output key are checked against
  `mLengthTolerance`. The pose evaluations used for integration and inversion
  together may not exceed `mMaxEvaluations`; running out of budget makes the
  function fail rather than return an approximate result that claims the
  tolerance was met.
- Equal arc length spacing is guaranteed **only at the output key times**.
  Between two consecutive output keys the output clip is interpolated with the
  ordinary library rules, so instantaneous speed inside an interval is not
  forced to be constant; no infinite-precision constant velocity is promised
  between keys.
