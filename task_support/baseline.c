#include "pixelforge.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"BASELINE_FAIL line=%d condition=%s\n",__LINE__,#c); return 1; } } while(0)
static void triangle(float z, PFcolor c) {
    pfColor(c);
    pfBegin(PF_TRIANGLES);
    pfVertex3f(-1.0f,-1.0f,z);
    pfVertex3f( 1.0f,-1.0f,z);
    pfVertex3f( 0.0f, 1.0f,z);
    pfEnd();
}
int main(void) {
    enum { W=48, H=40 };
    PFcolor pixels[W*H];
    PFcontext ctx=pfCreateContext(pixels,W,H,PF_RGBA,PF_UNSIGNED_BYTE);
    CHECK(ctx != NULL);
    pfMakeCurrent(ctx);
    pfViewport(0,0,W,H);
    pfMatrixMode(PF_PROJECTION); pfLoadIdentity(); pfFrustum(-1,1,-1,1,1,10);
    pfMatrixMode(PF_MODELVIEW); pfLoadIdentity();
    pfEnable(PF_DEPTH_TEST);
    PFframebuffer f=pfGenFramebuffer(W,H,PF_RGBA,PF_UNSIGNED_BYTE);
    CHECK(pfIsValidFramebuffer(&f));
    pfBindFramebuffer(&f); pfEnable(PF_FRAMEBUFFER);
    pfClearColor(0,0,0,255); pfClear(PF_COLOR_BUFFER_BIT|PF_DEPTH_BUFFER_BIT);
    triangle(-3,(PFcolor){255,0,0,255});
    PFcolor red=pfGetFramebufferPixel(&f,W/2,H/2);
    float back=pfGetFramebufferDepth(&f,W/2,H/2);
    CHECK(red.r>200 && red.g==0 && red.b==0 && isfinite(back));
    triangle(-2,(PFcolor){0,255,0,255});
    PFcolor green=pfGetFramebufferPixel(&f,W/2,H/2);
    float front=pfGetFramebufferDepth(&f,W/2,H/2);
    CHECK(green.r==0 && green.g>200 && green.b==0 && front<back);
    triangle(-4,(PFcolor){0,0,255,255});
    PFcolor after=pfGetFramebufferPixel(&f,W/2,H/2);
    CHECK(after.g>200 && after.b==0 && pfGetFramebufferDepth(&f,W/2,H/2)==front);
    PFrenderlist list=pfGenList(); CHECK(list);
    pfNewList(list); triangle(-2,(PFcolor){0,0,255,255}); pfEndList();
    CHECK(pfGetFramebufferPixel(&f,W/2,H/2).g>200);
    pfClear(PF_COLOR_BUFFER_BIT|PF_DEPTH_BUFFER_BIT); pfCallList(list);
    PFcolor blue=pfGetFramebufferPixel(&f,W/2,H/2);
    CHECK(blue.b>200 && blue.r==0 && blue.g==0);
    CHECK(fabsf(pfGetFramebufferDepth(&f,W/2,H/2)-front)<1e-5f);
    pfDisable(PF_FRAMEBUFFER);
    pfClear(PF_COLOR_BUFFER_BIT|PF_DEPTH_BUFFER_BIT);
    triangle(-2,(PFcolor){255,255,0,255});
    CHECK(pixels[H/2*W+W/2].r>200 && pixels[H/2*W+W/2].g>200);
    CHECK(pfGetFramebufferPixel(&f,W/2,H/2).b>200);
    PFcolor texels[1]={{128,64,32,255}};
    PFtexture tex=pfGenTexture(texels,1,1,PF_RGBA,PF_UNSIGNED_BYTE); CHECK(tex);
    pfBindTexture(tex); pfEnable(PF_TEXTURE_2D);
    pfEnable(PF_LIGHTING); pfLightModel(PF_PHONG); pfShadeModel(PF_SMOOTH);
    pfEnableLight(0);
    PFfloat zero[3]={0,0,0}, redLight[3]={1,0,0};
    pfLightfv(0,PF_POSITION,zero); pfLightfv(0,PF_DIFFUSE,redLight);
    pfLightfv(0,PF_AMBIENT,zero); pfLightfv(0,PF_SPECULAR,zero);
    pfMaterialf(PF_FRONT_AND_BACK,PF_AMBIENT,0);
    pfMaterialf(PF_FRONT_AND_BACK,PF_DIFFUSE,0.5f);
    pfMaterialf(PF_FRONT_AND_BACK,PF_SPECULAR,0);
    pfMaterialf(PF_FRONT_AND_BACK,PF_EMISSION,0);
    pfNormal3f(0,0,1);
    pfClear(PF_COLOR_BUFFER_BIT|PF_DEPTH_BUFFER_BIT);
    triangle(-2,(PFcolor){255,255,255,255});
    PFcolor lit=pixels[H/2*W+W/2];
    CHECK(lit.r>50 && lit.r<70 && lit.g==0 && lit.b==0);
    printf("PHONG_TEXTURE_OK rgb=%u,%u,%u\n",lit.r,lit.g,lit.b);
    pfBindTexture(NULL); pfDeleteTexture(&tex,PF_FALSE);
    CHECK(pfGetError()==PF_NO_ERROR);
    pfDeleteList(&list); pfBindFramebuffer(NULL); pfDeleteFramebuffer(&f); pfDeleteContext(ctx);
    printf("PIXELFORGE_BASELINE_OK framebuffer=isolated depth=near-wins renderlist=replayed\n");
    return 0;
}
