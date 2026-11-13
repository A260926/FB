#include "ufbx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
static void check(bool ok, const char *msg) { if (!ok) { std::fprintf(stderr,"BASELINE_FAIL %s\n",msg); std::exit(1); } }
static void same(ufbx_vec3 a, ufbx_vec3 b) { check(std::fabs(a.x-b.x)<1e-8 && std::fabs(a.y-b.y)<1e-8 && std::fabs(a.z-b.z)<1e-8,"position"); }
int main() {
    ufbx_node node = {}; ufbx_skin_cluster a = {}, b = {};
    a.bone_node = b.bone_node = &node;
    a.geometry_to_world = b.geometry_to_world = ufbx_identity_matrix;
    a.geometry_to_world.m03 = 4.0; b.geometry_to_world.m13 = 6.0;
    ufbx_skin_cluster *clusters[] = {&a,&b};
    ufbx_skin_vertex vertices[] = {{0,2,0.0},{2,0,0.0}};
    ufbx_skin_weight weights[] = {{0,0.2},{1,0.3}};
    ufbx_skin_deformer skin = {};
    skin.clusters = {clusters,2}; skin.vertices = {vertices,2}; skin.weights = {weights,2};
    ufbx_matrix fallback = ufbx_identity_matrix; fallback.m03 = 11.0;
    ufbx_matrix m = ufbx_get_skin_vertex_matrix(&skin,0,&fallback);
    same(ufbx_transform_position(&m,ufbx_zero_vec3),ufbx_vec3{1.6,3.6,0.0});
    m=ufbx_get_skin_vertex_matrix(&skin,1,&fallback);
    same(ufbx_transform_position(&m,ufbx_zero_vec3),ufbx_vec3{11.0,0.0,0.0});
    std::puts("NORMALIZED_AND_FALLBACK_OK");
    ufbx_error err = {};
    ufbx_scene *scene = ufbx_load_file("data/blender_279_sausage_7400_binary.fbx",nullptr,&err);
    check(scene!=nullptr,"load sausage");
    auto *stack = reinterpret_cast<ufbx_anim_stack*>(ufbx_find_element(scene,UFBX_ELEMENT_ANIM_STACK,"Skeleton|Wiggle"));
    check(stack!=nullptr,"Wiggle stack");
    const double times[] = {0.0,0.2,0.5,0.2};
    double samples[4] = {}; size_t count=0;
    for (int ti=0;ti<4;ti++) {
        ufbx_evaluate_opts opts = {}; opts.evaluate_skinning=true;
        ufbx_scene *pose=ufbx_evaluate_scene(scene,stack->anim,times[ti],&opts,&err);
        check(pose!=nullptr,"evaluate scene");
        bool saw=false;
        for (size_t mi=0;mi<pose->meshes.count;mi++) {
            auto *mesh=pose->meshes.data[mi];
            if (!mesh->skin_deformers.count) continue;
            check(mesh->num_vertices>0 && mesh->instances.count==1,"mesh identity");
            auto *s=mesh->skin_deformers.data[0];
            check(s->skinning_method==UFBX_SKINNING_METHOD_LINEAR,"LBS fixture");
            check(mesh->skinned_position.values.count==mesh->num_vertices,"vertex count");
            auto *fb=&mesh->instances.data[0]->geometry_to_world;
            for (size_t vi=0;vi<mesh->num_vertices;vi++) {
                auto mat=ufbx_get_skin_vertex_matrix(s,vi,fb);
                auto v=ufbx_transform_position(&mat,mesh->vertices.data[vi]);
                auto actual=mesh->skinned_position.values.data[vi];
                check(std::isfinite(actual.x)&&std::isfinite(actual.y)&&std::isfinite(actual.z),"finite pose");
                same(v,actual); samples[ti] += (vi+1)*(v.x+3*v.y+7*v.z); count++; saw=true;
            }
        }
        check(saw,"real skin call"); ufbx_free_scene(pose);
    }
    check(std::fabs(samples[0]-samples[2])>1e-3,"animated geometry moves");
    check(std::fabs(samples[1]-samples[3])<1e-8,"out of order repeat");
    check(count>100,"real vertex coverage");
    std::printf("REAL_SKIN_SCENE_OK samples=4 vertex_calls=%zu\n",count);
    ufbx_free_scene(scene);
}
