#include <eely/clip/clip.h>
#include <eely/project/project.h>
#include <eely/project/project_uncooked.h>
#include <eely/skeleton/skeleton_uncooked.h>
#include <eely/skeleton/skeleton_pose.h>
#include <eely/clip/clip_uncooked.h>
#include <eely/anim_graph/anim_graph_uncooked.h>
#include <eely/anim_graph/anim_graph_node_clip.h>
#include <eely/anim_graph/anim_graph_node_blend.h>
#include <eely/anim_graph/anim_graph_node_param.h>
#include <eely/anim_graph/anim_graph_player.h>
#include <eely/params/params.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
static void require(bool value) { if (!value) throw std::runtime_error("tree baseline mismatch"); }
int main() {
    using namespace eely;
    project_uncooked input{measurement_unit::meters,axis_system::y_up_x_right_z_forward};
    auto& sk=input.add_resource<skeleton_uncooked>("rig");
    sk.get_joints()={{.id="root",.parent_index=std::nullopt,.rest_pose_transform={}},
        {.id="child",.parent_index=0,.rest_pose_transform={.translation={0,1,0}}}};
    for(int i=0;i<2;++i) {
        auto& c=input.add_resource<clip_uncooked>(i==0?"a":"b");
        c.set_target_skeleton_id("rig");c.set_compression_scheme(clip_compression_scheme::fixed);
        float x=i*4.0F;
        c.set_tracks({{.joint_id="root",.keys={{0.0F,{.translation=float3{x,0,0}}},{1.0F,{.translation=float3{x+2,0,0}}}}},
            {.joint_id="child",.keys={{0.0F,{.translation=float3{0,1,0}}},{1.0F,{.translation=float3{0,1,0}}}}}});
    }
    auto& g=input.add_resource<anim_graph_uncooked>("graph");g.set_skeleton_id("rig");
    auto& a=g.add_node<anim_graph_node_clip>();a.set_clip_id("a");
    auto& b=g.add_node<anim_graph_node_clip>();b.set_clip_id("b");
    auto& p=g.add_node<anim_graph_node_param>();p.set_param_id("weight");
    auto& blend=g.add_node<anim_graph_node_blend>();
    blend.get_pose_nodes()={{.id=a.get_id(),.factor=0},{.id=b.get_id(),.factor=1}};
    blend.set_factor_node_id(p.get_id());g.set_root_node_id(blend.get_id());
    std::vector<std::byte> storage(1024*1024);std::span<std::byte> bytes{storage};
    project::cook(input,bytes);project cooked{bytes};
    const auto* graph=cooked.get_resource<anim_graph>("graph");
    const auto* rig=cooked.get_resource<skeleton>("rig");require(graph&&rig);
    anim_graph_player player{*graph};skeleton_pose pose{*rig};params parameters;
    parameters.get_value<float>("weight")=.25F;
    player.play(0,parameters,pose);player.play(.5F,parameters,pose);
    const auto& root=pose.get_transform_object_space(0).translation;
    const auto& child=pose.get_transform_object_space(1).translation;
    require(std::fabs(root.x-2)<1e-3F&&std::fabs(child.x-2)<1e-3F&&std::fabs(child.y-1)<1e-3F);
    std::printf("EELY_FIXED_PRODUCTION_OK root_x=%.6f child_x=%.6f child_y=%.6f\n",root.x,child.x,child.y);
    const auto* clip_a=cooked.get_resource<clip>("a");require(clip_a!=nullptr);
    auto seek=clip_a->create_player();seek->play(.9F,pose);require(std::fabs(pose.get_transform_object_space(1).translation.x-1.8F)<1e-3F);
    seek->play(.1F,pose);require(std::fabs(pose.get_transform_object_space(1).translation.x-.2F)<1e-3F);
    std::printf("EELY_FIXED_REVERSE_OK child_x=%.6f\n",pose.get_transform_object_space(1).translation.x);
}
