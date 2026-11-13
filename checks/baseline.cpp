#include <foonathan/memory/memory_stack.hpp>
#include <sanitizer/asan_interface.h>
#include <cstdio>
#include <cstring>
int main(){foonathan::memory::memory_stack<> s(4096);auto m=s.top();auto*p=static_cast<unsigned char*>(s.allocate(31,16));std::memset(p,0x35,31);for(int i=0;i<31;++i)if(p[i]!=0x35)return 1;s.unwind(m);int poisoned=__asan_address_is_poisoned(p);if(poisoned)return 2;volatile unsigned char*stale=p;stale[0]=0x44;auto*q=static_cast<unsigned char*>(s.allocate(31,16));if(q!=p)return 3;s.unwind(m);std::puts("FOONATHAN stack=readwrite unwind_poison=0 reuse=same");return 0;}
