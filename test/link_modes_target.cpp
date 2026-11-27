// Tests for `ufbx_skin_link_mode`: UFBX_SKIN_LINK_TOTAL_ONE and
// UFBX_SKIN_LINK_ADDITIVE, both via the public structs and via generated
// ASCII FBX files loaded and evaluated with `ufbx_evaluate_scene()`.
#include "ufbx.h"
#include "check.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <sstream>

// The documented API contract: NORMALIZE is zero so zero-initialized data is
// treated as normalized skinning.
static_assert(UFBX_SKIN_LINK_NORMALIZE == 0, "UFBX_SKIN_LINK_NORMALIZE must be zero");
static_assert(UFBX_SKIN_LINK_TOTAL_ONE != UFBX_SKIN_LINK_ADDITIVE, "link modes must differ");

// ---------------------------------------------------------------------------
// Small independent linear algebra used to compute the expected values.
// `M3x4` is a 3x4 column-vector matrix: world = M * local.

struct M3x4 {
    double m[3][4];
};

static M3x4 identity_m() {
    M3x4 r = {};
    r.m[0][0] = 1.0; r.m[1][1] = 1.0; r.m[2][2] = 1.0;
    return r;
}

static M3x4 translation_m(double x, double y, double z) {
    M3x4 r = identity_m();
    r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z;
    return r;
}

static M3x4 rotation_z_m(double degrees) {
    M3x4 r = identity_m();
    double a = degrees * 3.14159265358979323846 / 180.0;
    double c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c; r.m[0][1] = -s;
    r.m[1][0] = s; r.m[1][1] = c;
    return r;
}

static M3x4 mul_m(const M3x4 &a, const M3x4 &b) {
    M3x4 r = {};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            r.m[i][j] = a.m[i][0]*b.m[0][j] + a.m[i][1]*b.m[1][j] + a.m[i][2]*b.m[2][j];
        }
        r.m[i][3] = a.m[i][0]*b.m[0][3] + a.m[i][1]*b.m[1][3] + a.m[i][2]*b.m[2][3] + a.m[i][3];
    }
    return r;
}

static M3x4 from_ufbx(const ufbx_matrix &u) {
    // ufbx stores `v[12]` as m00,m10,m20,m01,m11,m21,m02,m12,m22,m03,m13,m23.
    M3x4 r = {};
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) {
            r.m[i][j] = u.v[j*3 + i];
        }
    }
    for (int i = 0; i < 3; i++) r.m[i][3] = u.v[9 + i];
    return r;
}

static ufbx_matrix to_ufbx(const M3x4 &m) {
    ufbx_matrix u = ufbx_identity_matrix;
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) u.v[j*3 + i] = (ufbx_real)m.m[i][j];
    }
    for (int i = 0; i < 3; i++) u.v[9 + i] = (ufbx_real)m.m[i][3];
    return u;
}

static M3x4 inverse_m(const M3x4 &m) {
    double a = m.m[0][0], b = m.m[0][1], c = m.m[0][2];
    double d = m.m[1][0], e = m.m[1][1], f = m.m[1][2];
    double g = m.m[2][0], h = m.m[2][1], i = m.m[2][2];
    double A =  (e*i - f*h);
    double B = -(d*i - f*g);
    double C =  (d*h - e*g);
    double det = a*A + b*B + c*C;
    M3x4 r = {};
    double rcp = 1.0 / det;
    r.m[0][0] = A*rcp;              r.m[0][1] = -(b*i - c*h)*rcp;  r.m[0][2] =  (b*f - c*e)*rcp;
    r.m[1][0] = B*rcp;              r.m[1][1] =  (a*i - c*g)*rcp;  r.m[1][2] = -(a*f - c*d)*rcp;
    r.m[2][0] = C*rcp;              r.m[2][1] = -(a*h - b*g)*rcp;  r.m[2][2] =  (a*e - b*d)*rcp;
    double tx = m.m[0][3], ty = m.m[1][3], tz = m.m[2][3];
    r.m[0][3] = -(r.m[0][0]*tx + r.m[0][1]*ty + r.m[0][2]*tz);
    r.m[1][3] = -(r.m[1][0]*tx + r.m[1][1]*ty + r.m[1][2]*tz);
    r.m[2][3] = -(r.m[2][0]*tx + r.m[2][1]*ty + r.m[2][2]*tz);
    return r;
}

static M3x4 blend_m(double w_identity, const M3x4 &c, double w_c) {
    M3x4 r = {};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            double id = (i == j) ? 1.0 : 0.0;
            r.m[i][j] = w_identity*id + w_c*c.m[i][j];
        }
    }
    return r;
}

static bool close3(const ufbx_vec3 &v, double x, double y, double z) {
    return std::fabs((double)v.x - x) <= 1e-7
        && std::fabs((double)v.y - y) <= 1e-7
        && std::fabs((double)v.z - z) <= 1e-7;
}

static bool close_m(const ufbx_matrix &u, const M3x4 &ref) {
    M3x4 a = from_ufbx(u);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            if (std::fabs(a.m[i][j] - ref.m[i][j]) > 1e-7) return false;
        }
    }
    return true;
}

static ufbx_vec3 apply_m(const M3x4 &m, double x, double y, double z) {
    ufbx_vec3 r;
    r.x = (ufbx_real)(m.m[0][0]*x + m.m[0][1]*y + m.m[0][2]*z + m.m[0][3]);
    r.y = (ufbx_real)(m.m[1][0]*x + m.m[1][1]*y + m.m[1][2]*z + m.m[1][3]);
    r.z = (ufbx_real)(m.m[2][0]*x + m.m[2][1]*y + m.m[2][2]*z + m.m[2][3]);
    return r;
}

// Reference TOTAL_ONE: sum(w_i * W_i) + (1 - sum(w_i)) * F.
static M3x4 ref_total_one(const std::vector<std::pair<M3x4, double> > &influences, const M3x4 &f) {
    M3x4 r = {};
    double total = 0.0;
    for (size_t k = 0; k < influences.size(); k++) {
        for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) r.m[i][j] += influences[k].second * influences[k].first.m[i][j];
        total += influences[k].second;
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) r.m[i][j] += (1.0 - total) * f.m[i][j];
    return r;
}

// Reference ADDITIVE, matrices applied in the given order.
static M3x4 ref_additive(const std::vector<std::pair<M3x4, double> > &influences_in_order, const M3x4 &f) {
    M3x4 inv_f = inverse_m(f);
    M3x4 acc = identity_m();
    for (size_t k = 0; k < influences_in_order.size(); k++) {
        M3x4 c = mul_m(inv_f, influences_in_order[k].first);
        M3x4 a = blend_m(1.0 - influences_in_order[k].second, c, influences_in_order[k].second);
        acc = mul_m(a, acc);
    }
    return mul_m(f, acc);
}

// ---------------------------------------------------------------------------
// Hand-built `ufbx_skin_deformer` used by the `residual` and `ordered` groups.

struct HandSkin {
    std::vector<ufbx_node> nodes;
    std::vector<ufbx_skin_cluster> clusters;
    std::vector<ufbx_skin_cluster*> cluster_ptrs;
    std::vector<ufbx_skin_vertex> verts;
    std::vector<ufbx_skin_weight> weights;
    ufbx_skin_deformer skin;
};

static void hand_begin(HandSkin &hs, ufbx_skin_link_mode mode, size_t num_clusters) {
    hs.nodes.assign(num_clusters, ufbx_node());
    hs.clusters.assign(num_clusters, ufbx_skin_cluster());
    hs.cluster_ptrs.resize(num_clusters);
    for (size_t i = 0; i < num_clusters; i++) {
        hs.cluster_ptrs[i] = &hs.clusters[i];
        hs.clusters[i].bone_node = &hs.nodes[i];
        hs.clusters[i].geometry_to_world = ufbx_identity_matrix;
        hs.clusters[i].link_mode = mode;
        hs.clusters[i].link_order = (uint32_t)i;
    }
    hs.skin = ufbx_skin_deformer();
    hs.skin.link_mode = mode;
    hs.skin.clusters.data = hs.cluster_ptrs.data();
    hs.skin.clusters.count = num_clusters;
}

