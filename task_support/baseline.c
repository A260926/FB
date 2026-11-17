#include "ik/ik.h"
#include <math.h>
#include <stdio.h>
static int run(int with_cone) {
    struct ik_solver_t *s=ik.solver.create(IK_FABRIK);
    struct ik_node_t *a=s->node->create(0), *b=s->node->create(1);
    struct ik_effector_t *e=s->effector->create();
    a->rotation.w=b->rotation.w=1;
    b->position.x=1;
    s->node->add_child(a,b);
    e->target_position.y=1;
    s->effector->attach(e,b);
    if(with_cone) { struct ik_constraint_t *c=s->constraint->create(IK_CONE); s->constraint->attach(c,b); }
    s->flags=with_cone?IK_ENABLE_CONSTRAINTS:0;
    ik.solver.set_tree(s,a);
    if(ik.solver.rebuild(s)!=IK_OK) return 2;
    ik.solver.solve(s);
    printf("%s endpoint=%.6f %.6f %.6f\n",with_cone?"CURRENT_CONE":"UNCONSTRAINED",(double)b->position.x,(double)b->position.y,(double)b->position.z);
    { int bad=fabs(b->position.x)>1e-6 || fabs(b->position.y-1)>1e-6 || fabs(b->position.z)>1e-6; ik.solver.destroy(s); return bad; }
}
int main(void) {int rc;ik.init();rc=run(0);if(!rc)rc=run(1);ik.deinit();if(!rc)puts("BASELINE_OK existing cone has no directional restriction");return rc;}
