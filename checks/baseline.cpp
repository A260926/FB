#include "imgui.h"
#include "imgui_internal.h"
#include <cassert>
#include <cstdio>
int main(){
 ImDrawListSharedData shared;shared.InitialDrawFlags=0;
 ImDrawList list(&shared);list._ResetForNewFrame();list._SetPixelDensity(1);
 list.PushClipRect(ImVec2(0,0),ImVec2(32,32));list.PushTexture(ImTextureRef((ImTextureID)11));
 list.AddRectFilled(ImVec2(1,1),ImVec2(8,8),IM_COL32(255,0,0,255));
 list.AddTriangleFilled(ImVec2(12,1),ImVec2(20,1),ImVec2(12,9),IM_COL32(0,255,0,255));
 assert(list.VtxBuffer.Size==7 && list.IdxBuffer.Size==9);unsigned count=0;
 for(const ImDrawCmd& c:list.CmdBuffer){if(!c.ElemCount)continue;assert(c.GetTexID()==11&&c.VtxOffset==0);for(unsigned k=0;k<c.ElemCount;k++)assert(c.VtxOffset+list.IdxBuffer[c.IdxOffset+k]<(unsigned)list.VtxBuffer.Size);count+=c.ElemCount;}
 assert(count==9);std::puts("DRAW_GEOMETRY vertices=7 indices=9 texture=11");std::puts("BASELINE_OK");
}
