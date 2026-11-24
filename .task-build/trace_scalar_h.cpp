#include "fixture.hpp"
#include <iostream>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
static void handler(int sig){void*b[32];fprintf(stderr,"SIG %d\n",sig);backtrace_symbols_fd(b,backtrace(b,32),2);_Exit(99);}
int main(int argc,char**argv){
 signal(SIGBUS,handler);signal(SIGSEGV,handler);
 int mode=argc>1?atoi(argv[1]):0;
 auto f=pair_fixture::make(mode?3:1,1,mode!=0,false);
 spirv_cross::CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
 c.build_combined_image_samplers();
 std::cerr<<"compiling\n";
 auto g=c.compile(); std::cerr<<"OK\n"; std::cout<<g.size()<<"\n";
 return 0;
}