static void hand_set_cluster(HandSkin &hs, size_t i, const M3x4 &w, uint32_t link_order) {
    hs.clusters[i].geometry_to_world = to_ufbx(w);
    hs.clusters[i].link_order = link_order;
}

static void hand_set_vertices(HandSkin &hs, const std::vector<std::vector<std::pair<int, double> > > &per_vertex) {
    hs.verts.assign(per_vertex.size(), ufbx_skin_vertex());
    hs.weights.clear();
    for (size_t v = 0; v < per_vertex.size(); v++) {
        ufbx_skin_vertex &sv = hs.verts[v];
        sv.weight_begin = (uint32_t)hs.weights.size();
        sv.num_weights = (uint32_t)per_vertex[v].size();
        sv.dq_weight = 0.0f;
        for (size_t k = 0; k < per_vertex[v].size(); k++) {
            ufbx_skin_weight w;
            w.cluster_index = (uint32_t)per_vertex[v][k].first;
            w.weight = (ufbx_real)per_vertex[v][k].second;
            hs.weights.push_back(w);
        }
    }
    hs.skin.vertices.data = hs.verts.data();
    hs.skin.vertices.count = hs.verts.size();
    hs.skin.weights.data = hs.weights.data();
    hs.skin.weights.count = hs.weights.size();
}

// ---------------------------------------------------------------------------
// Tiny ASCII FBX generator used by the `scene` group.

namespace fbxgen {

struct ClusterDesc {
    std::string name;
    std::string mode; // empty for a missing `Mode`
    std::vector<int> indices;
    std::vector<double> weights;
    M3x4 transform;       // mesh_node_to_bone (`Transform`)
    M3x4 transform_link;  // bind_to_world (`TransformLink`)
    bool transform_associate_model = false;
};

struct AnimSpec {
    int model_index; // 0 = mesh node, 1.. = bone index
    std::vector<double> times;
    std::vector<double> xs;
    std::vector<double> ys;
    std::vector<double> zs;
};

struct FbxDesc {
    std::string mesh_node_name = "MeshNode";
    std::string mesh_parent_name = "MeshParent";
    M3x4 mesh_parent_local = identity_m();
    M3x4 mesh_node_local = identity_m();
    M3x4 geometry_local = identity_m();
    std::vector<double> vertices; // xyz triples
    std::vector<int> polygon_indices;
    std::string skinning_type = "Linear";
    std::vector<ClusterDesc> clusters;
    std::vector<M3x4> bone_locals;
    // Order in which cluster `OO` connections are written, an index permutation
    // into `clusters`. Defaults to the declaration (Objects) order.
    std::vector<size_t> cluster_connection_order;
    // Animation targets: 0 = mesh parent node, 1 = mesh node, 2.. = bones.
    std::vector<AnimSpec> anims;
    bool duplicate_mesh_instance = false;
    bool extra_skin = false;
    bool external_cache = false;
    bool associate_model_object = false;
};

static std::string fmt(double v) {
    char b[64];
    snprintf(b, sizeof(b), "%.9g", v);
    return std::string(b);
}

// FBX stores a transform as columns of the matrix followed by the translation.
static std::string matrix_list(const M3x4 &m) {
    std::string s;
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            double v;
            if (j < 3) v = i < 3 ? m.m[i][j] : 0.0;
            else v = i < 3 ? m.m[i][3] : 1.0;
            if (!s.empty()) s += ",";
            s += fmt(v);
        }
    }
    return s;
}

static long long ktime(double seconds) {
    return (long long)(seconds * 46186158000.0 + 0.5);
}

