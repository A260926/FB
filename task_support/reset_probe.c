#include "speex/speex.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"probe:%d: %s\n",__LINE__,#x);return 1;} } while(0)
static void signal_frame(spx_int16_t *p,int n,int frame,int seed) {
    int i; for(i=0;i<n;++i) { int k=frame*n+i; p[i]=(spx_int16_t)(((k*(13+seed))%201-100)*35+((k*(3+seed))%57-28)*18); }
}
static int config(void *s, int enc, int vbr) {
    int q=6,complexity=3,on=1; float vq=6.25f;
    if(enc) {
        if(speex_encoder_ctl(s,SPEEX_SET_QUALITY,&q)||speex_encoder_ctl(s,SPEEX_SET_COMPLEXITY,&complexity)) return 1;
        if(speex_encoder_ctl(s,SPEEX_SET_VBR,&vbr)||speex_encoder_ctl(s,SPEEX_SET_VBR_QUALITY,&vq)) return 1;
        if(vbr && (speex_encoder_ctl(s,SPEEX_SET_VAD,&on)||speex_encoder_ctl(s,SPEEX_SET_DTX,&on))) return 1;
    } else if(speex_decoder_ctl(s,SPEEX_SET_ENH,&on)) return 1;
    return 0;
}
static int run(int mode,int vbr,int repro,unsigned long *et,unsigned long *dt) {
    const SpeexMode *m=speex_lib_get_mode(mode);
    void *ef=speex_encoder_init(m),*er=speex_encoder_init(m),*df=speex_decoder_init(m),*dr=speex_decoder_init(m);
    SpeexBits a,b,c; spx_int16_t in[640],copy[640],fresh[640],old[640];
    int fs=0,k,i,an,bn; unsigned long em=0,dm=0,nonzero=0; char ab[1024],bb[1024];
    CHECK(ef&&er&&df&&dr); CHECK(!config(ef,1,vbr)&&!config(er,1,vbr)&&!config(df,0,vbr)&&!config(dr,0,vbr));
    CHECK(!speex_encoder_ctl(ef,SPEEX_GET_FRAME_SIZE,&fs)); CHECK(fs>0&&fs<=640);
    speex_bits_init(&a);speex_bits_init(&b);speex_bits_init(&c);
    if(repro) {
        for(k=0;k<25;++k) {
            signal_frame(in,fs,k,7); if(k>12) memset(in,0,(size_t)fs*sizeof(*in));
            speex_bits_reset(&a);CHECK(speex_encode_int(er,in,&a)>=0);
            speex_bits_rewind(&a);CHECK(!speex_decode_int(dr,&a,old));
        }
        for(k=0;k<3;++k) CHECK(!speex_decode_int(dr,NULL,old));
        CHECK(!speex_encoder_ctl(er,SPEEX_RESET_STATE,NULL));CHECK(!speex_decoder_ctl(dr,SPEEX_RESET_STATE,NULL));
    }
    for(k=0;k<8;++k) {
        signal_frame(in,fs,k,2);memcpy(copy,in,(size_t)fs*sizeof(*in));
        speex_bits_reset(&a);CHECK(speex_encode_int(ef,in,&a)>=0);an=speex_bits_write(&a,ab,sizeof(ab));CHECK(an>0);
        if(repro) {
            speex_bits_reset(&b);CHECK(speex_encode_int(er,copy,&b)>=0);bn=speex_bits_write(&b,bb,sizeof(bb));CHECK(bn>0);
            if(a.nbBits!=b.nbBits||an!=bn||memcmp(ab,bb,(size_t)an)) ++em;
        }
        speex_bits_read_from(&c,ab,an);CHECK(!speex_decode_int(df,&c,fresh));
        if(repro) { speex_bits_read_from(&c,ab,an);CHECK(!speex_decode_int(dr,&c,old)); }
        for(i=0;i<fs;++i) {if(fresh[i]) ++nonzero;if(repro&&old[i]!=fresh[i])++dm;}
    }
    CHECK(nonzero>0);
    printf("RESET_OBS mode=%s kind=%s encoded_frame_mismatches=%lu decoded_sample_mismatches=%lu nonzero=%lu\n",mode==0?"NB":mode==1?"WB":"UWB",vbr?"VBR":"CBR",em,dm,nonzero);
    *et+=em;*dt+=dm;
    speex_bits_destroy(&a);speex_bits_destroy(&b);speex_bits_destroy(&c);
    speex_encoder_destroy(ef);speex_encoder_destroy(er);speex_decoder_destroy(df);speex_decoder_destroy(dr);
    return 0;
}
static void u16(FILE*f,unsigned x){fputc((int)(x&255),f);fputc((int)((x>>8)&255),f);}
static void u32(FILE*f,uint32_t x){u16(f,x&65535);u16(f,x>>16);}
static int fixture(void) {
    int i;spx_int16_t a[160];FILE *f=fopen(".task-build/input.wav","wb");CHECK(f);
    fwrite("RIFF",1,4,f);u32(f,36+5120);fwrite("WAVEfmt ",1,8,f);u32(f,16);u16(f,1);u16(f,1);u32(f,8000);u32(f,16000);u16(f,2);u16(f,16);fwrite("data",1,4,f);u32(f,5120);
    for(i=0;i<16;++i){int j;signal_frame(a,160,i,2);for(j=0;j<160;++j)u16(f,(uint16_t)a[j]);}
    CHECK(!ferror(f));CHECK(!fclose(f));puts("INPUT_WAVE_OK frames=2560");return 0;
}
int main(int argc,char**argv) {
    int m,v,repro;unsigned long e=0,d=0;
    if(argc!=2) return 2;if(!strcmp(argv[1],"fixture")) return fixture();
    repro=!strcmp(argv[1],"repro");if(!repro&&strcmp(argv[1],"normal"))return 2;
    for(m=0;m<3;++m)for(v=0;v<2;++v)if(run(m,v,repro,&e,&d))return 1;
    if(repro) {CHECK(e+d>0);printf("RESET_GAP_CONFIRMED encoder_frames=%lu decoder_samples=%lu\n",e,d);}
    else puts("SPEEX_NORMAL_OK modes=3 configurations=6 nonzero_pcm=1");
    return 0;
}
