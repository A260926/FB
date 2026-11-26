#include "ik/node_base.h"

#define IK_NODE_FABRIK_HEAD                                                   \
    IK_NODE_HEAD                                                              \
    union                                                                     \
    {                                                                         \
        struct                                                                \
        {                                                                     \
            ik_quat_t initial_rotation;                                       \
            ik_vec3_t initial_position;                                       \
        };                                                                    \
        ikreal_t initial_transform[7];                                        \
    };                                                                        \
    /* Reference pose captured at rebuild (local space). */                   \
    ik_quat_t bind_local_rotation;                                            \
    ik_vec3_t bind_local_position;                                            \
    /* Accumulated reference world orientation (bind frame). */               \
    ik_quat_t bind_rotation;                                                  \
    /* Accumulated solved world orientation (solved frame). */                \
    ik_quat_t frame_rotation;                                                 \
    /* Local rotation derived from the solved frames. */                      \
    ik_quat_t solved_local_rotation;

struct ik_node_FABRIK_t
{
    IK_NODE_FABRIK_HEAD
};

IK_IMPLEMENT(node_FABRIK, node_base)
{
    IK_OVERRIDE(create)
    IK_CONSTRUCTOR(construct)
}
