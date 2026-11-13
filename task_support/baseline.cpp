#include "ufbx.h"
#include <cmath>
#include <cstdio>
#include <cstring>

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "baseline:%d: %s\n", __LINE__, #x); return 1; } } while (0)
static bool near(double a, double b) { return std::fabs(a-b) < 1e-9; }
static int report(const ufbx_error &error) {
    char text[1024]; ufbx_format_error(text, sizeof(text), &error);
    std::fprintf(stderr, "%s\n", text); return 1;
}
int main() {
    ufbx_error error = {};
    ufbx_load_opts load = {};
    load.load_external_files = false;
    ufbx_scene *source = ufbx_load_file("task_support/parent_chain.fbx", &load, &error);
    if (!source) return report(error);
    REQUIRE(source->constraints.count == 2 && source->meshes.count == 1);
    REQUIRE(source->skin_clusters.count == 1);
    for (size_t i = 0; i < source->nodes.count; ++i) {
        const ufbx_node *n = source->nodes.data[i];
        REQUIRE(n->inherit_mode == UFBX_INHERIT_MODE_NORMAL);
        REQUIRE(near(n->local_transform.scale.x, 1) && near(n->local_transform.scale.y, 1) && near(n->local_transform.scale.z, 1));
    }
    for (size_t i = 0; i < source->constraints.count; ++i) {
        const ufbx_constraint *c = source->constraints.data[i];
        REQUIRE(c->type == UFBX_CONSTRAINT_PARENT && c->active && near(c->weight, 1));
        REQUIRE(c->node && c->targets.count == 1 && c->targets.data[0].node);
        REQUIRE(near(c->targets.data[0].weight, 1));
        if (!std::strcmp(c->node->name.data, "A")) {
            REQUIRE(!std::strcmp(c->targets.data[0].node->name.data, "T"));
            REQUIRE(near(c->targets.data[0].transform.translation.x, 1));
        } else {
            REQUIRE(!std::strcmp(c->node->name.data, "B"));
            REQUIRE(!std::strcmp(c->targets.data[0].node->name.data, "C"));
            REQUIRE(near(c->targets.data[0].transform.translation.x, 0));
        }
    }
    ufbx_evaluate_opts opts = {};
    opts.evaluate_skinning = true;
    ufbx_scene *evaluated = ufbx_evaluate_scene(source, source->anim, 0.0, &opts, &error);
    if (!evaluated) return report(error);
    const ufbx_node *a = ufbx_find_node(evaluated, "A");
    const ufbx_node *b = ufbx_find_node(evaluated, "B");
    const ufbx_node *child = ufbx_find_node(evaluated, "C");
    const ufbx_node *t = ufbx_find_node(evaluated, "T");
    REQUIRE(a && b && child && t);
    REQUIRE(near(t->node_to_world.m00, 0) && near(t->node_to_world.m01, -1));
    REQUIRE(near(t->node_to_world.m10, 1) && near(t->node_to_world.m11, 0));
    REQUIRE(evaluated->skin_clusters.data[0]->bone_node == b);
    REQUIRE(near(a->node_to_world.m03, 0) && near(a->node_to_world.m13, 0));
    REQUIRE(near(b->node_to_world.m03, 0) && near(b->node_to_world.m13, 0));
    REQUIRE(near(child->node_to_world.m03, 1) && near(t->node_to_world.m03, 2));
    const ufbx_mesh *mesh = evaluated->meshes.data[0];
    REQUIRE(!mesh->skinned_is_local && mesh->skinned_position.values.count == 3);
    ufbx_vec3 p = mesh->skinned_position.values.data[0];
    REQUIRE(near(p.x, 1) && near(p.y, 0) && near(p.z, 0));
    REQUIRE(evaluated->constraints.data[0]->node != source->constraints.data[0]->node);
    REQUIRE(evaluated->constraints.data[0]->targets.data[0].node != source->constraints.data[0]->targets.data[0].node);
    std::printf("constraints=%zu skin_clusters=%zu vertices=%zu\n", evaluated->constraints.count, evaluated->skin_clusters.count, mesh->vertices.count);
    std::printf("default_pose A=(%.3f,%.3f) B=(%.3f,%.3f) C=(%.3f,%.3f)\n", a->node_to_world.m03, a->node_to_world.m13, b->node_to_world.m03, b->node_to_world.m13, child->node_to_world.m03, child->node_to_world.m13);
    std::printf("default_skinned_vertex=(%.3f,%.3f,%.3f)\nPARENT_BASELINE_OK\n", p.x, p.y, p.z);
    ufbx_free_scene(evaluated);
    ufbx_free_scene(source);
    return 0;
}
