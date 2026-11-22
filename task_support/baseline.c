#include <opusfile.h>
#include <opus.h>
#include <ogg/ogg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#define NPACKETS 40
#define FRAME 960
#define CHANNELS 2
#define DROPPED 20
#define ENDTRIM 137
static void need(int b,const char *s){if(!b){fprintf(stderr,"baseline: %s\n",s);exit(1);}}
static int lengths[NPACKETS];
static unsigned char packets[NPACKETS][4000];
static int preskip;
static long expected;
static float reference[NPACKETS*FRAME*CHANNELS];
static void le32(unsigned char *p,uint32_t v){int i;for(i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static uint32_t rd32(const unsigned char*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void writepage(FILE *f,ogg_page *p){need(fwrite(p->header,1,p->header_len,f)==(size_t)p->header_len,"page header");need(fwrite(p->body,1,p->body_len,f)==(size_t)p->body_len,"page body");}
static void flushpages(ogg_stream_state *os,FILE *clean,FILE *hole,int omit){ogg_page pg;while(ogg_stream_flush(os,&pg)){writepage(clean,&pg);if(!omit)writepage(hole,&pg);}}
static void fixture(void){
 int error,i,c,k;OpusEncoder *enc;OpusDecoder *dec;float in[FRAME*CHANNELS];
 ogg_stream_state os;ogg_packet op;unsigned char head[19]={0};unsigned char tags[16]={0};
 FILE *clean,*hole,*ref;
 enc=opus_encoder_create(48000,CHANNELS,OPUS_APPLICATION_VOIP,&error);need(enc&&error==OPUS_OK,"encoder");
 need(opus_encoder_ctl(enc,OPUS_SET_BITRATE(36000))==0,"bitrate");
 need(opus_encoder_ctl(enc,OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND))==0,"bandwidth");
 need(opus_encoder_ctl(enc,OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE))==0,"signal");
 need(opus_encoder_ctl(enc,OPUS_SET_INBAND_FEC(1))==0,"fec setting");
 need(opus_encoder_ctl(enc,OPUS_SET_PACKET_LOSS_PERC(30))==0,"loss setting");
 need(opus_encoder_ctl(enc,OPUS_GET_LOOKAHEAD(&preskip))==0,"lookahead");
 dec=opus_decoder_create(48000,CHANNELS,&error);need(dec&&error==0,"decoder");
 for(k=0;k<NPACKETS;k++){
  for(i=0;i<FRAME;i++)for(c=0;c<CHANNELS;c++){
   double t=(k*FRAME+i)/48000.;
   in[i*CHANNELS+c]=(float)(.22*sin((117+31*c)*6.283185307179586*t)*(0.75+0.2*sin(2.9*6.283185307179586*t))+.07*sin((421+37*c)*6.283185307179586*t));
  }
  lengths[k]=opus_encode_float(enc,in,FRAME,packets[k],sizeof(packets[k]));need(lengths[k]>0,"encode");
  need(opus_decode_float(dec,packets[k],lengths[k],reference+k*FRAME*CHANNELS,FRAME,0)==FRAME,"reference decode");
 }
 opus_encoder_destroy(enc);opus_decoder_destroy(dec);
 expected=NPACKETS*FRAME-preskip-ENDTRIM;
 unlink(".task-build/source.opus");unlink(".task-build/hole.opus");unlink(".task-build/reference.f32");
 clean=fopen(".task-build/source.opus","wb");hole=fopen(".task-build/hole.opus","wb");need(clean&&hole,"fixture output");
 need(ogg_stream_init(&os,246841)==0,"ogg init");
 memcpy(head,"OpusHead",8);head[8]=1;head[9]=CHANNELS;head[10]=preskip&255;head[11]=(unsigned)preskip>>8;le32(head+12,48000);
 memset(&op,0,sizeof(op));op.packet=head;op.bytes=sizeof(head);op.b_o_s=1;op.packetno=0;need(ogg_stream_packetin(&os,&op)==0,"head packet");flushpages(&os,clean,hole,0);
 memcpy(tags,"OpusTags",8);op.packet=tags;op.bytes=sizeof(tags);op.b_o_s=0;op.packetno=1;need(ogg_stream_packetin(&os,&op)==0,"tags packet");flushpages(&os,clean,hole,0);
 for(k=0;k<NPACKETS;k++){
  op.packet=packets[k];op.bytes=lengths[k];op.packetno=k+2;op.granulepos=(k+1)*FRAME;op.e_o_s=k==NPACKETS-1;
  if(op.e_o_s)op.granulepos-=ENDTRIM;
  need(ogg_stream_packetin(&os,&op)==0,"audio packet");flushpages(&os,clean,hole,k==DROPPED);
 }
 ogg_stream_clear(&os);need(fclose(clean)==0&&fclose(hole)==0,"close fixture");
 ref=fopen(".task-build/reference.f32","wb");need(ref!=NULL,"reference output");
 need(fwrite(reference+preskip*CHANNELS,sizeof(float)*CHANNELS,expected,ref)==(size_t)expected,"reference output bytes");need(fclose(ref)==0,"reference close");
}
struct stream {FILE *f;};
static int read37(void *v,unsigned char*p,int n){struct stream*s=v;size_t r;if(n>37)n=37;r=fread(p,1,n,s->f);return r>0||feof(s->f)?(int)r:-1;}
static void playback(int damaged){
 struct stream s;OpusFileCallbacks cb={read37,NULL,NULL,NULL};OggOpusFile *of;int err,r,li,holes=0;long n=0;float out[274];
 s.f=fopen(damaged?".task-build/hole.opus":".task-build/source.opus","rb");need(s.f!=NULL,"input");
 of=op_open_callbacks(&s,&cb,NULL,0,&err);need(of!=NULL,"open callbacks");need(!op_seekable(of),"nonseekable");
 for(;;){
  r=op_read_float(of,out,sizeof(out)/sizeof(*out),&li);
  if(r==OP_HOLE){holes++;continue;}
  need(r>=0,"read result");if(!r)break;
  if(!damaged){long j;need(n+r<=expected,"output bounds");for(j=0;j<r*CHANNELS;j++)need(fabs(out[j]-reference[(preskip+n)*CHANNELS+j])<2e-6,"PCM reference");}
  n+=r;
 }
 if(!damaged){need(n==expected,"normal frames");need(op_pcm_tell(of)==expected,"normal tell");printf("NORMAL_OPUSFILE frames=%ld fragmented_read=37 PCM_REFERENCE_PASS\n",n);}
 else{printf("LEGACY_HOLE holes=%d frames=%ld intact=%ld\n",holes,n,expected);need(holes>0&&n<expected,"legacy omitted PCM");puts("INBAND_FEC_READER_GAP_CONFIRMED");}
 op_free(of);need(fclose(s.f)==0,"input close");
}
static void wavcheck(const char *path){
 FILE *f=fopen(path,"rb"),*ref;unsigned char h[12],ch[8],fmt[16];unsigned data_bytes=0;long data_pos=0,physical,i;int fmtok=0;float original;
 need(f!=NULL,"WAV output");need(fread(h,1,12,f)==12&&!memcmp(h,"RIFF",4)&&!memcmp(h+8,"WAVE",4),"WAV header");
 need(fseek(f,0,SEEK_END)==0,"WAV physical seek");physical=ftell(f);need((long)rd32(h+4)+8==physical,"fresh RIFF length");need(fseek(f,12,SEEK_SET)==0,"WAV chunks seek");
 while(fread(ch,1,8,f)==8){
  uint32_t len=rd32(ch+4);long next=ftell(f)+len+(len&1);
  need(next<=physical,"chunk range");
  if(!memcmp(ch,"fmt ",4)){need(len>=16&&fread(fmt,1,16,f)==16,"fmt body");need(fmt[0]==1&&fmt[1]==0&&fmt[2]==2&&fmt[3]==0&&rd32(fmt+4)==48000&&fmt[14]==16&&fmt[15]==0,"PCM stereo format");fmtok=1;}
  if(!memcmp(ch,"data",4)){need(!data_pos,"single data");data_bytes=len;data_pos=ftell(f);}
  need(fseek(f,next,SEEK_SET)==0,"next chunk");
 }
 need(fmtok&&data_pos&&data_bytes%4==0,"WAV structure");expected=NPACKETS*FRAME-312-ENDTRIM;
 /*The generated encoder may have a different pre-skip, so take the intact
   reference's exact frame count, rather than guessing it from duration.*/
 ref=fopen(".task-build/reference.f32","rb");need(ref!=NULL,"reference input");need(fseek(ref,0,SEEK_END)==0,"ref length");expected=ftell(ref)/(sizeof(float)*CHANNELS);rewind(ref);
 need(data_bytes/4==(unsigned long)expected,"repaired WAV frame count");need(fseek(f,data_pos,SEEK_SET)==0,"PCM seek");
 for(i=0;i<3000*CHANNELS;i++){
  unsigned char b[2];int got,want;need(fread(b,1,2,f)==2&&fread(&original,sizeof(float),1,ref)==1,"prefix sample");got=(int16_t)((unsigned)b[0]|(unsigned)b[1]<<8);want=(int)lrintf(original*32768.f);need(abs(got-want)<=1,"actual WAV prefix PCM");
 }
 need(fclose(ref)==0&&fclose(f)==0,"WAV close");printf("WAV_CONSUMER frames=%ld prefix_pcm=3000 PASS\n",expected);
}
int main(int argc,char **argv){if(argc>2&&!strcmp(argv[1],"wavcheck")){wavcheck(argv[2]);return 0;}fixture();playback(argc>1&&!strcmp(argv[1],"probe"));return 0;}
