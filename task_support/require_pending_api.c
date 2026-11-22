#include <oggz/oggz.h>
#include <stdio.h>
#include <string.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"required pending API probe failed: %s\n",#x);return 1;}}while(0)
int main(void){
 OGGZ *z=oggz_new(OGGZ_WRITE);ogg_packet op;unsigned char bytes[4]={1,2,3,4},out[17];long n,total=0;
 REQUIRE(z!=NULL);REQUIRE(oggz_write_get_pending_bytes(z)==0);
 REQUIRE(oggz_write_set_pending_limit(z,3)==0);
 memset(&op,0,sizeof op);op.packet=bytes;op.bytes=4;op.b_o_s=1;op.e_o_s=1;op.packetno=0;op.granulepos=0;
 REQUIRE(oggz_write_feed(z,&op,17,OGGZ_FLUSH_AFTER,NULL)==OGGZ_ERR_IO_AGAIN);
 REQUIRE(oggz_write_get_pending_bytes(z)==0);
 REQUIRE(oggz_write_set_pending_limit(z,4)==0);
 REQUIRE(oggz_write_feed(z,&op,17,OGGZ_FLUSH_AFTER,NULL)==0);
 REQUIRE(oggz_write_get_pending_bytes(z)==4);
 while((n=oggz_write_output(z,out,sizeof out))>0){total+=n;REQUIRE(oggz_write_get_pending_bytes(z)>=0);}
 REQUIRE(n==0&&total>4);REQUIRE(oggz_write_get_pending_bytes(z)==0);REQUIRE(oggz_close(z)==0);
 puts("REQUIRED_PENDING_API_CALL_OK");return 0;
}