static std::string generate(const FbxDesc &d) {
    std::ostringstream o;
    unsigned base = 1000;
    unsigned mesh_model_id = base + 1;
    unsigned geom_id = base + 2;
    unsigned mesh2_model_id = base + 3;
    unsigned skin_id = base + 10;
    unsigned skin2_id = base + 11;
    unsigned cache_id = base + 12;
    unsigned assoc_id = base + 13;
    unsigned stack_id = base + 60;
    unsigned layer_id = base + 61;

    std::vector<unsigned> cluster_ids;
    std::vector<unsigned> bone_model_ids;
    for (size_t i = 0; i < d.clusters.size(); i++) {
        cluster_ids.push_back(base + 20 + (unsigned)i);
        bone_model_ids.push_back(base + 40 + (unsigned)i);
    }
    std::vector<size_t> conn_order = d.cluster_connection_order;
    if (conn_order.empty()) {
        for (size_t i = 0; i < d.clusters.size(); i++) conn_order.push_back(i);
    }

    o << "; FBX 7.4.0 project file\n";
    o << "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n}\n";
    o << "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n";
    o << "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n";
    o << "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n";
    o << "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n";
    o << "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n";
    o << "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n";
    o << "\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n";
    o << "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n";
    o << "\t}\n}\n";

    o << "Objects:  {\n";

    // Parent of the mesh node, used to test a moving mesh parent chain.
    unsigned mesh_parent_id = 0;
    if (!d.mesh_parent_name.empty()) {
        mesh_parent_id = base + 4;
        const M3x4 &t = d.mesh_parent_local;
        o << "\tModel: " << mesh_parent_id << ", \"Model::" << d.mesh_parent_name << "\", \"Null\" {\n";
        o << "\t\tVersion: 232\n\t\tProperties70:  {\n";
        o << "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\","
          << fmt(t.m[0][3]) << "," << fmt(t.m[1][3]) << "," << fmt(t.m[2][3]) << "\n";
        o << "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,0\n";
        o << "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n";
        o << "\t\t\tP: \"InheritType\", \"enum\", \"\", \"\",1\n";
        o << "\t\t}\n\t}\n";
    }

    o << "\tModel: " << mesh_model_id << ", \"Model::" << d.mesh_node_name << "\", \"Mesh\" {\n";
    o << "\t\tVersion: 232\n\t\tProperties70:  {\n";
    {
        const M3x4 &t = d.mesh_node_local;
        o << "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\","
          << fmt(t.m[0][3]) << "," << fmt(t.m[1][3]) << "," << fmt(t.m[2][3]) << "\n";
        o << "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,0\n";
        o << "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n";
    }
    {
        const M3x4 &g = d.geometry_local;
        o << "\t\t\tP: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\","
          << fmt(g.m[0][3]) << "," << fmt(g.m[1][3]) << "," << fmt(g.m[2][3]) << "\n";
        o << "\t\t\tP: \"GeometricRotation\", \"Vector3D\", \"Vector\", \"\",0,0,0\n";
        o << "\t\t\tP: \"GeometricScaling\", \"Vector3D\", \"Vector\", \"\",1,1,1\n";
    }
    o << "\t\t\tP: \"InheritType\", \"enum\", \"\", \"\",1\n";
    o << "\t\t}\n\t}\n";

    if (d.duplicate_mesh_instance) {
        o << "\tModel: " << mesh2_model_id << ", \"Model::MeshNode2\", \"Mesh\" {\n";
        o << "\t\tVersion: 232\n\t\tProperties70:  {\n";
        o << "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,0\n";
        o << "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,0\n";
        o << "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n";
        o << "\t\t\tP: \"InheritType\", \"enum\", \"\", \"\",1\n";
        o << "\t\t}\n\t}\n";
    }

    o << "\tGeometry: " << geom_id << ", \"Geometry::\", \"Mesh\" {\n";
    o << "\t\tVertices: *" << d.vertices.size() << " { a: ";
    for (size_t i = 0; i < d.vertices.size(); i++) {
        if (i) o << ",";
        o << fmt(d.vertices[i]);
    }
    o << " }\n";
    o << "\t\tPolygonVertexIndex: *" << d.polygon_indices.size() << " { a: ";
    for (size_t i = 0; i < d.polygon_indices.size(); i++) {
        if (i) o << ",";
        o << d.polygon_indices[i];
    }
    o << " }\n\t\tGeometryVersion: 124\n\t}\n";

    for (size_t i = 0; i < bone_model_ids.size(); i++) {
        const M3x4 &t = i < d.bone_locals.size() ? d.bone_locals[i] : identity_m();
        o << "\tModel: " << bone_model_ids[i] << ", \"Model::Bone" << i << "\", \"LimbNode\" {\n";
        o << "\t\tVersion: 232\n\t\tProperties70:  {\n";
        o << "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\","
          << fmt(t.m[0][3]) << "," << fmt(t.m[1][3]) << "," << fmt(t.m[2][3]) << "\n";
        o << "\t\t\tP: \"Lcl Rotation\", \"Lcl Rotation\", \"\", \"A\",0,0,0\n";
        o << "\t\t\tP: \"Lcl Scaling\", \"Lcl Scaling\", \"\", \"A\",1,1,1\n";
        o << "\t\t}\n\t}\n";
    }

    o << "\tDeformer: " << skin_id << ", \"Deformer::Skin\", \"Skin\" {\n";
    o << "\t\tVersion: 101\n\t\tSkinningType: \"" << d.skinning_type << "\"\n\t}\n";
    if (d.extra_skin) {
        o << "\tDeformer: " << skin2_id << ", \"Deformer::Skin2\", \"Skin\" {\n";
        o << "\t\tVersion: 101\n\t\tSkinningType: \"" << d.skinning_type << "\"\n\t}\n";
    }
    if (d.external_cache) {
        o << "\tDeformer: " << cache_id << ", \"Deformer::Cache\", \"VertexCacheDeformer\" {\n";
        o << "\t\tVersion: 100\n\t\tProperties70:  {\n";
        o << "\t\t\tP: \"CacheFileType\", \"enum\", \"\", \"\",0\n";
        o << "\t\t}\n\t}\n";
    }

    for (size_t i = 0; i < d.clusters.size(); i++) {
        const ClusterDesc &c = d.clusters[i];
        o << "\tDeformer: " << cluster_ids[i] << ", \"SubDeformer::" << c.name << "\", \"Cluster\" {\n";
        o << "\t\tVersion: 100\n";
        if (!c.mode.empty()) o << "\t\tMode: \"" << c.mode << "\"\n";
        o << "\t\tIndexes: *" << c.indices.size() << " { a: ";
        for (size_t k = 0; k < c.indices.size(); k++) { if (k) o << ","; o << c.indices[k]; }
        o << " }\n";
        o << "\t\tWeights: *" << c.weights.size() << " { a: ";
        for (size_t k = 0; k < c.weights.size(); k++) { if (k) o << ","; o << fmt(c.weights[k]); }
        o << " }\n";
        o << "\t\tTransform: *16 { a: " << matrix_list(c.transform) << " }\n";
        o << "\t\tTransformLink: *16 { a: " << matrix_list(c.transform_link) << " }\n";
        if (c.transform_associate_model) {
            o << "\t\tTransformAssociateModel: *16 { a: " << matrix_list(identity_m()) << " }\n";
        }
        o << "\t}\n";
    }

    if (d.associate_model_object) {
        o << "\tDeformer: " << assoc_id << ", \"Deformer::Associate\", \"AssociateModel\" {\n";
        o << "\t\tVersion: 100\n\t\tProperties70:  {\n";
        o << "\t\t\tP: \"Transform\", \"Lcl Translation\", \"\", \"A\",0,0,0\n";
        o << "\t\t}\n\t}\n";
    }

    // Animation objects.
    unsigned next_anim_id = base + 100;
    struct TempAnim { AnimSpec spec; unsigned curve_node_id; unsigned curve_ids[3]; };
    std::vector<TempAnim> temp_anims;
    bool has_anim = !d.anims.empty();
    if (has_anim) {
        o << "\tAnimationStack: " << stack_id << ", \"AnimStack::Take 001\", \"\" {\n";
        o << "\t\tProperties70:  {\n";
        o << "\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\"," << ktime(10.0) << "\n";
        o << "\t\t}\n\t}\n";
        o << "\tAnimationLayer: " << layer_id << ", \"AnimLayer::BaseLayer\", \"\" {\n\t}\n";

        for (size_t a = 0; a < d.anims.size(); a++) {
            TempAnim ta;
            ta.spec = d.anims[a];
            ta.curve_node_id = next_anim_id++;
            for (int ax = 0; ax < 3; ax++) ta.curve_ids[ax] = next_anim_id++;
            temp_anims.push_back(ta);

            o << "\tAnimationCurveNode: " << ta.curve_node_id << ", \"AnimCurveNode::T\", \"\" {\n";
            o << "\t\tProperties70:  {\n";
            o << "\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n";
            o << "\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\",0\n";
            o << "\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0\n";
            o << "\t\t}\n\t}\n";

            for (int ax = 0; ax < 3; ax++) {
                const std::vector<double> &vals = ax == 0 ? ta.spec.xs : (ax == 1 ? ta.spec.ys : ta.spec.zs);
                o << "\tAnimationCurve: " << ta.curve_ids[ax] << ", \"AnimCurve::\", \"\" {\n";
                o << "\t\tDefault: 0\n\t\tKeyVer: 4009\n";
                o << "\t\tKeyTime: *" << ta.spec.times.size() << " { a: ";
                for (size_t k = 0; k < ta.spec.times.size(); k++) { if (k) o << ","; o << ktime(ta.spec.times[k]); }
                o << " }\n";
                o << "\t\tKeyValueFloat: *" << vals.size() << " { a: ";
                for (size_t k = 0; k < vals.size(); k++) { if (k) o << ","; o << fmt(vals[k]); }
                o << " }\n";
                o << "\t\tKeyAttrFlags: *" << ta.spec.times.size() << " { a: ";
                for (size_t k = 0; k < ta.spec.times.size(); k++) { if (k) o << ","; o << 4; }
                o << " }\n";
                o << "\t\tKeyAttrDataFloat: *" << ta.spec.times.size() * 4 << " { a: ";
                for (size_t k = 0; k < ta.spec.times.size() * 4; k++) { if (k) o << ","; o << 0; }
                o << " }\n";
                o << "\t\tKeyAttrRefCount: *" << ta.spec.times.size() << " { a: ";
                for (size_t k = 0; k < ta.spec.times.size(); k++) { if (k) o << ","; o << 1; }
                o << " }\n\t}\n";
            }
        }
    }

    o << "}\n";

    o << "Connections:  {\n";
    if (mesh_parent_id) {
        o << "\tC: \"OO\"," << mesh_parent_id << ",0\n";
        o << "\tC: \"OO\"," << mesh_model_id << "," << mesh_parent_id << "\n";
    } else {
        o << "\tC: \"OO\"," << mesh_model_id << ",0\n";
    }
    o << "\tC: \"OO\"," << geom_id << "," << mesh_model_id << "\n";
    if (d.duplicate_mesh_instance) {
        o << "\tC: \"OO\"," << geom_id << "," << mesh2_model_id << "\n";
        o << "\tC: \"OO\"," << mesh2_model_id << ",0\n";
    }
    for (size_t i = 0; i < bone_model_ids.size(); i++) {
        if (i == 0) o << "\tC: \"OO\"," << bone_model_ids[i] << ",0\n";
        else o << "\tC: \"OO\"," << bone_model_ids[i] << "," << bone_model_ids[i - 1] << "\n";
    }
    o << "\tC: \"OO\"," << skin_id << "," << geom_id << "\n";
    if (d.extra_skin) o << "\tC: \"OO\"," << skin2_id << "," << geom_id << "\n";
    if (d.external_cache) o << "\tC: \"OO\"," << cache_id << "," << geom_id << "\n";
    for (size_t k = 0; k < conn_order.size(); k++) {
        size_t i = conn_order[k];
        o << "\tC: \"OO\"," << cluster_ids[i] << "," << skin_id << "\n";
    }
    for (size_t i = 0; i < cluster_ids.size(); i++) {
        o << "\tC: \"OO\"," << bone_model_ids[i] << "," << cluster_ids[i] << "\n";
    }
    if (d.associate_model_object) {
        o << "\tC: \"OO\"," << assoc_id << "," << geom_id << "\n";
    }

    if (has_anim) {
        o << "\tC: \"OO\"," << layer_id << "," << stack_id << "\n";
        for (size_t a = 0; a < temp_anims.size(); a++) {
            TempAnim &ta = temp_anims[a];
            unsigned target;
            if (ta.spec.model_index == 0) target = mesh_parent_id ? mesh_parent_id : mesh_model_id;
            else if (ta.spec.model_index == 1 && mesh_parent_id) target = mesh_model_id;
            else {
                int bone_ix = ta.spec.model_index - (mesh_parent_id ? 2 : 1);
                target = bone_model_ids[bone_ix];
            }
            o << "\tC: \"OO\"," << ta.curve_node_id << "," << layer_id << "\n";
            o << "\tC: \"OP\"," << ta.curve_node_id << "," << target << ", \"Lcl Translation\"\n";
            o << "\tC: \"OP\"," << ta.curve_ids[0] << "," << ta.curve_node_id << ", \"d|X\"\n";
            o << "\tC: \"OP\"," << ta.curve_ids[1] << "," << ta.curve_node_id << ", \"d|Y\"\n";
            o << "\tC: \"OP\"," << ta.curve_ids[2] << "," << ta.curve_node_id << ", \"d|Z\"\n";
        }
    }
    o << "}\n";

    return o.str();
}

