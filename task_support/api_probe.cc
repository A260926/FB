#define REQUIRE_ROWS
#include "codec_witness.h"
int main(){int r=codec_witness(".task-build/api-normal");if(r)std::cerr<<"WITNESS_FAILURE code="<<r<<"\n";return r;}
