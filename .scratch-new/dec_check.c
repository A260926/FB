#include "gsm.h"
#include "private.h"
#include "proto.h"
#include <stdio.h>
#include <string.h>
#include "../src/plc_frame.c"

int main(void){
	gsm ew=gsm_create(), d=gsm_create();
	gsm_signal in[320], got[320], ref[320];
	gsm_byte b[66];
	word L[8],N[4],bc[4],M[4],x[4],xm[52];
	unsigned i; int one=1; gsm_byte chain;
	gsm_option(ew,GSM_OPT_WAV49,&one);
	gsm_option(d,GSM_OPT_WAV49,&one);
	for(i=0;i<320;i++){unsigned t=i;in[i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}
	memset(b,0,sizeof b);
	gsm_encode(ew,in,b); gsm_encode(ew,in+160,b+32);

	/* my path */
	if(gsm_decode(d,b,got)||gsm_decode(d,b+33,got+160)){printf("decode err\n");return 9;}

	/* reference path: my unpackers + stock Gsm_Decoder on a fresh state */
	gsm r=gsm_create();
	unpack_wav_even(b,L,N,bc,M,x,xm,&chain);
	Gsm_Decoder(r,L,N,bc,M,x,xm,ref);
	unpack_wav_odd(chain,b+33,L,N,bc,M,x,xm);
	Gsm_Decoder(r,L,N,bc,M,x,xm,ref+160);

	int diff=0,first=-1;
	for(i=0;i<320;i++){ if(got[i]!=ref[i]){if(first<0)first=(int)i;diff++;} }
	printf("wav block0 diffs=%d first=%d (got=%d ref=%d)\n",diff,first,
		first>=0?got[first]:0,first>=0?ref[first]:0);
	/* also compare normal Gsm_Decoder vs gsm_decode on a normal frame */
	gsm en=gsm_create(), dn=gsm_create(), rn=gsm_create();
	gsm_byte a[33]; gsm_signal gn[160],rf[160];
	gsm_encode(en,in,a);
	gsm_decode(dn,a,gn);
	unpack_normal(a,L,N,bc,M,x,xm);
	Gsm_Decoder(rn,L,N,bc,M,x,xm,rf);
	int nd=0;for(i=0;i<160;i++)if(gn[i]!=rf[i])nd++;
	printf("normal frame diffs=%d\n",nd);
	gsm_destroy(ew);gsm_destroy(d);gsm_destroy(r);gsm_destroy(en);gsm_destroy(dn);gsm_destroy(rn);
	return diff!=0;
}
