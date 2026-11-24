#include "anim.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static int near(float a,float b){return fabsf(a-b)<0.0002f;}
int main(void) {
    struct anm_node *r=anm_create_node(), *c=anm_create_node();
    float m[16], inv[16], local[16], v;
    assert(r && c);
    anm_use_node_animation(r,0); anm_use_node_animation(c,0);
    anm_link_node(r,c);
    anm_set_position3f(r,0,0,0,0); anm_set_position3f(r,4,0,0,1000);
    anm_set_position3f(c,0,2,0,0);
    anm_get_node_matrix(c,local,500);
    assert(near(local[13],2));
    anm_get_matrix(c,m,500); anm_get_inv_matrix(c,inv,500);
    assert(near(m[12],2) && near(m[13],2));
    assert(near(inv[12],-2) && near(inv[13],-2));
    v=m[0]*1+m[4]*0+m[8]*0+m[12];
    assert(near(v,3)); /* bind vertex (1,0,0), identity inverse bind */
    anm_eval(r,500); assert(near(c->matrix[12],2));
    printf("ANIM_BASELINE world=(%.1f,%.1f) skin_x=%.1f\n",m[12],m[13],v);
    anm_free_node_tree(r);
    {
        struct anm_track a,b;
        assert(!anm_init_track(&a) && !anm_init_track(&b));
        assert(!anm_set_value(&a,0,1) && !anm_set_value(&a,1000,5));
        anm_copy_track(&b,&a);
        assert(a.keys != b.keys && near(anm_get_value(&b,500),3));
        assert(!anm_set_value(&a,0,9));
        assert(near(anm_get_value(&b,500),3));
        anm_destroy_track(&a); anm_destroy_track(&b);
        puts("TRACK_BASELINE independent_copy=1");
    }
    return 0;
}