static ufbx_scene *load(const FbxDesc &d, const ufbx_load_opts *opts, ufbx_error *err) {
    std::string s = generate(d);
    return ufbx_load_memory(s.data(), s.size(), opts, err);
}

} // namespace fbxgen

// ---------------------------------------------------------------------------
// residual group

static void residual_total_one_scaled_probe() {
    // Non-identity `F`, weights summing to less than one.
    M3x4 w0 = translation_m(4, 0, 0);
    M3x4 w1 = translation_m(0, 6, 0);

    HandSkin hs;
    hand_begin(hs, UFBX_SKIN_LINK_TOTAL_ONE, 2);
    hand_set_cluster(hs, 0, w0, 0);
    hand_set_cluster(hs, 1, w1, 1);
    hand_set_vertices(hs, { { {0, 0.2}, {1, 0.3} } });

    M3x4 f = mul_m(rotation_z_m(30.0), translation_m(11, -2, 5));
    ufbx_matrix f_m = to_ufbx(f);
    ufbx_matrix got = ufbx_get_skin_vertex_matrix(&hs.skin, 0, &f_m);

    M3x4 ref = ref_total_one({ {w0, 0.2}, {w1, 0.3} }, f);
    TASK_CHECK(close_m(got, ref));

    // The residual term must use the actual `F`, not the identity.
    ufbx_vec3 p = ufbx_transform_position(&got, ufbx_vec3{0, 0, 0});
    TASK_CHECK(close3(p, ref.m[0][3], ref.m[1][3], ref.m[2][3]));
    M3x4 ref_identity = ref_total_one({ {w0, 0.2}, {w1, 0.3} }, identity_m());
    TASK_CHECK(!close3(p, ref_identity.m[0][3], ref_identity.m[1][3], ref_identity.m[2][3]));

    // The exact documented example: identity `F`, translations and weights .2/.3.
    HandSkin plain;
    hand_begin(plain, UFBX_SKIN_LINK_TOTAL_ONE, 2);
    hand_set_cluster(plain, 0, translation_m(4, 0, 0), 0);
    hand_set_cluster(plain, 1, translation_m(0, 6, 0), 1);
    hand_set_vertices(plain, { { {0, 0.2}, {1, 0.3} } });
    ufbx_matrix identity = ufbx_identity_matrix;
    ufbx_matrix plain_m = ufbx_get_skin_vertex_matrix(&plain.skin, 0, &identity);
    ufbx_vec3 origin = ufbx_transform_position(&plain_m, ufbx_vec3{0, 0, 0});
    TASK_CHECK(close3(origin, 0.8, 1.8, 0.0));

    // ... while the normalized path stays at (1.6, 3.6, 0).
    plain.skin.link_mode = UFBX_SKIN_LINK_NORMALIZE;
    ufbx_matrix plain_norm_m = ufbx_get_skin_vertex_matrix(&plain.skin, 0, &identity);
    ufbx_vec3 normalized = ufbx_transform_position(&plain_norm_m, ufbx_vec3{0, 0, 0});
    TASK_CHECK(close3(normalized, 1.6, 3.6, 0.0));
}

static void residual_total_one_overshoot() {
    // Weights summing to more than one including a single weight above one.
    M3x4 w0 = translation_m(3, 0, 0);
    M3x4 w1 = translation_m(0, 5, 0);

    HandSkin hs;
    hand_begin(hs, UFBX_SKIN_LINK_TOTAL_ONE, 2);
    hand_set_cluster(hs, 0, w0, 0);
    hand_set_cluster(hs, 1, w1, 1);
    hand_set_vertices(hs, { { {0, 1.25}, {1, 0.75} } });

    M3x4 f = translation_m(-1, 2, 4);
    ufbx_matrix f_m = to_ufbx(f);
    ufbx_matrix got = ufbx_get_skin_vertex_matrix(&hs.skin, 0, &f_m);

    M3x4 ref = ref_total_one({ {w0, 1.25}, {w1, 0.75} }, f);
    TASK_CHECK(close_m(got, ref));

    // Total weight is 2.0: the result must not be divided by it.
    M3x4 normalized_ref = ref_total_one({ {w0, 1.25 / 2.0}, {w1, 0.75 / 2.0} }, f);
    TASK_CHECK(!close_m(got, normalized_ref));

    // A single weight above one is applied verbatim.
    HandSkin single;
    hand_begin(single, UFBX_SKIN_LINK_TOTAL_ONE, 1);
    hand_set_cluster(single, 0, translation_m(10, 0, 0), 0);
    hand_set_vertices(single, { { {0, 2.0} } });
    ufbx_matrix identity = ufbx_identity_matrix;
    ufbx_matrix single_m = ufbx_get_skin_vertex_matrix(&single.skin, 0, &identity);
    ufbx_vec3 p = ufbx_transform_position(&single_m, ufbx_vec3{0, 0, 0});
    // sum(w*W) + (1 - w)*F: 2*translate(10) - 1*I, origin -> (20, 0, 0).
    TASK_CHECK(close3(p, 20.0, 0.0, 0.0));
}

static void residual_zero_influence_fallback() {
    // Vertices without influences use `F` exactly, identity when `F` is NULL.
    HandSkin hs;
    hand_begin(hs, UFBX_SKIN_LINK_TOTAL_ONE, 1);
    hand_set_cluster(hs, 0, translation_m(5, 0, 0), 0);
    hand_set_vertices(hs, { {}, { {0, 0.0} } });

    M3x4 f = mul_m(rotation_z_m(-45.0), translation_m(2, 3, 4));
    ufbx_matrix f_m = to_ufbx(f);
    TASK_CHECK(close_m(ufbx_get_skin_vertex_matrix(&hs.skin, 0, &f_m), f));
    TASK_CHECK(close_m(ufbx_get_skin_vertex_matrix(&hs.skin, 1, &f_m), f));
    TASK_CHECK(close_m(ufbx_get_skin_vertex_matrix(&hs.skin, 0, NULL), identity_m()));

    hs.skin.link_mode = UFBX_SKIN_LINK_ADDITIVE;
    TASK_CHECK(close_m(ufbx_get_skin_vertex_matrix(&hs.skin, 0, &f_m), f));
    TASK_CHECK(close_m(ufbx_get_skin_vertex_matrix(&hs.skin, 0, NULL), identity_m()));
}

