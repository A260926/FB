/* AVX2+FMA instantiation of the model-space mesh-blend kernel (8-wide). Selected only when
 * MRW_FEAT_FMA is set; here mrw_w_fma lowers to _mm256_fmadd_ps. */
#define MRW_ISA_AVX2          1
#define MRW_ISA_FMA           1
#define MRW_MESH_BLEND_KERNEL mrw_mesh_blend_kernel_avx2_fma
#include "marrow_mesh_blend_simd.h"
