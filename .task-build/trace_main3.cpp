#include "fixture.hpp"
#include <iostream>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
static void handler(int sig) {
    void *bt[32]; int n=backtrace(bt,32);
    fprintf(stderr,"SIGNAL %d\n",sig); backtrace_symbols_fd(bt,n,2); _Exit(99);
}
int main(){
    signal(SIGBUS,handler); signal(SIGSEGV,handler);
    auto f = pair_fixture::make(2,3,true,true,true);
    spirv_cross::CompilerGLSL c(f.words);
    c.set_common_options(pair_fixture::options());
    c.build_combined_image_samplers();
    std::cerr<<"built, compiling\n";
    std::string g=c.compile();
    std::cerr<<"OK\n"; std::cout<<g;
    return 0;
}
