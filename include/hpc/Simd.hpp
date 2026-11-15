#pragma once
// SIMDe selects native AVX when enabled and NEON/portable implementations
// otherwise.
#include <simde/x86/avx2.h>

namespace SoftRasterizer {
using Float8 = simde__m256;
}
