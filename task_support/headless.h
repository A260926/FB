#ifndef TASK_HEADLESS_H
#define TASK_HEADLESS_H
/* CPU declarations from goxel.h/render.h; no graphics/window implementation. */
#define GOXEL_H
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include "volume.h"
#include "volume_utils.h"
#include "block_def.h"
#include "utils/vec.h"
#include "utils/color.h"
#define BLOCK_SIZE 16
#define VOXEL_TEXTURE_SIZE 8
#define EFFECT_MARCHING_CUBES (1 << 7)
#define EFFECT_MC_SMOOTH (1 << 9)
#define min(a,b) ({__typeof__(a) _a=(a);__typeof__(b) _b=(b);_a<_b?_a:_b;})
#define max(a,b) ({__typeof__(a) _a=(a);__typeof__(b) _b=(b);_a>_b?_a:_b;})
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define SWAP(a,b) do{__typeof__(a) _t=(a);(a)=(b);(b)=_t;}while(0)
#endif
