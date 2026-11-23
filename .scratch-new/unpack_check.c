#include "gsm.h"
#include "private.h"
#include "proto.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
/* pull in the static unpackers under test */
#include "../src/plc_frame.c"

/* Reference: verbatim copy of original gsm_decode WAV49 odd branch. */
static void ref_odd(uword frame_chain, const gsm_byte * c,
	word *LARc,word*Nc,word*bc,word*Mc,word*xmaxc,word*xmc){
	uword sr = frame_chain;
	sr |= (uword)*c++ << 4;
	LARc[0] = sr & 0x3f; sr >>= 6; LARc[1] = sr & 0x3f; sr >>= 6;
	sr = *c++;
	LARc[2] = sr & 0x1f; sr >>= 5;
	sr |= (uword)*c++ << 3;
	LARc[3] = sr & 0x1f; sr >>= 5;
	LARc[4] = sr & 0xf; sr >>= 4;
	sr |= (uword)*c++ << 2;
	LARc[5] = sr & 0xf; sr >>= 4;
	LARc[6] = sr & 0x7; sr >>= 3;
	LARc[7] = sr & 0x7; sr >>= 3;
	sr = *c++;
	Nc[0] = sr & 0x7f; sr >>= 7; sr |= (uword)*c++ << 1;
	bc[0] = sr & 0x3; sr >>= 2; Mc[0] = sr & 0x3; sr >>= 2;
	sr |= (uword)*c++ << 5;
	xmaxc[0] = sr & 0x3f; sr >>= 6;
	xmc[0]=sr&7;sr>>=3;xmc[1]=sr&7;sr>>=3;sr|=(uword)*c++<<1;
	xmc[2]=sr&7;sr>>=3;xmc[3]=sr&7;sr>>=3;xmc[4]=sr&7;sr>>=3;
	sr=*c++;xmc[5]=sr&7;sr>>=3;xmc[6]=sr&7;sr>>=3;sr|=(uword)*c++<<2;
	xmc[7]=sr&7;sr>>=3;xmc[8]=sr&7;sr>>=3;xmc[9]=sr&7;sr>>=3;
	sr|=(uword)*c++<<1;xmc[10]=sr&7;sr>>=3;xmc[11]=sr&7;sr>>=3;xmc[12]=sr&7;sr>>=3;
	sr=*c++;Nc[1]=sr&0x7f;sr>>=7;sr|=(uword)*c++<<1;bc[1]=sr&3;sr>>=2;Mc[1]=sr&3;sr>>=2;
	sr|=(uword)*c++<<5;xmaxc[1]=sr&0x3f;sr>>=6;xmc[13]=sr&7;sr>>=3;xmc[14]=sr&7;sr>>=3;
	sr|=(uword)*c++<<1;xmc[15]=sr&7;sr>>=3;xmc[16]=sr&7;sr>>=3;xmc[17]=sr&7;sr>>=3;
	sr=*c++;xmc[18]=sr&7;sr>>=3;xmc[19]=sr&7;sr>>=3;sr|=(uword)*c++<<2;
	xmc[20]=sr&7;sr>>=3;xmc[21]=sr&7;sr>>=3;xmc[22]=sr&7;sr>>=3;
	sr|=(uword)*c++<<1;xmc[23]=sr&7;sr>>=3;xmc[24]=sr&7;sr>>=3;xmc[25]=sr&7;sr>>=3;
	sr=*c++;Nc[2]=sr&0x7f;sr>>=7;sr|=(uword)*c++<<1;bc[2]=sr&3;sr>>=2;Mc[2]=sr&3;sr>>=2;
	sr|=(uword)*c++<<5;xmaxc[2]=sr&0x3f;sr>>=6;xmc[26]=sr&7;sr>>=3;xmc[27]=sr&7;sr>>=3;
	sr|=(uword)*c++<<1;xmc[28]=sr&7;sr>>=3;xmc[29]=sr&7;sr>>=3;xmc[30]=sr&7;sr>>=3;
	sr=*c++;xmc[31]=sr&7;sr>>=3;xmc[32]=sr&7;sr>>=3;sr|=(uword)*c++<<2;
	xmc[33]=sr&7;sr>>=3;xmc[34]=sr&7;sr>>=3;xmc[35]=sr&7;sr>>=3;
	sr|=(uword)*c++<<1;xmc[36]=sr&7;sr>>=3;xmc[37]=sr&7;sr>>=3;xmc[38]=sr&7;sr>>=3;
	sr=*c++;Nc[3]=sr&0x7f;sr>>=7;sr|=(uword)*c++<<1;bc[3]=sr&3;sr>>=2;Mc[3]=sr&3;sr>>=2;
	sr|=(uword)*c++<<5;xmaxc[3]=sr&0x3f;sr>>=6;xmc[39]=sr&7;sr>>=3;xmc[40]=sr&7;sr>>=3;
	sr|=(uword)*c++<<1;xmc[41]=sr&7;sr>>=3;xmc[42]=sr&7;sr>>=3;
	sr=*c++;xmc[43]=sr&7;sr>>=3;xmc[44]=sr&7;sr>>=3;sr|=(uword)*c++<<2;
	xmc[45]=sr&7;sr>>=3;xmc[46]=sr&7;sr>>=3;xmc[47]=sr&7;sr>>=3;
	sr|=(uword)*c++<<1;xmc[48]=sr&7;sr>>=3;xmc[49]=sr&7;sr>>=3;xmc[50]=sr&7;sr>>=3;xmc[51]=sr&7;sr>>=3;
}

