/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggTheora SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE Theora SOURCE CODE IS COPYRIGHT (C) 2002-2009,2025           *
 * by the Xiph.Org Foundation and contributors                      *
 * https://www.xiph.org/                                            *
 *                                                                  *
 ********************************************************************

  function: region decoding example
  last mod: $Id$

 ********************************************************************/

/*Encodes three frames with the Theora encoder, then decodes a region of
   the last (non-first) frame with the incremental region decoder and
   writes the three planes of the region, in order, to the file named by
   the first command line argument.
  Usage: region_preview <output.yuv>
  Returns 0 on success, non-zero on failure.*/

#include <theora/theoraenc.h>
#include <theora/theoraregion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 128
#define HEIGHT 128
#define NFRAMES 3
#define REGION_X 32
#define REGION_Y 32
#define REGION_W 64
#define REGION_H 64

static void make_frame(int f,unsigned char *y,unsigned char *u,
 unsigned char *v){
  int i;
  int j;
  for(j=0;j<HEIGHT;j++)for(i=0;i<WIDTH;i++){
    int sx;
    int sy;
    sx=i-3*f<0?0:i-3*f>=WIDTH?WIDTH-1:i-3*f;
    sy=j-2*f<0?0:j-2*f>=HEIGHT?HEIGHT-1:j-2*f;
    y[j*WIDTH+i]=(unsigned char)(sx*2+sy*3+((sx*sy)&15)+((sx^sy)&32?40:0));
  }
  for(j=20+f*4;j<44+f*4;j++)for(i=24+f*4;i<56+f*4;i++){
    if(j>=0&&j<HEIGHT&&i>=0&&i<WIDTH)y[j*WIDTH+i]=(unsigned char)(180+(i&31));
  }
  for(j=0;j<HEIGHT/2;j++)for(i=0;i<WIDTH/2;i++){
    u[j*(WIDTH/2)+i]=(unsigned char)(100+((i*5+j*3+f*4)&31));
    v[j*(WIDTH/2)+i]=(unsigned char)(140-((i*3+j*7+f*2)&31));
  }
}

int main(int argc,char **argv){
  th_info        info;
  th_info        parsed;
  th_comment     comment;
  th_setup_info *setup;
  th_enc_ctx    *enc;
  th_region_ctx *region;
  th_ycbcr_buffer img;
  th_ycbcr_buffer out;
  ogg_packet     op;
  unsigned char *y;
  unsigned char *u;
  unsigned char *v;
  FILE          *fp;
  int            f;
  int            p;
  int            ret;
  if(argc!=2){
    fprintf(stderr,"usage: %s <output.yuv>\n",argv[0]);
    return 1;
  }
  setup=NULL;
  enc=NULL;
  region=NULL;
  fp=NULL;
  ret=1;
  th_info_init(&info);
  th_info_init(&parsed);
  th_comment_init(&comment);
  info.frame_width=info.pic_width=WIDTH;
  info.frame_height=info.pic_height=HEIGHT;
  info.fps_numerator=25;
  info.fps_denominator=1;
  info.aspect_numerator=info.aspect_denominator=1;
  info.pixel_fmt=TH_PF_420;
  info.quality=42;
  info.keyframe_granule_shift=6;
  enc=th_encode_alloc(&info);
  if(enc==NULL)goto end;
  while(th_encode_flushheader(enc,&comment,&op)>0){
    if(th_decode_headerin(&parsed,&comment,&setup,&op)<=0)goto end;
  }
  region=th_region_alloc(&parsed,setup,NFRAMES);
  if(region==NULL)goto end;
  y=(unsigned char *)malloc(WIDTH*HEIGHT);
  u=(unsigned char *)malloc(WIDTH*HEIGHT/4);
  v=(unsigned char *)malloc(WIDTH*HEIGHT/4);
  if(y==NULL||u==NULL||v==NULL){
    free(y);
    free(u);
    free(v);
    goto end;
  }
  img[0]=(th_img_plane){WIDTH,HEIGHT,WIDTH,y};
  img[1]=(th_img_plane){WIDTH/2,HEIGHT/2,WIDTH/2,u};
  img[2]=(th_img_plane){WIDTH/2,HEIGHT/2,WIDTH/2,v};
  for(f=0;f<NFRAMES;f++){
    make_frame(f,y,u,v);
    if(th_encode_ycbcr_in(enc,img)!=0){
      free(y);
      free(u);
      free(v);
      goto end;
    }
    while(th_encode_packetout(enc,f==NFRAMES-1,&op)>0){
      if(th_region_packetin(region,&op)!=0){
        free(y);
        free(u);
        free(v);
        goto end;
      }
    }
  }
  free(y);
  free(u);
  free(v);
  /*Request a region of a non-first frame and write the three planes, in
     order, to the output file.*/
  if(th_region_out(region,NFRAMES-1,REGION_X,REGION_Y,REGION_W,REGION_H,
   out)!=0)goto end;
  fp=fopen(argv[1],"wb");
  if(fp==NULL)goto end;
  for(p=0;p<3;p++){
    int row;
    for(row=0;row<out[p].height;row++){
      if(fwrite(out[p].data+(ptrdiff_t)row*out[p].stride,1,
       (size_t)out[p].width,fp)!=(size_t)out[p].width){
        fclose(fp);
        fp=NULL;
        goto end;
      }
    }
  }
  if(fclose(fp)!=0){
    fp=NULL;
    goto end;
  }
  fp=NULL;
  ret=0;
 end:
  if(fp!=NULL)fclose(fp);
  th_region_free(region);
  th_encode_free(enc);
  th_setup_free(setup);
  th_info_clear(&info);
  th_info_clear(&parsed);
  th_comment_clear(&comment);
  return ret;
}
