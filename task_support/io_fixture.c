#include "caption/caption.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc,char**argv){
 if(argc!=3)return 2;
 if(!strcmp(argv[1],"make")){
  FILE*f=fopen(argv[2],"wb");if(!f)return 3;fputs("SET first 10 AB\nPULL 14\n",f);return fclose(f)?4:0;
 }
 if(strcmp(argv[1],"check"))return 2;
 FILE*f=fopen(argv[2],"rb");if(!f)return 5;caption_frame_t frame;caption_frame_init(&frame);
 for(unsigned long long n=0;n<14;n++){
  unsigned long long at;unsigned word;
  if(fscanf(f,"%llu %x",&at,&word)!=2||at!=n||word>65535||!eia608_parity_varify(word)){fclose(f);return 6;}
  if(caption_frame_decode(&frame,word,n/30.0)==LIBCAPTION_ERROR){fclose(f);return 7;}
  const char*a=caption_frame_read_char(&frame,14,0,0,0);const char*b=caption_frame_read_char(&frame,14,1,0,0);
  if(n<10?(*a||*b):(strcmp(a,"A")||strcmp(b,"B"))){fclose(f);return 8;}
 }
 int ch;do{ch=fgetc(f);}while(ch==' '||ch=='\n'||ch=='\r'||ch=='\t');
 fclose(f);if(ch!=EOF)return 9;
 puts("CAPTION_EXAMPLE_OK frames=14 display_at=10 text=AB");return 0;
}
