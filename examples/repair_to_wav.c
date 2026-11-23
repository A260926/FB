/********************************************************************
 *                                                                  *
 * repair_to_wav.c: stream Ogg Opus from stdin over a non-seekable  *
 * pipe, enable libopusfile's single-packet in-band FEC repair,     *
 * disable dithering, and write a 16-bit stereo 48 kHz WAV file to  *
 * the path given on the command line.                             *
 *                                                                  *
 * The WAV chunk sizes are placeholders while the data is written   *
 * and are backfilled from the number of PCM frames actually        *
 * produced, so a repaired stream ends up with the same length as   *
 * the undamaged original.                                          *
 *                                                                  *
 ********************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opusfile.h>

/*Read at most _nbytes from stdin; no seek/tell/close callbacks are provided,
   so libopusfile treats the source as a non-seekable pipe.*/
static int stdin_read(void *_stream,unsigned char *_ptr,int _nbytes){
  FILE *fp;
  size_t ret;
  (void)_stream;
  if(_nbytes<=0)return 0;
  fp=stdin;
  ret=fread(_ptr,1,(size_t)_nbytes,fp);
  if(ret>0||feof(fp))return (int)ret;
  return OP_EREAD;
}

static void put_le32(unsigned char *_dst,opus_uint32 _x){
  _dst[0]=(unsigned char)(_x&0xFF);
  _dst[1]=(unsigned char)(_x>>8&0xFF);
  _dst[2]=(unsigned char)(_x>>16&0xFF);
  _dst[3]=(unsigned char)(_x>>24&0xFF);
}

/*Build the standard 44-byte PCM WAV header for 48 kHz stereo 16-bit audio,
   with both size fields left at zero until the real sizes are known.*/
static void make_wav_header(unsigned char _dst[44]){
  static const unsigned char template_[44]={
    'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
    16,0,0,0, /*fmt chunk size*/
    1,0,      /*PCM format*/
    2,0,      /*two channels*/
    0,0,0,0,  /*sample rate, filled below*/
    0,0,0,0,  /*byte rate, filled below*/
    4,0,      /*block align: 2 channels * 2 bytes*/
    16,0,     /*bits per sample*/
    'd','a','t','a',0,0,0,0
  };
  memcpy(_dst,template_,sizeof(template_));
  put_le32(_dst+24,48000);
  put_le32(_dst+28,48000*2*2);
}

int main(int _argc,const char **_argv){
  OpusFileCallbacks cb;
  OggOpusFile       *of;
  FILE              *out;
  unsigned char      wav_header[44];
  opus_int16         pcm[120*48*2];
  ogg_int64_t        frames;
  int                err;
  if(_argc!=2){
    fprintf(stderr,"Usage: %s <output.wav> < input.opus\n",_argv[0]);
    return EXIT_FAILURE;
  }
  out=fopen(_argv[1],"wb");
  if(out==NULL){
    fprintf(stderr,"Could not open '%s' for writing.\n",_argv[1]);
    return EXIT_FAILURE;
  }
  memset(&cb,0,sizeof(cb));
  cb.read=stdin_read;
  of=op_open_callbacks(NULL,&cb,NULL,0,&err);
  if(of==NULL){
    fprintf(stderr,"Could not open Ogg Opus stream: %d\n",err);
    fclose(out);
    return EXIT_FAILURE;
  }
  if(op_seekable(of)){
    fprintf(stderr,"Expected a non-seekable input stream.\n");
    op_free(of);
    fclose(out);
    return EXIT_FAILURE;
  }
  err=op_set_inband_fec(of,1);
  if(err!=0){
    fprintf(stderr,"Could not enable in-band FEC repair: %d\n",err);
    op_free(of);
    fclose(out);
    return EXIT_FAILURE;
  }
  /*No noise-shaped dithering: emit plain rounded 16-bit samples.*/
  op_set_dither_enabled(of,0);
  make_wav_header(wav_header);
  if(fwrite(wav_header,sizeof(wav_header),1,out)!=1){
    fprintf(stderr,"Error writing WAV header.\n");
    op_free(of);
    fclose(out);
    return EXIT_FAILURE;
  }
  frames=0;
  for(;;){
    int ret;
    ret=op_read_stereo(of,pcm,(int)(sizeof(pcm)/sizeof(*pcm)));
    if(ret<0){
      /*This includes any unrepaired OP_HOLE: report failure rather than
         silently shortening the output.*/
      fprintf(stderr,"Error reading Opus stream: %d\n",ret);
      op_free(of);
      fclose(out);
      return EXIT_FAILURE;
    }
    if(ret==0)break;
    if(fwrite(pcm,sizeof(*pcm)*2,(size_t)ret,out)!=(size_t)ret){
      fprintf(stderr,"Error writing WAV PCM data.\n");
      op_free(of);
      fclose(out);
      return EXIT_FAILURE;
    }
    frames+=ret;
  }
  op_free(of);
  /*Backfill the chunk sizes from the PCM actually delivered.
    A 16-bit stereo WAV data chunk cannot describe more than 0xFFFFFFFC bytes.*/
  if(frames>0x3FFFFFFD){
    fprintf(stderr,"Output too large for a single WAV data chunk.\n");
    fclose(out);
    return EXIT_FAILURE;
  }
  put_le32(wav_header+40,(opus_uint32)(frames*2*2));
  put_le32(wav_header+4,(opus_uint32)(36+frames*2*2));
  if(fseek(out,0,SEEK_SET)!=0||
   fwrite(wav_header,sizeof(wav_header),1,out)!=1||fclose(out)!=0){
    fprintf(stderr,"Error backfilling WAV header.\n");
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
