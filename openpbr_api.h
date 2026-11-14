/***************************************************************************
 * Copyright 2026 Adobe
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ***************************************************************************/

#ifndef OPENPBR_API_H
#define OPENPBR_API_H

#include "impl/openpbr_bsdf.h"

// Public API for OpenPBR BSDF
// These wrapper functions provide the stable public interface, calling internal functions.
// More advanced workflows can call additional internal functions directly if needed - see README.md and openpbr_bsdf.h for details.

// Main initialization entrypoint: prepares volume, BSDF lobes, and emission from resolved inputs.
OPENPBR_INLINE_FUNCTION OpenPBR_PreparedBsdf openpbr_prepare(OPENPBR_ADDRESS_SPACE_THREAD OPENPBR_CONST_REF(OpenPBR_ResolvedInputs) resolved_inputs,
                                                             const vec3 path_throughput,
                                                             const vec3 rgb_wavelengths_nm,
                                                             const float exterior_ior,
                                                             const vec3 view_direction)
{
    return openpbr_prepare_impl(resolved_inputs, path_throughput, rgb_wavelengths_nm, exterior_ior, view_direction);
}

// Evaluates the BSDF for a given light direction (includes cosine term).
OPENPBR_INLINE_FUNCTION OpenPBR_DiffuseSpecular openpbr_eval(OPENPBR_ADDRESS_SPACE_THREAD OPENPBR_CONST_REF(OpenPBR_PreparedBsdf) prepared,
                                                             const vec3 light_direction)
{
    return openpbr_eval_impl(prepared, light_direction);
}

// Importance-samples a light direction and returns weight, PDF, and sampled lobe type.
OPENPBR_INLINE_FUNCTION void openpbr_sample(OPENPBR_ADDRESS_SPACE_THREAD OPENPBR_CONST_REF(OpenPBR_PreparedBsdf) prepared,
                                            const vec3 rand,
                                            OPENPBR_ADDRESS_SPACE_THREAD OPENPBR_OUT(vec3) light_direction,
                                            OPENPBR_ADDRESS_SPACE_THREAD OPENPBR_OUT(OpenPBR_DiffuseSpecular) weight,
                                            OPENPBR_ADDRESS_SPACE_THREAD OPENPBR_OUT(float) pdf,
                                            OPENPBR_ADDRESS_SPACE_THREAD OPENPBR_OUT(OpenPBR_BsdfLobeType) sampled_type)
{
    openpbr_sample_impl(prepared, rand, light_direction, weight, pdf, sampled_type);
}

// Returns the PDF for sampling the given light direction.
//
// This is the continuous (per-steradian) density only. Discrete (delta) specular events are
// excluded even when the query direction exactly matches an ideal mirror or refraction
// direction; use openpbr_discrete_pdf() for their probability mass. The result is finite.
OPENPBR_INLINE_FUNCTION float openpbr_pdf(OPENPBR_ADDRESS_SPACE_THREAD OPENPBR_CONST_REF(OpenPBR_PreparedBsdf) prepared, const vec3 light_direction)
{
    return openpbr_pdf_impl(prepared, light_direction);
}

// Returns the total discrete probability mass for the given light direction across the whole material.
//
// A direction matches a discrete event when its squared distance to the ideal direction is at most
// 1e-12. Directions that do not match any discrete event, and materials without delta lobes, return
// zero. The result is never infinite.
OPENPBR_INLINE_FUNCTION float openpbr_discrete_pdf(OPENPBR_ADDRESS_SPACE_THREAD OPENPBR_CONST_REF(OpenPBR_PreparedBsdf) prepared,
                                                   const vec3 light_direction)
{
    return openpbr_discrete_pdf_impl(prepared, light_direction);
}

// Evaluates the integrated contribution of the discrete events in the given light direction.
//
// The result is the total discrete energy contribution (in the same DiffuseSpecular format as
// openpbr_eval) rather than a finite-width peak value. openpbr_eval() itself only returns the
// continuous part. Materials without delta lobes, and non-matching directions, return zero.
OPENPBR_INLINE_FUNCTION OpenPBR_DiffuseSpecular openpbr_eval_discrete(OPENPBR_ADDRESS_SPACE_THREAD OPENPBR_CONST_REF(OpenPBR_PreparedBsdf) prepared,
                                                                      const vec3 light_direction)
{
    return openpbr_eval_discrete_impl(prepared, light_direction);
}

#endif  // OPENPBR_API_H
