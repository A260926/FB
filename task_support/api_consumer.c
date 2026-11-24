#include <ogg/theora_edit.h>
#include "fixture_io.h"
static int verify(const char*path){struct fixture a,b;int i,rc=1;if(fixture_read("tests/fixtures/frames.ogv",&a))return 1;if(fixture_read(path,&b)){fixture_free(&a);return 1;}if(b.count!=5)goto done;
 for(i=0;i<5;i++)if(a.bytes[i]!=b.bytes[i]||memcmp(a.data[i],b.data[i],a.bytes[i]))goto done;
 {unsigned shift=((a.data[0][40]&3)<<3)|(a.data[0][41]>>5);if(b.gp[4]!=(((ogg_int64_t)1<<shift)+1))goto done;}
 rc=0;printf("consumer frames=2 payload=preserved granule=rebased\n");
 done:fixture_free(&a);fixture_free(&b);return rc;}
int main(int argc,char**argv){if(argc==3&&!strcmp(argv[1],"--verify"))return verify(argv[2]);
 FILE*in=fopen("tests/fixtures/frames.ogv","rb");FILE*out=fopen(".task-build/consumer.ogv","wb");ogg_theora_plan*p=NULL;ogg_theora_cut cut={0,0,2};int rc=1;
 if(!in||!out)goto done;if(ogg_theora_plan_create(in,&cut,1,&p)||!p)goto done;if(ogg_theora_plan_write(p,out,901))goto done;if(fflush(out))goto done;rc=0;
 done:ogg_theora_plan_destroy(p);if(in)fclose(in);if(out&&fclose(out))rc=1;return rc?rc:verify(".task-build/consumer.ogv");}
