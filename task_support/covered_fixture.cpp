#ifndef VARIANT
#define VARIANT 1
#endif
// Benign, finite, total arithmetic helper. No input mutation or failure path.
extern "C" int covered_value(int x) {
 if (x >= 0 && x <= 100) return x * 2 + VARIANT;
 return VARIANT;
}
