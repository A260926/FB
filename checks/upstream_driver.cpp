// Deterministic inputs for the unchanged upstream fuzzer callback.
#include "ebur128.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t);
template<class T> static void put(std::vector<uint8_t>& v, const T& x) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&x);
  v.insert(v.end(), p, p + sizeof(x));
}
int main() {
  for (unsigned seed=1; seed<=8; ++seed) {
    std::vector<uint8_t> v;
    put(v, unsigned(2)); put(v, (unsigned long)48000);
    put(v, int(EBUR128_MODE_I|EBUR128_MODE_LRA|EBUR128_MODE_TRUE_PEAK));
    put(v, unsigned(6)); put(v, (unsigned long)44100);
    put(v, (unsigned long)4000); put(v, seed);
    for (unsigned i=0;i<480000;i++) put(v, int((i%97)*123456-6000000));
    if (LLVMFuzzerTestOneInput(v.data(),v.size())) return 1;
    std::printf("UPSTREAM_CALLBACK seed=%u PASS\n",seed);
  }
  std::puts("UPSTREAM_FUZZ_DRIVER_OK calls=8");
}