int main(void){
	gsm en=gsm_create(), ew=gsm_create();
	gsm_signal in[320]; gsm_byte b[66];
	word L1[8],N1[4],b1[4],M1[4],x1[4],xm1[52];
	word L2[8],N2[4],b2[4],M2[4],x2[4],xm2[52];
	unsigned i; int one=1; gsm_byte chain;
	gsm_option(ew,GSM_OPT_WAV49,&one);
	for(i=0;i<320;i++){unsigned t=i;in[i]=(gsm_signal)((int)((t*73+t*t*3)%24001)-12000);}
	memset(b,0,sizeof b);
	gsm_encode(ew,in,b); gsm_encode(ew,in+160,b+32);
	unpack_wav_even(b,L1,N1,b1,M1,x1,xm1,&chain);
	ref_odd((uword)chain,b+33,L2,N2,b2,M2,x2,xm2);
	unpack_wav_odd(chain,b+33,L1,N1,b1,M1,x1,xm1);
	int diff=0;
	for(i=0;i<8;i++) if(L1[i]!=L2[i]){printf("LARc[%u] mine=%d ref=%d\n",i,L1[i],L2[i]);diff=1;}
	for(i=0;i<4;i++){
	 if(N1[i]!=N2[i]){printf("Nc[%u] mine=%d ref=%d\n",i,N1[i],N2[i]);diff=1;}
	 if(b1[i]!=b2[i]){printf("bc[%u] mine=%d ref=%d\n",i,b1[i],b2[i]);diff=1;}
	 if(M1[i]!=M2[i]){printf("Mc[%u] mine=%d ref=%d\n",i,M1[i],M2[i]);diff=1;}
	 if(x1[i]!=x2[i]){printf("xmaxc[%u] mine=%d ref=%d\n",i,x1[i],x2[i]);diff=1;}
	}
	for(i=0;i<52;i++) if(xm1[i]!=xm2[i]){printf("xmc[%u] mine=%d ref=%d\n",i,xm1[i],xm2[i]);diff=1;}
	printf(diff?"ODD_UNPACK_DIFFERS chain=%u\n":"ODD_UNPACK_MATCH chain=%u\n",(unsigned)chain);
	gsm_destroy(en);gsm_destroy(ew);
	return diff;
}
