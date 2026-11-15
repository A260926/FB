#ifndef IQM_TASK_DEMO_API_H
#define IQM_TASK_DEMO_API_H
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "demo/util.h"
#include "demo/geom.h"
#include "demo/iqm.h"
extern uchar *meshdata, *animdata;
extern float *inposition, *innormal, *intangent, *intexcoord;
extern uchar *inblendindex, *inblendweight;
extern float *outposition, *outnormal, *outtangent, *outbitangent;
extern int numverts, numjoints, numframes, numanims;
extern iqmjoint *joints;
extern iqmanim *anims;
extern iqmtriangle *tris;
extern Matrix3x4 *baseframe, *inversebaseframe, *outframe, *frames;
bool loadiqm(const char *filename);
void animateiqm(float frame);
void cleanupiqm();
#endif
