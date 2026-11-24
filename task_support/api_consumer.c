#include <deepstate/Replay.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
static void consume_one(void){if(DeepState_UChar()!=0x21)_exit(7);DeepState_Pass();}
int main(int argc,char **argv){
 if(argc!=2)return 2;
 unsigned char data[]={0x21,0x43};
 struct DeepState_TestInfo test={NULL,consume_one,"ConsumeOne","api_consumer.c",1};
 struct DeepState_ReplayCase item={17,&test,data,sizeof(data)};
 struct DeepState_ReplayConfig cfg={71,1,1024,-1,argv[1]};
 struct DeepState_ReplayResult result;
 memset(&result,0x5a,sizeof(result));
 if(DeepState_ReplayBatch(&item,1,&cfg,&result)!=0)return 3;
 if(result.run_id!=71 || result.id!=17 || result.state!=DeepState_ReplayComplete || result.result!=DeepState_TestRunPass || result.consumed!=1)return 4;
 char path[4096];if(snprintf(path,sizeof(path),"%s/17.pass",argv[1])>=sizeof(path))return 5;
 int fd=open(path,O_RDONLY);unsigned char bytes[3];ssize_t n=fd<0?-1:read(fd,bytes,sizeof(bytes));if(fd>=0)close(fd);
 if(n!=1 || bytes[0]!=0x21)return 6;
 const char msg[]="API_CONSUMER replay normal=pass consumed=1\n";
 return write(1,msg,sizeof(msg)-1)==sizeof(msg)-1?0:7;
}
