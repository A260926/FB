#include "gsm.h"
#include "private.h"
#include "proto.h"
#include <stdio.h>
#include <string.h>
#include "../src/plc_frame.c"

int main(void){
	gsm en=gsm_create(), ew=gsm_create();
	gsm_signal in[320]; gsm_byte a[66], b[66];
	word Ln[8],Nn[4],bn[4],Mn[4],xn[4],xmn[52];
	word Lw[8],Nw[4],bw[4],Mw[4],xw[4],xmw[52];
	unsigned i; int one=1; gsm_byte chain;
	gsm_option(ew,GSM_OPT_WAV49,&one);
	for(i=0;i<320;i++){unsigned t=i;in[i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}
	memset(a,0,sizeof a);memset(b,0,sizeof b);
	gsm_encode(en,in,a); gsm_encode(en,in+160,a+33);
	gsm_encode(ew,in,b); gsm_encode(ew,in+160,b+32);

	/* second frame (odd half) */
	unpack_normal(a+33,Ln,Nn,bn,Mn,xn,xmn);
	unpack_wav_even(b,Lw,Nw,bw,Mw,xw,xmw,&chain);      /* even (frame 0) */
	unpack_wav_odd(chain,b+33,Lw,Nw,bw,Mw,xw,xmw);     /* odd  (frame 1) overwrites */

	int diff=0;
	for(i=0;i<8;i++) if(Ln[i]!=Lw[i]){printf("LARc[%u] n=%d w=%d\n",i,Ln[i],Lw[i]);diff=1;}
	for(i=0;i<4;i++){
	 if(Nn[i]!=Nw[i]){printf("Nc[%u] n=%d w=%d\n",i,Nn[i],Nw[i]);diff=1;}
	 if(bn[i]!=bw[i]){printf("bc[%u] n=%d w=%d\n",i,bn[i],bw[i]);diff=1;}
	 if(Mn[i]!=Mw[i]){printf("Mc[%u] n=%d w=%d\n",i,Mn[i],Mw[i]);diff=1;}
	 if(xn[i]!=xw[i]){printf("xmaxc[%u] n=%d w=%d\n",i,xn[i],xw[i]);diff=1;}
	}
	for(i=0;i<52;i++) if(xmn[i]!=xmw[i]){printf("xmc[%u] n=%d w=%d\n",i,xmn[i],xmw[i]);diff=1;}
	printf(diff?"PARAM_DIFF (odd frame)\n":"PARAM_MATCH (odd frame) chain=%u\n",(unsigned)chain);

	/* first frame (even half) */
	unpack_normal(a,Ln,Nn,bn,Mn,xn,xmn);
	unpack_wav_even(b,Lw,Nw,bw,Mw,xw,xmw,&chain);
	diff=0;
	for(i=0;i<8;i++) if(Ln[i]!=Lw[i]){printf("e LARc[%u] n=%d w=%d\n",i,Ln[i],Lw[i]);diff=1;}
	for(i=0;i<4;i++){
	 if(Nn[i]!=Nw[i]){printf("e Nc[%u] n=%d w=%d\n",i,Nn[i],Nw[i]);diff=1;}
	 if(bn[i]!=bw[i]){printf("e bc[%u] n=%d w=%d\n",i,bn[i],bw[i]);diff=1;}
	 if(Mn[i]!=Mw[i]){printf("e Mc[%u] n=%d w=%d\n",i,Mn[i],Mw[i]);diff=1;}
	 if(xn[i]!=xw[i]){printf("e xmaxc[%u] n=%d w=%d\n",i,xn[i],xw[i]);diff=1;}
	}
	for(i=0;i<52;i++) if(xmn[i]!=xmw[i]){printf("e xmc[%u] n=%d w=%d\n",i,xmn[i],xmw[i]);diff=1;}
	printf(diff?"PARAM_DIFF (even frame)\n":"PARAM_MATCH (even frame)\n");
	gsm_destroy(en);gsm_destroy(ew);
	return 0;
}
