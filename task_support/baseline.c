#include <oggz/oggz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"baseline failed line=%d\n",__LINE__);exit(1);} }while(0)
static int got;
static int packet(OGGZ *z, oggz_packet *zp, long serial, void *u){
 ogg_packet *p=&zp->op; int i; (void)z;(void)u;
 REQUIRE(serial==17); REQUIRE(p->packetno==got); REQUIRE(p->bytes==(got?70000:7));
 for(i=0;i<p->bytes;i++) REQUIRE(p->packet[i]==(unsigned char)(i+got));
 REQUIRE(!!p->b_o_s==!got);REQUIRE(!!p->e_o_s==!!got);REQUIRE(p->granulepos==(got?48000:0));got++;return 0;
}
static size_t sink(void *u,void *b,size_t n){return fwrite(b,1,n,(FILE*)u);}
int main(void){
 OGGZ *w=oggz_new(OGGZ_WRITE),*r=oggz_new(OGGZ_READ); FILE *f=tmpfile(); unsigned char data[70000],buf[113]; int j,i;long n,total=0;ogg_packet p;
 REQUIRE(w&&r&&f); REQUIRE(oggz_io_set_write(w,sink,f)==0);
 for(j=0;j<2;j++){for(i=0;i<70000;i++)data[i]=(unsigned char)(i+j);memset(&p,0,sizeof p);p.packet=data;p.bytes=j?70000:7;p.b_o_s=!j;p.e_o_s=j;p.packetno=j;p.granulepos=j?48000:0;REQUIRE(oggz_write_feed(w,&p,17,OGGZ_FLUSH_AFTER,NULL)==0);}
 while((n=oggz_write(w,113))>0)total+=n; REQUIRE(n==0);REQUIRE(total>70000);REQUIRE(ftell(f)==total);
 rewind(f);REQUIRE(oggz_set_read_callback(r,-1,packet,NULL)==0);
 while((n=(long)fread(buf,1,sizeof buf,f))>0)REQUIRE(oggz_read_input(r,buf,n)==n);
 REQUIRE(got==2); REQUIRE(oggz_close(w)==0);REQUIRE(oggz_close(r)==0);fclose(f);
 printf("BASELINE full_sink packets=%d bytes=%ld cross_page=1 roundtrip=OK\n",got,total);return 0;
}
