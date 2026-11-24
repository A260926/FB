#include "codec_probe.h"
int main(){ISVCEncoder* e=0;if(WelsCreateSVCEncoder(&e)||!e)return 1;
SEncParamExt p;if(!probe_params(e,p)||p.bEnableScalingLists)return 2;
p.bEnableScalingLists=true;
for(int c=0;c<6;++c)for(int i=0;i<16;++i)p.uiScalingLists4x4[c][i]=(unsigned char)(8+c+i);
p.uiScalingLists4x4[2][7]=0;
if(e->InitializeExt(&p)==cmResultSuccess)return 3;
e->Uninitialize();WelsDestroySVCEncoder(e);
p.uiScalingLists4x4[2][7]=17;
return probe_codec(p,2);}
