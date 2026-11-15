#include "ufbx.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"BASELINE failed line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static ufbx_vec3 transform(const ufbx_matrix *m,ufbx_vec3 p) {
    ufbx_vec3 q={m->m00*p.x+m->m01*p.y+m->m02*p.z+m->m03,
                 m->m10*p.x+m->m11*p.y+m->m12*p.z+m->m13,
                 m->m20*p.x+m->m21*p.y+m->m22*p.z+m->m23}; return q;
}
static double distance(ufbx_vec3 a,ufbx_vec3 b) {
    double x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return sqrt(x*x+y*y+z*z);
}
int main(void) {
    ufbx_error error;ufbx_load_opts load={0};
    ufbx_scene *scene=ufbx_load_file("data/maya_transformed_skin_7700_ascii.fbx",&load,&error);
    CHECK(scene);CHECK(scene->meshes.count>0);
    size_t mi=0;while(mi<scene->meshes.count&&!scene->meshes.data[mi]->skin_deformers.count)mi++;
    CHECK(mi<scene->meshes.count);
    ufbx_mesh *original=scene->meshes.data[mi];CHECK(original->num_vertices>0);CHECK(original->blend_deformers.count==0);
    ufbx_skin_deformer *base_skin=original->skin_deformers.data[0];CHECK(base_skin->skinning_method==UFBX_SKINNING_METHOD_LINEAR);
    CHECK(base_skin->weights.count>0);double source_weight=base_skin->weights.data[0].weight;
    ufbx_vec3 *first=calloc(original->num_vertices,sizeof(ufbx_vec3));CHECK(first);
    ufbx_matrix first_instance={0};double max_instance_delta=0;
    double max_motion=0;size_t weighted_checks=0;const double times[]={4.0/24.0,8.0/24.0,4.0/24.0};
    for(size_t sample=0;sample<3;sample++) {
        ufbx_evaluate_opts opts={0};opts.evaluate_skinning=true;
        ufbx_scene *frame=ufbx_evaluate_scene(scene,NULL,times[sample],&opts,&error);CHECK(frame);
        ufbx_mesh *mesh=frame->meshes.data[mi];CHECK(mesh->num_vertices==original->num_vertices);
        CHECK(mesh->skinned_position.values.count==mesh->num_vertices);CHECK(!mesh->skinned_is_local);
        ufbx_skin_deformer *skin=mesh->skin_deformers.data[0];
        CHECK(mesh->instances.count==1);
        if(sample==0)first_instance=mesh->instances.data[0]->geometry_to_world;
        if(sample==1)for(size_t k=0;k<12;k++){double d=fabs(first_instance.v[k]-mesh->instances.data[0]->geometry_to_world.v[k]);if(d>max_instance_delta)max_instance_delta=d;}
        for(size_t v=0;v<mesh->num_vertices;v++) {
            ufbx_skin_vertex vertex=skin->vertices.data[v];ufbx_vec3 p=mesh->vertices.data[v],reference={0};double sum=0;
            for(size_t j=0;j<vertex.num_weights;j++) {
                ufbx_skin_weight w=skin->weights.data[vertex.weight_begin+j];CHECK(w.cluster_index<skin->clusters.count);
                const ufbx_matrix *m=&skin->clusters.data[w.cluster_index]->geometry_to_world;ufbx_vec3 q=transform(m,p);
                reference.x+=w.weight*q.x;reference.y+=w.weight*q.y;reference.z+=w.weight*q.z;sum+=w.weight;
            }
            if(sum>1e-9) { reference.x/=sum;reference.y/=sum;reference.z/=sum;weighted_checks++;
                CHECK(distance(reference,mesh->skinned_position.values.data[v])<1e-7);
            }
            ufbx_vec3 actual=mesh->skinned_position.values.data[v];CHECK(isfinite(actual.x)&&isfinite(actual.y)&&isfinite(actual.z));
            if(sample==0)first[v]=actual;
            if(sample==1){double d=distance(first[v],actual);if(d>max_motion)max_motion=d;}
            if(sample==2)CHECK(distance(first[v],actual)<1e-9);
        }
        ufbx_free_scene(frame);
    }
    CHECK(weighted_checks>0);CHECK(max_instance_delta>1e-3);CHECK(max_motion<1e-7);CHECK(base_skin->weights.data[0].weight==source_weight);
    printf("BASELINE scene-lbs samples=3 vertices=%zu weighted_checks=%zu max_motion=%.9g max_instance_delta=%.9g\n",original->num_vertices,weighted_checks,max_motion,max_instance_delta);
    free(first);ufbx_free_scene(scene);puts("BASELINE animated-bind-space-and-repeat OK");return 0;
}
