#include "codec_probe.h"
int main(){ISVCEncoder* e=0;if(WelsCreateSVCEncoder(&e)||!e)return 1;
SEncParamExt p;if(!probe_params(e,p))return 2;WelsDestroySVCEncoder(e);
return probe_codec(p,8);}
