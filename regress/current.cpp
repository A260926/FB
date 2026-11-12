#include <cassert>
#include <cstdio>
#define main upstream_long_test_main
#include "../test/Test.cpp"
#undef main
int main() {
 assert(TestConvolver(171,37,1,17,8,true));
 assert(TestConvolver(73,97,3,11,5,true));
 assert(TestTwoStageConvolver(171,113,1,17,4,16,true));
 assert(TestTwoStageConvolver(45,123,3,13,4,32,true));
 puts("CONVOLUTION_BASELINE_OK selected=4");
}
