/* marrow model-space mesh-blend rotation steps - NOT part of the public ABI.
 *
 * ONE source of truth for the hierarchy-rotation recurrence used by the single-pose
 * mrw_pose_mesh_blend and the across-instance batch kernel (scalar reference). Each quaternion is
 * (x,y,z,w), Hamilton, model-space composition q_model = q_parent ⊗ q_local (apply local first).
 *
 * The recurrence per joint j:
 *   QA[j] = root ? normalize(localA[j]) : normalize(QA[parent] ⊗ localA[j])
 *   QB[j] = likewise for input B
 *   QO[j] = hemisphere-corrected nlerp(QA[j], QB[j], we)
 *   localOut[j] = root ? QO[0] : normalize(conj(QO[parent]) ⊗ QO[j])
 * The model-space sweep is INDEPENDENT of the mask; masking enters only through we in the nlerp.
 * That is what makes a mask=0 child keep input A's model-space DIRECTION while its ancestor moves:
 * its restored local rotation compensates for the changed output parent.
 *
 * Only runtime src/ TUs include this (it pulls the scalar quat kit). */
#ifndef MRW_MESH_ROT_H
#define MRW_MESH_ROT_H

#include "marrow.h"
#include "marrow_quat.h"   /* mrw_quat_mul / _conj / _normalize / _nlerp */

/* QA[j]/QB[j]: one model-space unit quat from its parent's model quat + the joint's local quat. */
static inline void mrw_mesh_model_rot_step(const float parent[4], const float local[4],
                                           int is_root, float out[4]) {
    if (is_root) {
        for (int k = 0; k < 4; ++k) out[k] = local[k];
    } else {
        mrw_quat_mul(parent, local, out);
    }
    mrw_quat_normalize(out);
}

/* QO[j]: hemisphere-corrected nlerp of the two model-space rotations at the effective weight. */
static inline void mrw_mesh_blend_rot_step(const float qa[4], const float qb[4], float we,
                                           float out[4]) {
    mrw_quat_nlerp(qa, qb, we, out);
}

/* localOut[j]: restore the local rotation against the OUTPUT parent's model rotation. */
static inline void mrw_mesh_local_rot_step(const float qo_parent[4], const float qo[4],
                                           int is_root, float out[4]) {
    if (is_root) {
        for (int k = 0; k < 4; ++k) out[k] = qo[k];
        mrw_quat_normalize(out);
        return;
    }
    float inv[4];
    mrw_quat_conj(qo_parent, inv);
    mrw_quat_mul(inv, qo, out);
    mrw_quat_normalize(out);
}

#endif /* MRW_MESH_ROT_H */
