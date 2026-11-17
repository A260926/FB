#include <oggz/oggz.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
/* Existing non-public placeholder; no implementation is supplied here. */
extern long oggz_seek_packets(OGGZ *, long, long, int);
static int count, bad;
static int receive(OGGZ *z, oggz_packet *p, long serial, void *u) {
  (void)z; (void)u;
  const unsigned char values[]={'A','B','C','D'};
  const long lengths[]={3,2,70000,257};
  if(count>=4) {bad=1; return OGGZ_STOP_ERR;}
  if(serial!=(count%2?9:7) || p->op.bytes!=lengths[count] || p->op.packetno!=count/2) bad=1;
  for(long j=0;j<p->op.bytes;j++) if(p->op.packet[j]!=values[count]) bad=1;
  count++;
  return OGGZ_CONTINUE;
}
static int packet(FILE *f, ogg_stream_state *s, int no, long size, int value) {
  unsigned char *buf=malloc((size_t)size);
  if(!buf)return 1;
  memset(buf,value,(size_t)size);
  ogg_packet p={0}; p.packet=buf; p.bytes=size; p.packetno=no;
  p.b_o_s=no==0; p.e_o_s=no==1; p.granulepos=no*480;
  if(ogg_stream_packetin(s,&p)){free(buf);return 1;}
  ogg_page page;
  while(ogg_stream_flush(s,&page)) {
    if(fwrite(page.header,1,(size_t)page.header_len,f)!=(size_t)page.header_len ||
       fwrite(page.body,1,(size_t)page.body_len,f)!=(size_t)page.body_len) {free(buf);return 1;}
  }
  free(buf);return 0;
}
int main(int argc,char **argv) {
  FILE *f=tmpfile(); if(!f)return 1;
  ogg_stream_state a,b;
  if(ogg_stream_init(&a,7)||ogg_stream_init(&b,9))return 2;
  if(packet(f,&a,0,3,'A')||packet(f,&b,0,2,'B')||packet(f,&a,1,70000,'C')||packet(f,&b,1,257,'D'))return 3;
  ogg_stream_clear(&a);ogg_stream_clear(&b);
  fflush(f);rewind(f);
  OGGZ *z=oggz_open_stdio(f,OGGZ_READ);
  if(!z||oggz_set_read_callback(z,-1,receive,NULL))return 4;
  long n; while((n=oggz_read(z,97))>0){}
  if(n<0||bad||count!=4)return 5;
  printf("BASELINE_READ packets=%d streams=2 continued_bytes=70000 payloads=exact\n",count);
  if(argc>1&&!strcmp(argv[1],"--observe-gap"))
    printf("PACKET_SEEK_START result=%ld\n",oggz_seek_packets(z,7,0,SEEK_SET));
  if(oggz_close(z))return 6;
  puts("BASELINE_COMPLETE");return 0;
}
