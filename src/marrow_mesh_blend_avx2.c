/* AVX2 (no-FMA) instantiation of the model-space mesh-blend kernel (8-wide). Selected when the
 * host has AVX2 but not FMA; built /fp:precise (-ffp-contract=off) so it emits no FMA. */
#define MRW_ISA_AVX2          1
#define MRW_MESH_BLEND_KERNEL mrw_mesh_blend_kernel_avx2
#include "marrow_mesh_blend_simd.h"
