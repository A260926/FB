#include <deepstate/DeepState.h>
#include <fcntl.h>
#include <sys/wait.h>
static int ackfd;
static void normal_case(void) {
 unsigned char a=DeepState_UChar(),b=DeepState_UChar(),c=DeepState_UChar();
 if(a!=0x21 || b!=0x43 || c!=0x65) _exit(9);
 if(write(ackfd,"ok",2)!=2) _exit(10);
 DeepState_Pass();
}
int main(void) {
 unsigned char data[]={0x21,0x43,0x65};int fds[2];
 struct DeepState_TestInfo test={NULL,normal_case,"BenignReplay","task_support/baseline.c",1};
 DeepState_LastTestInfo=&test;DeepState_Setup();FLAGS_fork=1;
 HAS_FLAG_output_test_dir=0;
 int fd=open(".task-build/baseline.input",O_CREAT|O_TRUNC|O_WRONLY,0600);
 if(fd<0 || write(fd,data,3)!=3 || close(fd)!=0 || pipe(fds))return 2;
 ackfd=fds[1];
 int result=DeepState_RunSavedTestCase(&test,".task-build","baseline.input");
 close(fds[1]);char ack[2];ssize_t n=read(fds[0],ack,2);close(fds[0]);
 if(result!=DeepState_TestRunPass || n!=2 || memcmp(ack,"ok",2))return 3;
 const char line[]="BASELINE saved-replay pass input=214365 consumed-in-child=3\n";
 return write(1,line,sizeof(line)-1)==sizeof(line)-1?0:4;
}
