/* Fixed consumer of the requested public API; contains no implementation. */
#include "anim.h"
#include <math.h>
int main(void) {
    struct anm_node *src=anm_create_node(), *copy;
    struct anm_track a,b;
    float m[16];
    if(!src) return 10;
    anm_use_node_animation(src,0);
    anm_set_position3f(src,3,0,0,0);
    copy=anm_clone_node_tree(src);
    if(!copy || copy==src || copy->parent || copy->next) return 11;
    anm_free_node_tree(src);
    anm_get_node_matrix(copy,m,1);
    if(fabsf(m[12]-3)>0.0002f) return 12;
    anm_free_node_tree(copy);
    if(anm_init_track(&a) || anm_init_track(&b)) return 13;
    anm_set_track_default(&a,2.5f);
    if(anm_share_track(&b,&a)) return 14;
    anm_destroy_track(&a);
    if(fabsf(anm_get_value(&b,1)-2.5f)>0.0002f) return 15;
    anm_destroy_track(&b);
    return 0;
}
