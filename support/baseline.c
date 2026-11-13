#include <sndfile.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
int main(void){
 SF_INFO info={0};info.samplerate=48000;info.channels=2;info.format=SF_FORMAT_WAV|SF_FORMAT_PCM_16;
 double in[64];int out[64];for(int i=0;i<64;i++)in[i]=((i%11)-5)/16.0;
 SNDFILE *f=sf_open("baseline.wav",SFM_WRITE,&info);if(!f)return 2;
 if(sf_writef_double(f,in,32)!=32||sf_close(f))return 3;
 memset(&info,0,sizeof info);f=sf_open("baseline.wav",SFM_READ,&info);if(!f)return 4;
 if(sf_readf_int(f,out,32)!=32)return 5;
 for(int i=0;i<64;i++)if(out[i]!=(int)(in[i]*2147483648.0))return 6;
 if(sf_seek(f,17,SEEK_SET)!=17)return 7;
 if(sf_readf_int(f,out,1)!=1||out[0]!=(int)(in[34]*2147483648.0))return 8;
 sf_close(f);remove("baseline.wav");
 SF_DITHER_INFO di={SFD_TRIANGULAR_PDF,1.0,0};
 info.format=SF_FORMAT_WAV|SF_FORMAT_PCM_16;f=sf_open("dither.wav",SFM_WRITE,&info);if(!f)return 9;
 if(sf_command(f,SFC_SET_DITHER_ON_WRITE,&di,sizeof di))return 10;
 for(int i=0;i<64;i++)in[i]=0.25/32768.0;
 if(sf_writef_double(f,in,32)!=32||sf_close(f))return 11;
 memset(&info,0,sizeof info);f=sf_open("dither.wav",SFM_READ,&info);if(!f)return 12;
 if(sf_readf_int(f,out,32)!=32)return 13;for(int i=0;i<64;i++)if(out[i])return 14;
 sf_close(f);remove("dither.wav");puts("DITHER_BASELINE triangular-currently-copy=1");
 puts("PCM_BASELINE frames=32 channels=2 exact=1 seek=17");return 0;
}
