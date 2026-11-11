#include <sndfile.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 SF_INFO info={0}; SF_CUES cues={0},got={0}; short in[4]={1,-2,3,-4},out[4]={0};
 info.samplerate=48000;info.channels=2;info.format=SF_FORMAT_WAV|SF_FORMAT_PCM_16;
 SNDFILE *f=sf_open("cue.wav",SFM_WRITE,&info);if(!f)return 1;
 cues.cue_count=1;cues.cue_points[0].indx=7;cues.cue_points[0].position=1;
 cues.cue_points[0].fcc_chunk=0x61746164;cues.cue_points[0].sample_offset=1;
 strcpy(cues.cue_points[0].name,"Intro");
 if(sf_command(f,SFC_SET_CUE,&cues,sizeof cues)!=SF_TRUE)return 2;
 if(sf_writef_short(f,in,2)!=2)return 3;
 if(sf_close(f))return 4;
 memset(&info,0,sizeof info);f=sf_open("cue.wav",SFM_READ,&info);if(!f)return 5;
 if(sf_command(f,SFC_GET_CUE,&got,sizeof got)!=SF_TRUE)return 6;
 if(got.cue_count!=1||got.cue_points[0].indx!=7||got.cue_points[0].sample_offset!=1)return 7;
 if(got.cue_points[0].name[0]!=0)return 8;
 if(sf_readf_short(f,out,2)!=2||memcmp(in,out,sizeof in))return 9;
 if(sf_close(f))return 10;
 puts("SNDFILE_BASELINE frames=2 cue=7 label=missing PCM=exact");return 0;
}