// ---------------------------------------------------------------------------
// ordered group

static void ordered_additive_forward() {
    M3x4 rot = rotation_z_m(90.0);
    M3x4 trans = translation_m(2, 0, 0);

    // Connection order: rotation first (weight .25), translation second (.75).
    HandSkin hs;
    hand_begin(hs, UFBX_SKIN_LINK_ADDITIVE, 2);
    hand_set_cluster(hs, 0, rot, 0);
    hand_set_cluster(hs, 1, trans, 1);
    hand_set_vertices(hs, { { {0, 0.25}, {1, 0.75} } });

    ufbx_matrix f_m = ufbx_identity_matrix;
    ufbx_matrix got = ufbx_get_skin_vertex_matrix(&hs.skin, 0, &f_m);

    M3x4 ref = ref_additive({ {rot, 0.25}, {trans, 0.75} }, identity_m());
    TASK_CHECK(close_m(got, ref));
    ufbx_vec3 p = ufbx_transform_position(&got, ufbx_vec3{1, 0, 0});
    TASK_CHECK(close3(p, 2.25, 0.25, 0.0));
}

static void ordered_additive_swapped() {
    M3x4 rot = rotation_z_m(90.0);
    M3x4 trans = translation_m(2, 0, 0);

    // The clusters are connected in the opposite order, with the smaller weight
    // first, which must change the result.
    HandSkin hs;
    hand_begin(hs, UFBX_SKIN_LINK_ADDITIVE, 2);
    hand_set_cluster(hs, 0, rot, 1);
    hand_set_cluster(hs, 1, trans, 0);
    hand_set_vertices(hs, { { {0, 0.25}, {1, 0.75} } });

    ufbx_matrix f_m = ufbx_identity_matrix;
    ufbx_matrix got = ufbx_get_skin_vertex_matrix(&hs.skin, 0, &f_m);

    M3x4 ref = ref_additive({ {trans, 0.75}, {rot, 0.25} }, identity_m());
    TASK_CHECK(close_m(got, ref));
    ufbx_vec3 p = ufbx_transform_position(&got, ufbx_vec3{1, 0, 0});
    TASK_CHECK(close3(p, 1.875, 0.625, 0.0));

    M3x4 ref_forward = ref_additive({ {rot, 0.25}, {trans, 0.75} }, identity_m());
    TASK_CHECK(!close_m(got, ref_forward));
}

static void ordered_additive_not_weight_sorted() {
    M3x4 a = rotation_z_m(90.0);
    M3x4 b = translation_m(2, 0, 0);

    // The weights are deliberately in descending array order while the
    // connection order has the smaller weight first, so sorting by weight
    // would produce the wrong result.
    HandSkin hs;
    hand_begin(hs, UFBX_SKIN_LINK_ADDITIVE, 2);
    hand_set_cluster(hs, 0, a, 1); // weight .75, applied second
    hand_set_cluster(hs, 1, b, 0); // weight .25, applied first
    hand_set_vertices(hs, { { {0, 0.75}, {1, 0.25} } });

    ufbx_matrix f_m = ufbx_identity_matrix;
    ufbx_matrix got = ufbx_get_skin_vertex_matrix(&hs.skin, 0, &f_m);

    M3x4 ref = ref_additive({ {b, 0.25}, {a, 0.75} }, identity_m());
    TASK_CHECK(close_m(got, ref));

    // Descending-weight order would be a different matrix.
    M3x4 ref_sorted = ref_additive({ {a, 0.75}, {b, 0.25} }, identity_m());
    TASK_CHECK(!close_m(got, ref_sorted));
}

// ---------------------------------------------------------------------------
// scene group

struct SceneSetup {
    fbxgen::FbxDesc desc;
    // Bone local transforms at the two evaluated key times, in bone order.
    std::vector<M3x4> bone_local_at[2];
    M3x4 mesh_parent_local_at[2];
    M3x4 mesh_local_at[2];
    M3x4 geometry_local;
    std::vector<M3x4> cluster_transform;
    std::vector<std::vector<int> > cluster_indices;
    std::vector<std::vector<double> > cluster_weights;
};

// Build a scene with a single mesh, a bone chain, an animated mesh parent
// chain and bone translations, a static geometry transform and per-cluster
// transforms.
static SceneSetup make_scene_setup(ufbx_skin_link_mode mode, bool duplicate_weights_axis) {
    SceneSetup ss;
    (void)duplicate_weights_axis;
    fbxgen::FbxDesc &d = ss.desc;
    d.vertices = {
        0.0, 0.0, 0.0,
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.5, 0.5, 0.0,
    };
    d.polygon_indices = { 0, 1, -3 };

    d.geometry_local = translation_m(0, 0, 1);

    M3x4 b0_local_t0 = translation_m(0, 0, 0);
    M3x4 b0_local_t1 = translation_m(3, 0, 0);
    M3x4 b1_local = translation_m(0, 3, 0);
    // The mesh parent moves, the mesh node itself moves as well.
    ss.mesh_parent_local_at[0] = translation_m(4, 0, 0);
    ss.mesh_parent_local_at[1] = translation_m(4, -1, 0);
    ss.mesh_local_at[0] = translation_m(0, 0, 0);
    ss.mesh_local_at[1] = translation_m(0, -2, 0);
    ss.bone_local_at[0] = { b0_local_t0, b1_local };
    ss.bone_local_at[1] = { b0_local_t1, b1_local };

    d.bone_locals = ss.bone_local_at[0];
    d.mesh_parent_local = ss.mesh_parent_local_at[0];

    M3x4 t0 = rotation_z_m(90.0);
    M3x4 t1 = translation_m(2, 0, 0);
    M3x4 bind0 = translation_m(7, 8, 9);
    M3x4 bind1 = rotation_z_m(30.0);

    fbxgen::ClusterDesc c0;
    c0.name = "ClusterA";
    c0.mode = mode == UFBX_SKIN_LINK_TOTAL_ONE ? "Total1" : "Additive";
    c0.indices = { 0, 1, 2 };            // vertex 3 has no influence
    c0.weights = { 0.6, 0.6, 0.6 };
    c0.transform = t0;
    c0.transform_link = bind0;

    fbxgen::ClusterDesc c1;
    c1.name = "ClusterB";
    c1.mode = c0.mode;
    c1.indices = { 0, 1, 2 };
    c1.weights = { 0.7, 0.7, 0.7 };
    c1.transform = t1;
    c1.transform_link = bind1;

    d.clusters = { c0, c1 };
    ss.cluster_transform = { t0, t1 };
    ss.cluster_indices = { c0.indices, c1.indices };
    ss.cluster_weights = { c0.weights, c1.weights };
    ss.geometry_local = d.geometry_local;

    // Animation targets: 0 = mesh parent, 1 = mesh node, 2.. = bones.
    d.anims.push_back({ 0, { 0.0, 2.0 }, { 4.0, 4.0 }, { 0.0, -1.0 }, { 0.0, 0.0 } });
    d.anims.push_back({ 1, { 0.0, 2.0 }, { 0.0, 0.0 }, { 0.0, -2.0 }, { 0.0, 0.0 } });
    d.anims.push_back({ 2, { 0.0, 2.0 }, { 0.0, 3.0 }, { 0.0, 0.0 }, { 0.0, 0.0 } });
    return ss;
}

static M3x4 bone_world_at(const SceneSetup &ss, int time_index, size_t bone) {
    M3x4 m = identity_m();
    for (size_t i = 0; i <= bone; i++) m = mul_m(m, ss.bone_local_at[time_index][i]);
    return m;
}

// F at a given time: the mesh instance `geometry_to_world`, ie. the mesh parent
// and node chain times the geometry transform.
static M3x4 instance_f_at(const SceneSetup &ss, int time_index) {
    M3x4 node_to_world = mul_m(ss.mesh_parent_local_at[time_index], ss.mesh_local_at[time_index]);
    return mul_m(node_to_world, ss.geometry_local);
}

