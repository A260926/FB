/*
 * Cone constraint regression cases.
 *
 * All geometry is reconstructed from the public node tree (local position
 * and rotation after solve); the internal chain/cache structures are never
 * read as ground truth.
 */
#include "ik/ik.h"
#include "task_support/check.h"
#include <math.h>
#include <stddef.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------------- */
/* Small math helpers operating on public types */
/* ------------------------------------------------------------------------- */

static int
finite_vec3(const ik_vec3_t* v)
{
    return isfinite(v->x) && isfinite(v->y) && isfinite(v->z);
}

static int
finite_quat(const ik_quat_t* q)
{
    return isfinite(q->x) && isfinite(q->y) &&
           isfinite(q->z) && isfinite(q->w);
}

static ik_vec3_t
vec3_sub(ik_vec3_t a, const ik_vec3_t* b)
{
    a.x -= b->x;
    a.y -= b->y;
    a.z -= b->z;
    return a;
}

static double
vec3_len(const ik_vec3_t* v)
{
    return sqrt(v->x * v->x + v->y * v->y + v->z * v->z);
}

static double
vec3_dot(const ik_vec3_t* a, const ik_vec3_t* b)
{
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

static double
angle_between(const ik_vec3_t* a, const ik_vec3_t* b)
{
    double la = vec3_len(a), lb = vec3_len(b);
    double cos_a;
    if (la == 0.0 || lb == 0.0)
        return M_PI;
    cos_a = vec3_dot(a, b) / (la * lb);
    if (cos_a > 1.0) cos_a = 1.0;
    if (cos_a < -1.0) cos_a = -1.0;
    return acos(cos_a);
}

static ik_quat_t
quat_axis_angle(double x, double y, double z, double angle)
{
    ik_quat_t q;
    double half = angle * 0.5;
    double s = sin(half);
    q.x = x * s;
    q.y = y * s;
    q.z = z * s;
    q.w = cos(half);
    return q;
}

static ik_vec3_t
rotate_vec3(const ik_vec3_t* v, const ik_quat_t* q)
{
    /* v' = q v q* (standard expansion) */
    ik_vec3_t u = {{q->x, q->y, q->z}};
    double dot = vec3_dot(&u, v);
    ik_vec3_t cross = {{
        u.y * v->z - u.z * v->y,
        u.z * v->x - u.x * v->z,
        u.x * v->y - u.y * v->x
    }};
    ik_vec3_t r;
    double s = q->w;
    r.x = 2.0 * dot * u.x + (s * s - vec3_dot(&u, &u)) * v->x + 2.0 * s * cross.x;
    r.y = 2.0 * dot * u.y + (s * s - vec3_dot(&u, &u)) * v->y + 2.0 * s * cross.y;
    r.z = 2.0 * dot * u.z + (s * s - vec3_dot(&u, &u)) * v->z + 2.0 * s * cross.z;
    return r;
}

/*
 * Forward kinematics over the public node tree. After a solve the nodes hold
 * local transforms; this rebuilds the corresponding world pose.
 */
struct fk_pose
{
    ik_quat_t rotation;
    ik_vec3_t position;
};

static void
fk_recursive(const struct ik_node_t* node, const struct fk_pose* parent,
             struct fk_pose* out)
{
    if (parent == NULL)
    {
        out->rotation = node->rotation;
        out->position = node->position;
    }
    else
    {
        ik_vec3_t offset = rotate_vec3(&node->position, &parent->rotation);
        out->position.x = parent->position.x + offset.x;
        out->position.y = parent->position.y + offset.y;
        out->position.z = parent->position.z + offset.z;
        out->rotation = parent->rotation;
        {
            /* quaternion multiply: parent * local */
            const ik_quat_t* a = &parent->rotation;
            const ik_quat_t* b = &node->rotation;
            ik_quat_t r;
            r.w = a->w * b->w - a->x * b->x - a->y * b->y - a->z * b->z;
            r.x = a->w * b->x + a->x * b->w + a->y * b->z - a->z * b->y;
            r.y = a->w * b->y - a->x * b->z + a->y * b->w + a->z * b->x;
            r.z = a->w * b->z + a->x * b->y - a->y * b->x + a->z * b->w;
            out->rotation = r;
        }
    }

    NODE_FOR_EACH(node, guid, child)
        struct fk_pose child_pose;
        fk_recursive(child, out, &child_pose);
    NODE_END_EACH
}

/* Collects world-space node data by guid for assertions. */
#define MAX_NODES 32
struct world_node
{
    uint32_t guid;
    ik_vec3_t position;
    ik_quat_t rotation;
};

static void
collect_recursive(const struct ik_node_t* node, const struct fk_pose* parent,
                  struct world_node* out, int* count)
{
    struct fk_pose pose;
    fk_recursive(node, parent, &pose);
    out[*count].guid = node->guid;
    out[*count].position = pose.position;
    out[*count].rotation = pose.rotation;
    ++(*count);

    NODE_FOR_EACH(node, guid, child)
        collect_recursive(child, &pose, out, count);
    NODE_END_EACH
}

static int
collect_world(const struct ik_node_t* root, struct world_node* out)
{
    int count = 0;
    collect_recursive(root, NULL, out, &count);
    return count;
}

static const struct world_node*
find_world(const struct world_node* nodes, int count, uint32_t guid)
{
    int i;
    for (i = 0; i < count; ++i)
        if (nodes[i].guid == guid)
            return &nodes[i];
    return NULL;
}

/* World direction of the bone ending at the given node. */
static ik_vec3_t
bone_direction(const struct world_node* nodes, int count,
               uint32_t child_guid, uint32_t parent_guid)
{
    const struct world_node* c = find_world(nodes, count, child_guid);
    const struct world_node* p = find_world(nodes, count, parent_guid);
    return vec3_sub(c->position, &p->position);
}

static double
bone_length(const struct world_node* nodes, int count,
            uint32_t child_guid, uint32_t parent_guid)
{
    ik_vec3_t d = bone_direction(nodes, count, child_guid, parent_guid);
    return vec3_len(&d);
}

/*
 * Reference cone axis for a bone whose bind pose runs along local +X.
 * The top bone follows the root's animated rotation; any deeper bone's
 * reference is the solved direction of the bone above its parent. Both
 * facts follow from the frame convention used by the solver and only need
 * public world positions here.
 */
static ik_vec3_t
top_reference_axis(const struct ik_node_t* root)
{
    ik_vec3_t x = {{1.0, 0.0, 0.0}};
    return rotate_vec3(&x, &root->rotation);
}

/* ------------------------------------------------------------------------- */
/* Skeleton builders */
/* ------------------------------------------------------------------------- */

static struct ik_node_t*
build_chain(struct ik_solver_t* s, int bones)
{
    int i;
    struct ik_node_t* root = s->node->create(0);
    struct ik_node_t* node = root;
    root->rotation.w = 1.0;
    for (i = 0; i < bones; ++i)
    {
        struct ik_node_t* child = s->node->create((uint32_t)(i + 1));
        child->rotation.w = 1.0;
        child->position.x = 1.0;
        s->node->add_child(node, child);
        node = child;
    }
    return root;
}

static struct ik_constraint_t*
attach_cone(struct ik_solver_t* s, struct ik_node_t* node,
            double axis_x, double axis_y, double axis_z, double angle)
{
    struct ik_constraint_t* c = s->constraint->create(IK_CONE);
    c->cone_axis.x = axis_x;
    c->cone_axis.y = axis_y;
    c->cone_axis.z = axis_z;
    c->cone_angle = angle;
    s->constraint->attach(c, node);
    return c;
}

static struct ik_effector_t*
attach_effector(struct ik_solver_t* s, struct ik_node_t* node,
                double x, double y, double z)
{
    struct ik_effector_t* e = s->effector->create();
    e->target_position.x = x;
    e->target_position.y = y;
    e->target_position.z = z;
    s->effector->attach(e, node);
    return e;
}

static void
configure_solver(struct ik_solver_t* s)
{
    s->max_iterations = 50;
    s->tolerance = 1e-5;
}

/*
 * A completed solve may report IK_RESULT_CONVERGED when the residual is
 * above the tolerance (clamped chains can oscillate slightly). Reachability
 * is asserted from the resulting pose instead of the return code.
 */
static int
solve_completed(ikret_t rc)
{
    return rc == IK_OK || rc == IK_RESULT_CONVERGED;
}

/* Assert every output transform is finite. */
static void
check_finite_tree(const struct ik_node_t* node, int* ok)
{
    if (!finite_vec3(&node->position) || !finite_quat(&node->rotation))
        *ok = 0;
    NODE_FOR_EACH(node, guid, child)
        check_finite_tree(child, ok);
    NODE_END_EACH
}

/* ========================================================================= */
/* cone group */
/* ========================================================================= */

static int
case_cone_reachable(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(s, 2);
    struct world_node world[MAX_NODES];
    int count;
    ik_vec3_t d0, d1, ref;

    attach_effector(s, root->v->find_child(root, 2), 1.9, 0.4, 0.0);
    attach_cone(s, root->v->find_child(root, 1), 1, 0, 0, 0.5);
    attach_cone(s, root->v->find_child(root, 2), 1, 0, 0, 0.5);
    s->flags = IK_ENABLE_CONSTRAINTS;
    configure_solver(s);
    ik.solver.set_tree(s, root);
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    TASK_CHECK(solve_completed(ik.solver.solve(s)));

    count = collect_world(root, world);
    TASK_CHECK(count == 3);
    {
        const struct world_node* tip = find_world(world, count, 2);
        TASK_CHECK(fabs(tip->position.x - 1.9) < 1e-3);
        TASK_CHECK(fabs(tip->position.y - 0.4) < 1e-3);
        TASK_CHECK(fabs(tip->position.z) < 1e-3);
    }

    ref = top_reference_axis(root);
    d0 = bone_direction(world, count, 1, 0);
    d1 = bone_direction(world, count, 2, 1);
    TASK_CHECK(angle_between(&d0, &ref) <= 0.5 + 1e-4);
    TASK_CHECK(angle_between(&d1, &d0) <= 0.5 + 1e-4);
    TASK_CHECK(fabs(bone_length(world, count, 1, 0) - 1.0) < 1e-6);
    TASK_CHECK(fabs(bone_length(world, count, 2, 1) - 1.0) < 1e-6);

    ik.solver.destroy(s);
    return 0;
}

static int
case_cone_blocks_sideways(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(s, 2);
    struct world_node world[MAX_NODES];
    int count, finite_ok = 1;
    ik_vec3_t d0, d1, ref;

    attach_effector(s, root->v->find_child(root, 2), 0.0, 2.0, 0.0);
    attach_cone(s, root->v->find_child(root, 1), 1, 0, 0, 0.3);
    attach_cone(s, root->v->find_child(root, 2), 1, 0, 0, 0.3);
    s->flags = IK_ENABLE_CONSTRAINTS;
    configure_solver(s);
    ik.solver.set_tree(s, root);
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    ik.solver.solve(s);

    count = collect_world(root, world);
    check_finite_tree(root, &finite_ok);
    TASK_CHECK(finite_ok);

    ref = top_reference_axis(root);
    d0 = bone_direction(world, count, 1, 0);
    d1 = bone_direction(world, count, 2, 1);
    TASK_CHECK(angle_between(&d0, &ref) <= 0.3 + 1e-4);
    TASK_CHECK(angle_between(&d1, &d0) <= 0.3 + 1e-4);
    TASK_CHECK(fabs(bone_length(world, count, 1, 0) - 1.0) < 1e-6);
    TASK_CHECK(fabs(bone_length(world, count, 2, 1) - 1.0) < 1e-6);

    /* Unreachable target: a distance error must remain. */
    {
        const struct world_node* tip = find_world(world, count, 2);
        ik_vec3_t target = {{0.0, 2.0, 0.0}};
        ik_vec3_t miss = vec3_sub(tip->position, &target);
        TASK_CHECK(vec3_len(&miss) > 0.5);
    }

    ik.solver.destroy(s);
    return 0;
}

static int
case_cone_invalid_axis(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(s, 2);
    struct ik_constraint_t* c;

    attach_effector(s, root->v->find_child(root, 2), 0.0, 2.0, 0.0);
    c = attach_cone(s, root->v->find_child(root, 1), 0, 0, 0, 0.3);
    s->flags = IK_ENABLE_CONSTRAINTS;
    ik.solver.set_tree(s, root);
    TASK_CHECK(ik.solver.rebuild(s) == IK_INVALID_CONSTRAINT);
    /* A failed rebuild must not leave a state that can still be solved. */
    TASK_CHECK(ik.solver.solve(s) != IK_OK);

    /* Repairing the constraint and rebuilding restores a usable solver. */
    c->cone_axis.x = 1.0;
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    TASK_CHECK(solve_completed(ik.solver.solve(s)));

    ik.solver.destroy(s);
    return 0;
}

static int
case_cone_invalid_angle(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(s, 2);
    struct ik_constraint_t* c;

    attach_effector(s, root->v->find_child(root, 2), 0.0, 2.0, 0.0);
    c = attach_cone(s, root->v->find_child(root, 1), 1, 0, 0, M_PI);
    s->flags = IK_ENABLE_CONSTRAINTS;
    ik.solver.set_tree(s, root);

    c->cone_angle = M_PI + 0.1;
    TASK_CHECK(ik.solver.rebuild(s) == IK_INVALID_CONSTRAINT);
    TASK_CHECK(ik.solver.solve(s) != IK_OK);

    c->cone_angle = -0.1;
    TASK_CHECK(ik.solver.rebuild(s) == IK_INVALID_CONSTRAINT);

    c->cone_angle = INFINITY;
    TASK_CHECK(ik.solver.rebuild(s) == IK_INVALID_CONSTRAINT);

    c->cone_angle = NAN;
    TASK_CHECK(ik.solver.rebuild(s) == IK_INVALID_CONSTRAINT);

    c->cone_axis.x = NAN;
    c->cone_angle = 0.3;
    TASK_CHECK(ik.solver.rebuild(s) == IK_INVALID_CONSTRAINT);

    c->cone_axis.x = 0.0;
    c->cone_angle = 0.3;
    TASK_CHECK(ik.solver.rebuild(s) == IK_INVALID_CONSTRAINT);

    c->cone_axis.x = 1.0;
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    TASK_CHECK(solve_completed(ik.solver.solve(s)));

    ik.solver.destroy(s);
    return 0;
}

static int
case_cone_pi_is_unrestricted(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(s, 2);
    struct world_node world[MAX_NODES];
    int count;

    /* A target well inside the reachable envelope avoids the slow folding
     * regime where FABRIK converges linearly near full extension. */
    attach_effector(s, root->v->find_child(root, 2), 0.6, 1.6, 0.0);
    /* Default cone: angle PI must not restrict the swing at all. */
    attach_cone(s, root->v->find_child(root, 1), 1, 0, 0, M_PI);
    attach_cone(s, root->v->find_child(root, 2), 1, 0, 0, M_PI);
    s->flags = IK_ENABLE_CONSTRAINTS;
    configure_solver(s);
    ik.solver.set_tree(s, root);
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    TASK_CHECK(solve_completed(ik.solver.solve(s)));

    count = collect_world(root, world);
    {
        const struct world_node* tip = find_world(world, count, 2);
        TASK_CHECK(fabs(tip->position.x - 0.6) < 1e-3);
        TASK_CHECK(fabs(tip->position.y - 1.6) < 1e-3);
    }

    ik.solver.destroy(s);
    return 0;
}

static int
case_cone_joint_rotation_consistency(void)
{
    struct ik_solver_t* a = ik.solver.create(IK_FABRIK);
    struct ik_solver_t* b = ik.solver.create(IK_FABRIK);
    struct ik_node_t* ra = build_chain(a, 2);
    struct ik_node_t* rb = build_chain(b, 2);
    struct world_node wa[MAX_NODES], wb[MAX_NODES];
    int ca, cb, i, finite_ok = 1;

    attach_effector(a, ra->v->find_child(ra, 2), 1.9, 0.4, 0.0);
    attach_effector(b, rb->v->find_child(rb, 2), 1.9, 0.4, 0.0);
    attach_cone(a, ra->v->find_child(ra, 1), 1, 0, 0, 0.5);
    attach_cone(a, ra->v->find_child(ra, 2), 1, 0, 0, 0.5);
    attach_cone(b, rb->v->find_child(rb, 1), 1, 0, 0, 0.5);
    attach_cone(b, rb->v->find_child(rb, 2), 1, 0, 0, 0.5);
    a->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    b->flags = IK_ENABLE_CONSTRAINTS;
    configure_solver(a);
    configure_solver(b);
    ik.solver.set_tree(a, ra);
    ik.solver.set_tree(b, rb);
    TASK_CHECK(ik.solver.rebuild(a) == IK_OK);
    TASK_CHECK(ik.solver.rebuild(b) == IK_OK);
    TASK_CHECK(solve_completed(ik.solver.solve(a)));
    TASK_CHECK(solve_completed(ik.solver.solve(b)));

    check_finite_tree(ra, &finite_ok);
    TASK_CHECK(finite_ok);

    ca = collect_world(ra, wa);
    cb = collect_world(rb, wb);
    TASK_CHECK(ca == cb);

    /* Rotations and positions describe one pose: world positions match the
     * solver that does not export joint rotations. */
    for (i = 0; i < ca; ++i)
    {
        const struct world_node* other = find_world(wb, cb, wa[i].guid);
        TASK_CHECK(other != NULL);
        TASK_CHECK(fabs(wa[i].position.x - other->position.x) < 1e-4);
        TASK_CHECK(fabs(wa[i].position.y - other->position.y) < 1e-4);
        TASK_CHECK(fabs(wa[i].position.z - other->position.z) < 1e-4);
    }

    /* Exported rotations stay unit quaternions. */
    NODE_FOR_EACH(ra, guid, node)
        double m = vec3_len(&(ik_vec3_t){{node->rotation.x, node->rotation.y, node->rotation.z}});
        TASK_CHECK(fabs(m * m + node->rotation.w * node->rotation.w - 1.0) < 1e-6);
    NODE_END_EACH

    ik.solver.destroy(a);
    ik.solver.destroy(b);
    return 0;
}

static int
case_cone_antiparallel_axis_rule(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(s, 1);
    struct world_node world[MAX_NODES];
    int count;
    ik_vec3_t d0, ref;

    /* Target directly behind the root: the solved bone is antiparallel to
     * the bind incoming direction, exercising the deterministic axis rule. */
    attach_effector(s, root->v->find_child(root, 1), -1.0, 0.0, 0.0);
    attach_cone(s, root->v->find_child(root, 1), 1, 0, 0, M_PI);
    s->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_JOINT_ROTATIONS;
    configure_solver(s);
    ik.solver.set_tree(s, root);
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    TASK_CHECK(solve_completed(ik.solver.solve(s)));

    count = collect_world(root, world);
    ref = top_reference_axis(root);
    d0 = bone_direction(world, count, 1, 0);
    TASK_CHECK(fabs(bone_length(world, count, 1, 0) - 1.0) < 1e-6);
    /* Folded fully backwards: direction is the negated reference axis. */
    TASK_CHECK(fabs(angle_between(&d0, &ref) - M_PI) < 1e-4);

    /*
     * The bind-frame axes least parallel to +X are Y and Z (a tie); the
     * rule resolves ties in X, Y, Z order, so the swing is about Y.
     */
    {
        const struct world_node* node = find_world(world, count, 1);
        TASK_CHECK(fabs(fabs(node->rotation.y) - 1.0) < 1e-4);
        TASK_CHECK(fabs(node->rotation.x) < 1e-4);
        TASK_CHECK(fabs(node->rotation.z) < 1e-4);
        TASK_CHECK(fabs(node->rotation.w) < 1e-4);
    }

    /* A tight cone forbids the fold: the bone must stay on the cone surface
     * and remain finite rather than flipping through the antiparallel case. */
    {
        struct ik_node_t* n1 = root->v->find_child(root, 1);
        n1->constraint->cone_angle = 0.3;
        n1->position.x = 1.0; n1->position.y = 0.0; n1->position.z = 0.0;
        TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
        ik.solver.solve(s);
        count = collect_world(root, world);
        d0 = bone_direction(world, count, 1, 0);
        TASK_CHECK(angle_between(&d0, &ref) <= 0.3 + 1e-4);
        TASK_CHECK(fabs(bone_length(world, count, 1, 0) - 1.0) < 1e-6);
    }

    ik.solver.destroy(s);
    return 0;
}

static int
case_cone_target_rotation_conflict(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(s, 2);

    attach_effector(s, root->v->find_child(root, 2), 0.0, 2.0, 0.0);
    attach_cone(s, root->v->find_child(root, 1), 1, 0, 0, 0.3);
    s->flags = IK_ENABLE_CONSTRAINTS | IK_ENABLE_TARGET_ROTATIONS;
    ik.solver.set_tree(s, root);
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    TASK_CHECK(ik.solver.solve(s) == IK_CONFLICTING_SOLVER_OPTIONS);

    /* Dropping either flag makes the combination legal again. */
    s->flags = IK_ENABLE_CONSTRAINTS;
    TASK_CHECK(solve_completed(ik.solver.solve(s)));

    ik.solver.destroy(s);
    return 0;
}

/* ========================================================================= */
/* branch group */
/* ========================================================================= */

/*
 *        a1 -- a2 (effector A)
 *       /
 * root - s
 *       \
 *        b1 -- b2 (effector B)
 *
 * guids: root 0, s 1, a1 2, a2 3, b1 4, b2 5
 */
static struct ik_node_t*
build_branch(struct ik_solver_t* s)
{
    struct ik_node_t* root = s->node->create(0);
    struct ik_node_t* split = s->node->create(1);
    struct ik_node_t* a1 = s->node->create(2);
    struct ik_node_t* a2 = s->node->create(3);
    struct ik_node_t* b1 = s->node->create(4);
    struct ik_node_t* b2 = s->node->create(5);
    struct ik_node_t* all[6] = {root, split, a1, a2, b1, b2};
    int i;

    for (i = 0; i < 6; ++i)
    {
        all[i]->rotation.w = 1.0;
        if (all[i] != root)
            all[i]->position.x = 1.0;
    }

    s->node->add_child(root, split);
    s->node->add_child(split, a1);
    s->node->add_child(a1, a2);
    s->node->add_child(split, b1);
    s->node->add_child(b1, b2);
    return root;
}

static int
case_branch_shared_ancestor(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_branch(s);
    struct world_node world[MAX_NODES];
    int count;
    ik_vec3_t root_s, s_a1, a1_a2, s_b1, b1_b2, ref;

    attach_effector(s, root->v->find_child(root, 3), 2.9, 0.6, 0.0);
    attach_effector(s, root->v->find_child(root, 5), 2.9, -0.6, 0.0);
    /* The cone on the shared split node is reached by both child chains. */
    attach_cone(s, root->v->find_child(root, 1), 1, 0, 0, 0.35);
    attach_cone(s, root->v->find_child(root, 2), 1, 0, 0, 0.35);
    attach_cone(s, root->v->find_child(root, 3), 1, 0, 0, 0.35);
    attach_cone(s, root->v->find_child(root, 4), 1, 0, 0, 0.35);
    attach_cone(s, root->v->find_child(root, 5), 1, 0, 0, 0.35);
    s->flags = IK_ENABLE_CONSTRAINTS;
    configure_solver(s);
    ik.solver.set_tree(s, root);
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    ik.solver.solve(s);

    count = collect_world(root, world);
    TASK_CHECK(count == 6);

    /* Both end chains satisfy their own cones. */
    ref = top_reference_axis(root);
    root_s = bone_direction(world, count, 1, 0);
    s_a1  = bone_direction(world, count, 2, 1);
    a1_a2 = bone_direction(world, count, 3, 2);
    s_b1  = bone_direction(world, count, 4, 1);
    b1_b2 = bone_direction(world, count, 5, 4);
    TASK_CHECK(angle_between(&root_s, &ref) <= 0.35 + 1e-4);
    TASK_CHECK(angle_between(&s_a1, &root_s) <= 0.35 + 1e-4);
    TASK_CHECK(angle_between(&a1_a2, &s_a1) <= 0.35 + 1e-4);
    TASK_CHECK(angle_between(&s_b1, &root_s) <= 0.35 + 1e-4);
    TASK_CHECK(angle_between(&b1_b2, &s_b1) <= 0.35 + 1e-4);

    /* Bone lengths and connectivity survive both branches pulling. */
    TASK_CHECK(fabs(bone_length(world, count, 1, 0) - 1.0) < 1e-6);
    TASK_CHECK(fabs(bone_length(world, count, 2, 1) - 1.0) < 1e-6);
    TASK_CHECK(fabs(bone_length(world, count, 3, 2) - 1.0) < 1e-6);
    TASK_CHECK(fabs(bone_length(world, count, 4, 1) - 1.0) < 1e-6);
    TASK_CHECK(fabs(bone_length(world, count, 5, 4) - 1.0) < 1e-6);

    /* The shared node has exactly one coherent position for both branches. */
    {
        const struct world_node* split = find_world(world, count, 1);
        const struct world_node* a1n = find_world(world, count, 2);
        const struct world_node* b1n = find_world(world, count, 4);
        ik_vec3_t da = vec3_sub(a1n->position, &split->position);
        ik_vec3_t db = vec3_sub(b1n->position, &split->position);
        TASK_CHECK(fabs(vec3_len(&da) - 1.0) < 1e-6);
        TASK_CHECK(fabs(vec3_len(&db) - 1.0) < 1e-6);
    }

    ik.solver.destroy(s);
    return 0;
}

static int
case_branch_unreachable_finite(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_branch(s);
    struct world_node world[MAX_NODES];
    int count, finite_ok = 1;
    ik_vec3_t root_s, s_a1, a1_a2, s_b1, b1_b2, ref;

    attach_effector(s, root->v->find_child(root, 3), 10.0, 10.0, 0.0);
    attach_effector(s, root->v->find_child(root, 5), 10.0, -10.0, 0.0);
    attach_cone(s, root->v->find_child(root, 2), 1, 0, 0, 0.35);
    attach_cone(s, root->v->find_child(root, 3), 1, 0, 0, 0.35);
    attach_cone(s, root->v->find_child(root, 4), 1, 0, 0, 0.35);
    attach_cone(s, root->v->find_child(root, 5), 1, 0, 0, 0.35);
    s->flags = IK_ENABLE_CONSTRAINTS;
    configure_solver(s);
    ik.solver.set_tree(s, root);
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    ik.solver.solve(s);

    check_finite_tree(root, &finite_ok);
    TASK_CHECK(finite_ok);

    count = collect_world(root, world);
    ref = top_reference_axis(root);
    root_s = bone_direction(world, count, 1, 0);
    s_a1  = bone_direction(world, count, 2, 1);
    a1_a2 = bone_direction(world, count, 3, 2);
    s_b1  = bone_direction(world, count, 4, 1);
    b1_b2 = bone_direction(world, count, 5, 4);
    TASK_CHECK(angle_between(&root_s, &ref) <= 0.35 + 1e-4);
    TASK_CHECK(angle_between(&s_a1, &root_s) <= 0.35 + 1e-4);
    TASK_CHECK(angle_between(&a1_a2, &s_a1) <= 0.35 + 1e-4);
    TASK_CHECK(angle_between(&s_b1, &root_s) <= 0.35 + 1e-4);
    TASK_CHECK(angle_between(&b1_b2, &s_b1) <= 0.35 + 1e-4);

    /* Distance errors remain but the shared skeleton is still intact. */
    TASK_CHECK(fabs(bone_length(world, count, 1, 0) - 1.0) < 1e-6);
    TASK_CHECK(fabs(bone_length(world, count, 3, 2) - 1.0) < 1e-6);
    TASK_CHECK(fabs(bone_length(world, count, 5, 4) - 1.0) < 1e-6);

    ik.solver.destroy(s);
    return 0;
}

/* ========================================================================= */
/* rebind group */
/* ========================================================================= */

static int
case_rebind_root_motion(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(s, 2);
    struct world_node world[MAX_NODES];
    int count;
    ik_vec3_t d0, d1, ref;

    /* Local target (1.9, 0.3), which Rz(90deg) maps to world (-0.3, 1.9).
     * The required relative bend (~0.56 rad) fits inside 0.7 rad cones. */
    attach_effector(s, root->v->find_child(root, 2), 4.7, 6.9, 0.0);
    attach_cone(s, root->v->find_child(root, 1), 1, 0, 0, 0.7);
    attach_cone(s, root->v->find_child(root, 2), 1, 0, 0, 0.7);
    s->flags = IK_ENABLE_CONSTRAINTS;
    configure_solver(s);

    /* Animated root: translated to (5,5,0) and rotated 90 degrees about Z,
     * so the +X cone axes swing to +Y. */
    root->position.x = 5.0;
    root->position.y = 5.0;
    root->rotation = quat_axis_angle(0.0, 0.0, 1.0, M_PI * 0.5);

    ik.solver.set_tree(s, root);
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    TASK_CHECK(solve_completed(ik.solver.solve(s)));

    count = collect_world(root, world);
    {
        const struct world_node* tip = find_world(world, count, 2);
        TASK_CHECK(fabs(tip->position.x - 4.7) < 1e-3);
        TASK_CHECK(fabs(tip->position.y - 6.9) < 1e-3);
    }
    ref = top_reference_axis(root);
    d0 = bone_direction(world, count, 1, 0);
    d1 = bone_direction(world, count, 2, 1);
    TASK_CHECK(angle_between(&d0, &ref) <= 0.7 + 1e-4);
    TASK_CHECK(angle_between(&d1, &d0) <= 0.7 + 1e-4);

    /* A target lying in world +X is now outside the carried cone. */
    {
        struct ik_effector_t* e = root->v->find_child(root, 2)->effector;
        e->target_position.x = 7.0;
        e->target_position.y = 5.0;
        e->target_position.z = 0.0;
        ik.solver.solve(s);
        count = collect_world(root, world);
        d0 = bone_direction(world, count, 1, 0);
        d1 = bone_direction(world, count, 2, 1);
        TASK_CHECK(angle_between(&d0, &ref) <= 0.7 + 1e-4);
        TASK_CHECK(angle_between(&d1, &d0) <= 0.7 + 1e-4);
        {
            const struct world_node* tip = find_world(world, count, 2);
            ik_vec3_t target = {{7.0, 5.0, 0.0}};
            ik_vec3_t miss = vec3_sub(tip->position, &target);
            TASK_CHECK(vec3_len(&miss) > 0.5);
        }
    }

    ik.solver.destroy(s);
    return 0;
}

static int
case_rebind_ancestor_limit(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(s, 3);
    struct world_node world[MAX_NODES];
    int count, finite_ok = 1;
    ik_vec3_t d0, ref;

    /*
     * Only the ancestor bone (root -> node 1) gets a tight cone. The deeper
     * bones are free to bend towards the sideways target, but the ancestor
     * must stay on its bound axis.
     */
    attach_effector(s, root->v->find_child(root, 3), 1.5, 1.3, 0.0);
    attach_cone(s, root->v->find_child(root, 1), 1, 0, 0, 0.2);
    s->flags = IK_ENABLE_CONSTRAINTS;
    configure_solver(s);
    ik.solver.set_tree(s, root);
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    ik.solver.solve(s);

    check_finite_tree(root, &finite_ok);
    TASK_CHECK(finite_ok);

    count = collect_world(root, world);
    ref = top_reference_axis(root);
    d0 = bone_direction(world, count, 1, 0);
    TASK_CHECK(angle_between(&d0, &ref) <= 0.2 + 1e-4);
    TASK_CHECK(fabs(bone_length(world, count, 1, 0) - 1.0) < 1e-6);
    TASK_CHECK(fabs(bone_length(world, count, 2, 1) - 1.0) < 1e-6);
    TASK_CHECK(fabs(bone_length(world, count, 3, 2) - 1.0) < 1e-6);

    ik.solver.destroy(s);
    return 0;
}

static int
case_rebind_explicit(void)
{
    struct ik_solver_t* s = ik.solver.create(IK_FABRIK);
    struct ik_node_t* root = build_chain(s, 2);
    struct ik_constraint_t* c;
    struct world_node world[MAX_NODES];
    int count;
    ik_vec3_t ref, d0, d1;

    c = attach_cone(s, root->v->find_child(root, 1), 1, 0, 0, 0.3);
    attach_effector(s, root->v->find_child(root, 2), 0.0, 2.0, 0.0);
    s->flags = IK_ENABLE_CONSTRAINTS;
    configure_solver(s);
    ik.solver.set_tree(s, root);
    TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    ik.solver.solve(s);

    /*
     * Change the cone parameters without rebuilding. The new axis must be
     * ignored: the bound +X cone still blocks the +Y target.
     */
    c->cone_axis.x = 0.0;
    c->cone_axis.z = 1.0;
    {
        struct ik_effector_t* e = root->v->find_child(root, 2)->effector;
        e->target_position.x = 0.0;
        e->target_position.y = 2.0;
        e->target_position.z = 0.0;
    }
    ik.solver.solve(s);
    count = collect_world(root, world);
    ref = top_reference_axis(root);
    d0 = bone_direction(world, count, 1, 0);
    d1 = bone_direction(world, count, 2, 1);
    /* Only node 1 carries a cone, so only the first bone is limited. */
    TASK_CHECK(angle_between(&d0, &ref) <= 0.3 + 1e-4);
    /* The unconstrained second bone keeps its length while reaching for +Y. */
    TASK_CHECK(fabs(bone_length(world, count, 2, 1) - 1.0) < 1e-6);
    (void)d1;

    /* Reset to a straight reference pose and rebuild: the reference pose is
     * explicitly re-bound to +Z so a +Z target becomes reachable. */
    c->cone_angle = 1.2;
    {
        struct ik_node_t* n1 = root->v->find_child(root, 1);
        struct ik_node_t* n2 = root->v->find_child(root, 2);
        n1->position.x = 1.0; n1->position.y = 0.0; n1->position.z = 0.0;
        n2->position.x = 1.0; n2->position.y = 0.0; n2->position.z = 0.0;
        TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
    }
    {
        struct ik_effector_t* e = root->v->find_child(root, 2)->effector;
        e->target_position.x = 0.0;
        e->target_position.y = 0.0;
        e->target_position.z = 1.5;
    }
    TASK_CHECK(solve_completed(ik.solver.solve(s)));
    count = collect_world(root, world);
    {
        const struct world_node* tip = find_world(world, count, 2);
        TASK_CHECK(fabs(tip->position.z - 1.5) < 1e-3);
        TASK_CHECK(fabs(tip->position.x) < 1e-3);
        TASK_CHECK(fabs(tip->position.y) < 1e-3);
    }

    /* Changing the rest pose and rebuilding re-binds bone lengths too. */
    {
        struct ik_node_t* n1 = root->v->find_child(root, 1);
        n1->position.x = 2.0;
        n1->position.y = 0.0;
        n1->position.z = 0.0;
        TASK_CHECK(ik.solver.rebuild(s) == IK_OK);
        ik.solver.solve(s);
        count = collect_world(root, world);
        TASK_CHECK(fabs(bone_length(world, count, 1, 0) - 2.0) < 1e-6);
        TASK_CHECK(fabs(bone_length(world, count, 2, 1) - 1.0) < 1e-6);
    }

    ik.solver.destroy(s);
    return 0;
}

/* ========================================================================= */
/* Registration */
/* ========================================================================= */

const struct task_case task_cases[] = {
    {"cone", "reachable_within_half_angle",       case_cone_reachable},
    {"cone", "sideways_target_blocked",           case_cone_blocks_sideways},
    {"cone", "zero_axis_rejected_at_rebuild",     case_cone_invalid_axis},
    {"cone", "bad_angle_rejected_at_rebuild",     case_cone_invalid_angle},
    {"cone", "pi_half_angle_is_unrestricted",     case_cone_pi_is_unrestricted},
    {"cone", "joint_rotations_match_pose",        case_cone_joint_rotation_consistency},
    {"cone", "target_rotation_combination_refused", case_cone_target_rotation_conflict},
    {"cone", "antiparallel_swing_axis_rule",        case_cone_antiparallel_axis_rule},
    {"branch", "two_effectors_share_ancestor",    case_branch_shared_ancestor},
    {"branch", "unreachable_targets_stay_finite", case_branch_unreachable_finite},
    {"rebind", "root_animation_carries_cone",     case_rebind_root_motion},
    {"rebind", "ancestor_cone_holds",             case_rebind_ancestor_limit},
    {"rebind", "only_rebuild_rebinds",            case_rebind_explicit},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
