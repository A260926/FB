#include "fixture.hpp"
#include <iostream>
#include <thread>
#include <pthread.h>
static void run(){
    auto f=pair_fixture::make(1,1,false,false);
    spirv_cross::CompilerGLSL c(f.words);c.set_common_options(pair_fixture::options());
    c.build_combined_image_samplers();
    std::cerr<<"size="<<c.compile().size()<<"\n";
}
int main(){
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64*1024*1024);
    pthread_create(&t,&a,[](void*)->void*{run();return nullptr;},nullptr);
    pthread_join(t,nullptr);
    std::cerr<<"joined ok\n";
    return 0;
}