static ufbx_scene *eval_pose(ufbx_scene *scene, ufbx_anim *anim, double time) {
    ufbx_evaluate_opts opts = {};
    opts.evaluate_skinning = true;
    ufbx_error err = {};
    ufbx_scene *pose = ufbx_evaluate_scene(scene, anim, time, &opts, &err);
    return pose;
}

static ufbx_vec3 mesh_positions_of(ufbx_scene *scene, ufbx_scene *pose, size_t vi) {
    (void)scene;
    return pose->meshes.data[0]->skinned_position.values.data[vi];
}

static void scene_total_one_moving() {
    SceneSetup ss = make_scene_setup(UFBX_SKIN_LINK_TOTAL_ONE, false);
    ufbx_error err = {};
    ufbx_scene *scene = fbxgen::load(ss.desc, NULL, &err);
    TASK_CHECK(scene != NULL);
    if (!scene) { std::fprintf(stderr, "load: %s %s\n", err.description.data, err.info); return; }

    TASK_CHECK(scene->skin_deformers.count == 1);
    ufbx_skin_deformer *skin = scene->skin_deformers.data[0];
    TASK_CHECK(skin->link_mode == UFBX_SKIN_LINK_TOTAL_ONE);
    TASK_CHECK(skin->skinning_method == UFBX_SKINNING_METHOD_LINEAR);
    TASK_CHECK(skin->clusters.count == 2);
    TASK_CHECK(skin->clusters.data[0]->link_mode == UFBX_SKIN_LINK_TOTAL_ONE);
    TASK_CHECK(skin->clusters.data[1]->link_mode == UFBX_SKIN_LINK_TOTAL_ONE);

    // Non-identity bind matrices survive the load.
    TASK_CHECK(close_m(skin->clusters.data[0]->bind_to_world, translation_m(7, 8, 9)));
    TASK_CHECK(close_m(skin->clusters.data[1]->bind_to_world, rotation_z_m(30.0)));

    ufbx_anim_stack *stack = (ufbx_anim_stack*)ufbx_find_element(scene, UFBX_ELEMENT_ANIM_STACK, "Take 001");
    TASK_CHECK(stack != NULL);
    if (!stack) { ufbx_free_scene(scene); return; }
    ufbx_anim *anim = stack->anim;

    // Record the source scene state to verify it is not modified.
    ufbx_matrix src_cluster0 = scene->skin_clusters.data[0]->geometry_to_world;
    ufbx_mesh *src_mesh = scene->meshes.data[0];
    ufbx_matrix src_instance = src_mesh->instances.data[0]->geometry_to_world;

    const double times[2] = { 0.0, 2.0 };
    ufbx_vec3 got[2][4];
    for (int ti = 0; ti < 2; ti++) {
        ufbx_scene *pose = eval_pose(scene, anim, times[ti]);
        TASK_CHECK(pose != NULL);
        if (!pose) continue;
        TASK_CHECK(pose->meshes.count == 1);
        ufbx_mesh *mesh = pose->meshes.data[0];
        TASK_CHECK(mesh->skin_deformers.count == 1);
        TASK_CHECK(mesh->instances.count == 1);
        TASK_CHECK(mesh->skinned_is_local == false);
        TASK_CHECK(mesh->skinned_position.values.count == mesh->num_vertices);
        ufbx_skin_deformer *pose_skin = mesh->skin_deformers.data[0];
        TASK_CHECK(pose_skin->link_mode == UFBX_SKIN_LINK_TOTAL_ONE);
        ufbx_matrix &f_m = mesh->instances.data[0]->geometry_to_world;

        M3x4 g = ss.geometry_local;
        M3x4 f = instance_f_at(ss, ti);
        TASK_CHECK(close_m(f_m, f));

        for (size_t vi = 0; vi < mesh->num_vertices; vi++) {
            ufbx_vec3 p = mesh->vertices.data[vi];
            bool has_influence = vi < 3;
            ufbx_vec3 expected;
            if (has_influence) {
                std::vector<std::pair<M3x4, double> > inf;
                for (size_t c = 0; c < ss.cluster_transform.size(); c++) {
                    // Find the weight of this cluster for this vertex.
                    double w = 0.0;
                    for (size_t k = 0; k < ss.cluster_indices[c].size(); k++) {
                        if (ss.cluster_indices[c][k] == (int)vi) w = ss.cluster_weights[c][k];
                    }
                    M3x4 wm = mul_m(bone_world_at(ss, ti, c), mul_m(ss.cluster_transform[c], g));
                    inf.push_back({ wm, w });
                }
                M3x4 ref = ref_total_one(inf, f);
                expected = apply_m(ref, p.x, p.y, p.z);
            } else {
                expected = apply_m(f, p.x, p.y, p.z);
            }
            ufbx_vec3 actual = mesh->skinned_position.values.data[vi];
            TASK_CHECK(std::fabs((double)actual.x - (double)expected.x) <= 1e-7);
            TASK_CHECK(std::fabs((double)actual.y - (double)expected.y) <= 1e-7);
            TASK_CHECK(std::fabs((double)actual.z - (double)expected.z) <= 1e-7);
            got[ti][vi] = actual;

            // The matrix entry point must agree with the evaluated positions.
            ufbx_matrix mat = ufbx_get_skin_vertex_matrix(pose_skin, vi, &f_m);
            ufbx_vec3 via_matrix = ufbx_transform_position(&mat, p);
            TASK_CHECK(std::fabs((double)via_matrix.x - (double)actual.x) <= 1e-7);
            TASK_CHECK(std::fabs((double)via_matrix.y - (double)actual.y) <= 1e-7);
            TASK_CHECK(std::fabs((double)via_matrix.z - (double)actual.z) <= 1e-7);
        }

        ufbx_free_scene(pose);
    }

    // The mesh and the bones both move between the evaluated times.
    TASK_CHECK(!close3(got[0][0], got[1][0].x, got[1][0].y, got[1][0].z));
    // The vertex without influences must move exactly with the mesh instance.
    {
        M3x4 f0 = instance_f_at(ss, 0);
        M3x4 f1 = instance_f_at(ss, 1);
        TASK_CHECK(!close_m(to_ufbx(f0), f1));
        ufbx_vec3 p = { 0.5f, 0.5f, 0.0f };
        ufbx_vec3 e0 = apply_m(f0, p.x, p.y, p.z);
        ufbx_vec3 e1 = apply_m(f1, p.x, p.y, p.z);
        TASK_CHECK(close3(got[0][3], e0.x, e0.y, e0.z));
        TASK_CHECK(close3(got[1][3], e1.x, e1.y, e1.z));
    }

    // Loading with `evaluate_skinning = true` goes through the same code path.
    {
        ufbx_load_opts opts = {};
        opts.evaluate_skinning = true;
        ufbx_error load_err = {};
        ufbx_scene *direct = fbxgen::load(ss.desc, &opts, &load_err);
        TASK_CHECK(direct != NULL);
        if (direct) {
            TASK_CHECK(direct->meshes.count == 1);
            ufbx_mesh *dm = direct->meshes.data[0];
            TASK_CHECK(dm->skin_deformers.count == 1);
            TASK_CHECK(dm->skin_deformers.data[0]->link_mode == UFBX_SKIN_LINK_TOTAL_ONE);
            TASK_CHECK(dm->skinned_is_local == false);
            // At load time (`time = 0`) the result must match the evaluated pose at 0.
            for (size_t vi = 0; vi < 4; vi++) {
                ufbx_vec3 d = dm->skinned_position.values.data[vi];
                TASK_CHECK(std::fabs((double)d.x - (double)got[0][vi].x) <= 1e-6);
                TASK_CHECK(std::fabs((double)d.y - (double)got[0][vi].y) <= 1e-6);
                TASK_CHECK(std::fabs((double)d.z - (double)got[0][vi].z) <= 1e-6);
            }
            ufbx_free_scene(direct);
        }
    }

    // Evaluating again out of order and repeatedly gives the same results.
    ufbx_scene *pose_a = eval_pose(scene, anim, times[0]);
    ufbx_scene *pose_b = eval_pose(scene, anim, times[1]);
    ufbx_scene *pose_c = eval_pose(scene, anim, times[0]);
    TASK_CHECK(pose_a && pose_b && pose_c);
    if (pose_a && pose_b && pose_c) {
        for (size_t vi = 0; vi < 4; vi++) {
            ufbx_vec3 a = pose_a->meshes.data[0]->skinned_position.values.data[vi];
            ufbx_vec3 b = pose_b->meshes.data[0]->skinned_position.values.data[vi];
            ufbx_vec3 c = pose_c->meshes.data[0]->skinned_position.values.data[vi];
            TASK_CHECK(std::fabs((double)a.x - (double)got[0][vi].x) <= 1e-7);
            TASK_CHECK(std::fabs((double)b.x - (double)got[1][vi].x) <= 1e-7);
            TASK_CHECK(std::fabs((double)a.x - (double)c.x) <= 1e-7);
        }
    }
    if (pose_a) ufbx_free_scene(pose_a);
    if (pose_b) ufbx_free_scene(pose_b);
    if (pose_c) ufbx_free_scene(pose_c);

    // The source scene is unchanged by evaluation.
    TASK_CHECK(close_m(scene->skin_clusters.data[0]->geometry_to_world, from_ufbx(src_cluster0)));
    TASK_CHECK(close_m(scene->meshes.data[0]->instances.data[0]->geometry_to_world, from_ufbx(src_instance)));

    ufbx_free_scene(scene);
}

