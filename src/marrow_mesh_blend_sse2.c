/* SSE2 instantiation of the model-space mesh-blend kernel (4-wide; two subgroups per 8-lane tile).
 * Own translation unit so the baseline can never emit AVX. */
#define MRW_ISA_SSE2           1
#define MRW_MESH_BLEND_KERNEL  mrw_mesh_blend_kernel_sse2
#include "marrow_mesh_blend_simd.h"