static void scene_additive_connection_order() {
    SceneSetup ss = make_scene_setup(UFBX_SKIN_LINK_ADDITIVE, false);
    // Declare ClusterA first (Objects order) but connect ClusterB first.
    ss.desc.cluster_connection_order = { 1, 0 };

    ufbx_error err = {};
    ufbx_scene *scene = fbxgen::load(ss.desc, NULL, &err);
    TASK_CHECK(scene != NULL);
    if (!scene) { std::fprintf(stderr, "load: %s %s\n", err.description.data, err.info); return; }

    TASK_CHECK(scene->skin_deformers.count == 1);
    ufbx_skin_deformer *skin = scene->skin_deformers.data[0];
    TASK_CHECK(skin->link_mode == UFBX_SKIN_LINK_ADDITIVE);
    TASK_CHECK(skin->clusters.count == 2);
    // `skin->clusters[]` follows the connection order (B first), while the
    // Objects declaration order is A then B.
    TASK_CHECK(std::strcmp(skin->clusters.data[0]->name.data, "ClusterB") == 0);
    TASK_CHECK(std::strcmp(skin->clusters.data[1]->name.data, "ClusterA") == 0);
    TASK_CHECK(skin->clusters.data[0]->link_order < skin->clusters.data[1]->link_order);
    TASK_CHECK(skin->clusters.data[1]->element_id < skin->clusters.data[0]->element_id);

    ufbx_anim_stack *stack = (ufbx_anim_stack*)ufbx_find_element(scene, UFBX_ELEMENT_ANIM_STACK, "Take 001");
    TASK_CHECK(stack != NULL);
    if (!stack) { ufbx_free_scene(scene); return; }
    ufbx_anim *anim = stack->anim;

    ufbx_scene *pose = eval_pose(scene, anim, 0.0);
    TASK_CHECK(pose != NULL);
    if (!pose) { ufbx_free_scene(scene); return; }
    ufbx_mesh *mesh = pose->meshes.data[0];
    TASK_CHECK(mesh->skinned_is_local == false);
    ufbx_skin_deformer *pose_skin = mesh->skin_deformers.data[0];
    TASK_CHECK(pose_skin->link_mode == UFBX_SKIN_LINK_ADDITIVE);
    ufbx_matrix &f_m = mesh->instances.data[0]->geometry_to_world;

    M3x4 g = ss.geometry_local;
    M3x4 f = instance_f_at(ss, 0);
    TASK_CHECK(close_m(f_m, f));

    ufbx_vec3 base_additive_positions[4];
    for (size_t vi = 0; vi < 4; vi++) base_additive_positions[vi] = mesh->skinned_position.values.data[vi];

    for (size_t vi = 0; vi < 3; vi++) {
        // Declared/cluster index 1 (ClusterB) is connected first, then index 0.
        std::vector<std::pair<M3x4, double> > in_connection_order;
        for (int k = 0; k < 2; k++) {
            size_t c = k == 0 ? 1 : 0;
            double w = 0.0;
            for (size_t i = 0; i < ss.cluster_indices[c].size(); i++) {
                if (ss.cluster_indices[c][i] == (int)vi) w = ss.cluster_weights[c][i];
            }
            M3x4 wm = mul_m(bone_world_at(ss, 0, c), mul_m(ss.cluster_transform[c], g));
            in_connection_order.push_back({ wm, w });
        }
        M3x4 ref = ref_additive(in_connection_order, f);
        ufbx_vec3 p = mesh->vertices.data[vi];
        ufbx_vec3 expected = apply_m(ref, p.x, p.y, p.z);
        ufbx_vec3 actual = mesh->skinned_position.values.data[vi];
        TASK_CHECK(std::fabs((double)actual.x - (double)expected.x) <= 1e-7);
        TASK_CHECK(std::fabs((double)actual.y - (double)expected.y) <= 1e-7);
        TASK_CHECK(std::fabs((double)actual.z - (double)expected.z) <= 1e-7);

        // Application in the Objects/cluster declaration order would differ.
        std::vector<std::pair<M3x4, double> > in_objects_order;
        for (int k = 0; k < 2; k++) {
            size_t c = (size_t)k;
            double w = 0.0;
            for (size_t i = 0; i < ss.cluster_indices[c].size(); i++) {
                if (ss.cluster_indices[c][i] == (int)vi) w = ss.cluster_weights[c][i];
            }
            M3x4 wm = mul_m(bone_world_at(ss, 0, c), mul_m(ss.cluster_transform[c], g));
            in_objects_order.push_back({ wm, w });
        }
        M3x4 wrong = ref_additive(in_objects_order, f);
        ufbx_vec3 wrong_pos = apply_m(wrong, p.x, p.y, p.z);
        TASK_CHECK(!close3(actual, wrong_pos.x, wrong_pos.y, wrong_pos.z));

        // Matrix entry point agrees.
        ufbx_matrix mat = ufbx_get_skin_vertex_matrix(pose_skin, vi, &f_m);
        TASK_CHECK(close_m(mat, ref));
        ufbx_vec3 via_matrix = ufbx_transform_position(&mat, p);
        TASK_CHECK(std::fabs((double)via_matrix.x - (double)actual.x) <= 1e-7);
        TASK_CHECK(std::fabs((double)via_matrix.y - (double)actual.y) <= 1e-7);
        TASK_CHECK(std::fabs((double)via_matrix.z - (double)actual.z) <= 1e-7);
    }

    // The vertex without influences gets `F * p`.
    {
        ufbx_vec3 p = mesh->vertices.data[3];
        ufbx_vec3 expected = apply_m(f, p.x, p.y, p.z);
        ufbx_vec3 actual = mesh->skinned_position.values.data[3];
        TASK_CHECK(std::fabs((double)actual.x - (double)expected.x) <= 1e-7);
        TASK_CHECK(std::fabs((double)actual.y - (double)expected.y) <= 1e-7);
        TASK_CHECK(std::fabs((double)actual.z - (double)expected.z) <= 1e-7);
    }
    ufbx_free_scene(pose);

    // The result must depend only on the connection order, not on cluster
    // names or ids: renaming both clusters and keeping the same connection
    // order leaves the result unchanged.
    {
        SceneSetup renamed = make_scene_setup(UFBX_SKIN_LINK_ADDITIVE, false);
        renamed.desc.clusters[0].name = "Zzz";
        renamed.desc.clusters[1].name = "Aaa";
        renamed.desc.cluster_connection_order = { 1, 0 };
        ufbx_error rerr = {};
        ufbx_scene *rscene = fbxgen::load(renamed.desc, NULL, &rerr);
        TASK_CHECK(rscene != NULL);
        if (rscene) {
            ufbx_anim_stack *rstack = (ufbx_anim_stack*)ufbx_find_element(rscene, UFBX_ELEMENT_ANIM_STACK, "Take 001");
            TASK_CHECK(rstack != NULL);
            if (rstack) {
                ufbx_scene *rpose = eval_pose(rscene, rstack->anim, 0.0);
                TASK_CHECK(rpose != NULL);
                if (rpose) {
                    for (size_t vi = 0; vi < 4; vi++) {
                        ufbx_vec3 a = mesh_positions_of(rscene, rpose, vi);
                        ufbx_vec3 b = base_additive_positions[vi];
                        TASK_CHECK(std::fabs((double)a.x - (double)b.x) <= 1e-7);
                        TASK_CHECK(std::fabs((double)a.y - (double)b.y) <= 1e-7);
                        TASK_CHECK(std::fabs((double)a.z - (double)b.z) <= 1e-7);
                    }
                    ufbx_free_scene(rpose);
                }
            }
            ufbx_free_scene(rscene);
        }
    }

    // A second time sample in the reverse order still matches the reference.
    {
        ufbx_scene *pose2 = eval_pose(scene, anim, 2.0);
        TASK_CHECK(pose2 != NULL);
        if (pose2) {
            ufbx_mesh *m2 = pose2->meshes.data[0];
            ufbx_skin_deformer *s2 = m2->skin_deformers.data[0];
            TASK_CHECK(s2->link_mode == UFBX_SKIN_LINK_ADDITIVE);
            ufbx_matrix &f2_m = m2->instances.data[0]->geometry_to_world;
            M3x4 f2 = instance_f_at(ss, 1);
            TASK_CHECK(close_m(f2_m, f2));
            for (size_t vi = 0; vi < 3; vi++) {
                std::vector<std::pair<M3x4, double> > inf;
                for (int k = 0; k < 2; k++) {
                    size_t c = k == 0 ? 1 : 0;
                    double w = 0.0;
                    for (size_t i = 0; i < ss.cluster_indices[c].size(); i++) {
                        if (ss.cluster_indices[c][i] == (int)vi) w = ss.cluster_weights[c][i];
                    }
                    M3x4 wm = mul_m(bone_world_at(ss, 1, c), mul_m(ss.cluster_transform[c], g));
                    inf.push_back({ wm, w });
                }
                M3x4 ref2 = ref_additive(inf, f2);
                ufbx_vec3 p = m2->vertices.data[vi];
                ufbx_vec3 expected = apply_m(ref2, p.x, p.y, p.z);
                ufbx_vec3 actual = m2->skinned_position.values.data[vi];
                TASK_CHECK(std::fabs((double)actual.x - (double)expected.x) <= 1e-7);
                TASK_CHECK(std::fabs((double)actual.y - (double)expected.y) <= 1e-7);
                TASK_CHECK(std::fabs((double)actual.z - (double)expected.z) <= 1e-7);
            }
            ufbx_free_scene(pose2);
        }
    }

    ufbx_free_scene(scene);
}

static void scene_unknown_and_mixed_modes_rejected() {
    // Unknown `Mode` string: load must fail with the cluster name in the error.
    {
        SceneSetup ss = make_scene_setup(UFBX_SKIN_LINK_TOTAL_ONE, false);
        ss.desc.clusters[1].mode = "BogusMode";
        ufbx_error err = {};
        ufbx_scene *scene = fbxgen::load(ss.desc, NULL, &err);
        TASK_CHECK(scene == NULL);
        TASK_CHECK(err.type != UFBX_ERROR_NONE);
        TASK_CHECK(err.description.data != NULL);
        TASK_CHECK(std::strstr(err.info, "ClusterB") != NULL);
        if (scene) ufbx_free_scene(scene);
    }

    // Mixed modes in a single skin: load must fail with the offending cluster name.
    {
        SceneSetup ss = make_scene_setup(UFBX_SKIN_LINK_TOTAL_ONE, false);
        ss.desc.clusters[1].mode = "Additive";
        ufbx_error err = {};
        ufbx_scene *scene = fbxgen::load(ss.desc, NULL, &err);
        TASK_CHECK(scene == NULL);
        TASK_CHECK(err.type != UFBX_ERROR_NONE);
        TASK_CHECK(err.description.data != NULL);
        TASK_CHECK(std::strstr(err.info, "ClusterB") != NULL);
        if (scene) ufbx_free_scene(scene);
    }

    // `Average` and `Normalize` both map to UFBX_SKIN_LINK_NORMALIZE.
    {
        SceneSetup ss = make_scene_setup(UFBX_SKIN_LINK_TOTAL_ONE, false);
        ss.desc.clusters[0].mode = "Average";
        ss.desc.clusters[1].mode = "Normalize";
        ufbx_error err = {};
        ufbx_scene *scene = fbxgen::load(ss.desc, NULL, &err);
        TASK_CHECK(scene != NULL);
        if (scene) {
            TASK_CHECK(scene->skin_deformers.count == 1);
            TASK_CHECK(scene->skin_deformers.data[0]->link_mode == UFBX_SKIN_LINK_NORMALIZE);
            ufbx_free_scene(scene);
        }
    }
}

static void scene_unsupported_combinations_rejected() {
    struct Variant { const char *name; bool multi_instance; bool multi_skin; bool non_linear; bool cache; bool associate_object; bool transform_associate; };

    Variant variants[] = {
        { "multi_instance", true, false, false, false, false, false },
        { "multi_skin", false, true, false, false, false, false },
        { "non_linear", false, false, true, false, false, false },
        { "geometry_cache", false, false, false, true, false, false },
        { "associate_model", false, false, false, false, true, false },
        { "transform_associate", false, false, false, false, false, true },
    };

    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        const Variant &v = variants[i];
        SceneSetup ss = make_scene_setup(UFBX_SKIN_LINK_TOTAL_ONE, false);
        ss.desc.duplicate_mesh_instance = v.multi_instance;
        ss.desc.extra_skin = v.multi_skin;
        if (v.non_linear) ss.desc.skinning_type = "DualQuaternion";
        ss.desc.external_cache = v.cache;
        ss.desc.associate_model_object = v.associate_object;
        ss.desc.clusters[0].transform_associate_model = v.transform_associate;

        ufbx_error err = {};
        ufbx_scene *scene = fbxgen::load(ss.desc, NULL, &err);
        // Must be an explicit error, never a silently empty scene.
        TASK_CHECK(scene == NULL);
        TASK_CHECK(err.type != UFBX_ERROR_NONE);
        TASK_CHECK(err.description.data != NULL && err.description.data[0] != '\0');
        if (scene) ufbx_free_scene(scene);
    }
}

// ---------------------------------------------------------------------------
// Test registration

const task_case task_cases[] = {
    { "residual", "total_one_scaled_probe", residual_total_one_scaled_probe },
    { "residual", "total_one_overshoot", residual_total_one_overshoot },
    { "residual", "zero_influence_fallback", residual_zero_influence_fallback },
    { "ordered", "additive_forward", ordered_additive_forward },
    { "ordered", "additive_swapped", ordered_additive_swapped },
    { "ordered", "additive_not_weight_sorted", ordered_additive_not_weight_sorted },
    { "scene", "total_one_moving", scene_total_one_moving },
    { "scene", "additive_connection_order", scene_additive_connection_order },
    { "scene", "unknown_and_mixed_modes_rejected", scene_unknown_and_mixed_modes_rejected },
    { "scene", "unsupported_combinations_rejected", scene_unsupported_combinations_rejected },
};
const unsigned task_case_count = (unsigned)(sizeof(task_cases) / sizeof(task_cases[0]));
