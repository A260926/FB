#include "task_support/check.h"

#include "fuzz/structured/driver.h"

#include <algorithm>
#include <unistd.h>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>

using namespace cgltf_structured;

namespace {

std::string case_dir(const char *prefix) {
    return temp_directory(prefix);
}

std::string structured_exe() {
    char cwd[PATH_MAX] = {0};
    if (::getcwd(cwd, sizeof(cwd))) {
        std::string absolute = std::string(cwd) + "/.task-build/structured";
        if (::access(absolute.c_str(), X_OK) == 0) return absolute;
    }
    if (::access(".task-build/structured", X_OK) == 0)
        return ".task-build/structured";
    return ".task-build/structured";
}

std::string read_text(const std::string &path) {
    std::string text, error;
    read_file(path, &text, &error);
    return text;
}

int count_occurrences(const std::string &text, const std::string &needle) {
    int n = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

void add_basic_buffer(Plan *p) {
    BufferModel b;
    b.id = "buf-0001";
    p->buffers.push_back(b);
}

std::string add_index_accessor(Plan *p, const std::string &prim_suffix) {
    ViewModel v;
    v.id = "view-idx-" + prim_suffix;
    v.buffer = "buf-0001";
    v.stride = 0;
    v.target = 34963;
    p->views.push_back(v);

    AccessorModel a;
    a.id = "acc-idx-" + prim_suffix;
    a.role = "INDEX";
    a.component_type = 5123;
    a.type = "SCALAR";
    a.view = v.id;
    a.offset = 0;
    a.index_values.push_back(0);
    a.index_values.push_back(1);
    a.index_values.push_back(2);
    p->accessors.push_back(a);
    return a.id;
}

void add_triangle_prim_node(Plan *p, const std::string &suffix,
                            const std::string &pos_id) {
    std::string idx_id = add_index_accessor(p, suffix);
    std::string mesh_id = "mesh-" + suffix;
    std::string prim_id = "prim-" + suffix;
    std::string node_id = "node-" + suffix;

    MeshModel m;
    m.id = mesh_id;
    p->meshes.push_back(m);

    PrimModel pr;
    pr.id = prim_id;
    pr.mesh = mesh_id;
    pr.indices = idx_id;
    pr.position = pos_id;
    pr.mode = 4;
    pr.triangles.push_back(Triangle("tri-" + suffix, "v-" + pos_id + "-0",
                                    "v-" + pos_id + "-1", "v-" + pos_id + "-2"));
    p->prims.push_back(pr);

    NodeModel n;
    n.id = node_id;
    n.mesh = mesh_id;
    p->nodes.push_back(n);
    p->scene.roots.push_back(node_id);
}

Plan make_layout_combo(bool shared, bool interleaved) {
    Plan p;
    p.seed = 17;
    p.scene.id = "scene-0001";
    add_basic_buffer(&p);

    const std::string view_pos = "view-pos";
    ViewModel v;
    v.id = view_pos;
    v.buffer = "buf-0001";
    v.stride = interleaved ? 24 : 12;
    v.target = 34962;
    p.views.push_back(v);

    auto add_vertices = [](AccessorModel *a) {
        a->vertices.push_back(Vertex("v-" + a->id + "-0", 0.0f, 0.0f, 0.0f));
        a->vertices.push_back(Vertex("v-" + a->id + "-1", 1.0f, 0.0f, 0.0f));
        a->vertices.push_back(Vertex("v-" + a->id + "-2", 0.0f, 1.0f, 0.0f));
    };

    if (shared) {
        AccessorModel a;
        a.id = "acc-pos-shared";
        a.role = "POSITION";
        a.component_type = 5126;
        a.type = "VEC3";
        a.view = view_pos;
        a.offset = interleaved ? 12 : 0;
        a.explicit_stride = interleaved ? 24 : 12;
        add_vertices(&a);
        p.accessors.push_back(a);

        if (interleaved) {
            AccessorModel c;
            c.id = "acc-pos-companion";
            c.role = "POSITION";
            c.component_type = 5126;
            c.type = "VEC3";
            c.view = view_pos;
            c.offset = 0;
            c.explicit_stride = 24;
            add_vertices(&c);
            p.accessors.push_back(c);
        }
        add_triangle_prim_node(&p, "a", "acc-pos-shared");
        add_triangle_prim_node(&p, "b", "acc-pos-shared");
    } else {
        for (int which = 0; which < 2; ++which) {
            std::string suffix = which == 0 ? "a" : "b";
            std::string this_view = view_pos;
            std::size_t offset = 0;
            if (interleaved) {
                offset = which == 0 ? 0 : 12;
            } else {
                this_view = "view-pos-" + suffix;
                ViewModel vv = v;
                vv.id = this_view;
                p.views.push_back(vv);
            }
            AccessorModel a;
            a.id = "acc-pos-" + suffix;
            a.role = "POSITION";
            a.component_type = 5126;
            a.type = "VEC3";
            a.view = this_view;
            a.offset = offset;
            a.explicit_stride = interleaved ? 24 : 12;
            add_vertices(&a);
            p.accessors.push_back(a);
            add_triangle_prim_node(&p, suffix, a.id);
        }
    }
    sync_index_values(&p);
    rebuild_layout(&p);
    return p;
}

Plan make_resume_fixture() {
    Plan p;
    p.seed = 77;
    p.scene.id = "scene-0001";
    add_basic_buffer(&p);

    ViewModel iv;
    iv.id = "view-inter";
    iv.buffer = "buf-0001";
    iv.stride = 24;
    iv.target = 34962;
    p.views.push_back(iv);

    AccessorModel companion;
    companion.id = "acc-companion";
    companion.role = "POSITION";
    companion.component_type = 5126;
    companion.type = "VEC3";
    companion.view = iv.id;
    companion.offset = 0;
    companion.explicit_stride = 24;
    companion.vertices.push_back(Vertex("cv0", 9, 0, 0));
    companion.vertices.push_back(Vertex("cv1", 9, 1, 0));
    companion.vertices.push_back(Vertex("cv2", 9, 0, 1));
    p.accessors.push_back(companion);

    AccessorModel shared;
    shared.id = "acc-shared";
    shared.role = "POSITION";
    shared.component_type = 5126;
    shared.type = "VEC3";
    shared.view = iv.id;
    shared.offset = 12;
    shared.explicit_stride = 24;
    shared.vertices.push_back(Vertex("sv0", 0, 0, 0));
    shared.vertices.push_back(Vertex("sv1", 1, 0, 0));
    shared.vertices.push_back(Vertex("sv2", 0, 1, 0));
    p.accessors.push_back(shared);

    for (int i = 1; i <= 3; ++i) {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "%04d", i);
        std::string s(suffix);
        std::string pos = i < 3 ? "acc-shared" : "acc-extra";
        std::string idx = "acc-idx-" + s;

        ViewModel ivx;
        ivx.id = "view-idx-" + s;
        ivx.buffer = "buf-0001";
        ivx.target = 34963;
        p.views.push_back(ivx);
        AccessorModel ia;
        ia.id = idx;
        ia.role = "INDEX";
        ia.component_type = 5123;
        ia.type = "SCALAR";
        ia.view = ivx.id;
        p.accessors.push_back(ia);

        MeshModel m;
        m.id = "mesh-" + s;
        p.meshes.push_back(m);
        PrimModel pr;
        pr.id = "prim-" + s;
        pr.mesh = m.id;
        pr.indices = idx;
        pr.position = pos;
        pr.triangles.push_back(Triangle("tri-" + s, "sv0", "sv1", "sv2"));
        p.prims.push_back(pr);

        NodeModel n;
        n.id = "node-" + s;
        n.mesh = m.id;
        p.nodes.push_back(n);
        p.scene.roots.push_back(n.id);
    }

    ViewModel ev;
    ev.id = "view-extra";
    ev.buffer = "buf-0001";
    ev.stride = 12;
    ev.target = 34962;
    p.views.push_back(ev);
    AccessorModel extra;
    extra.id = "acc-extra";
    extra.role = "POSITION";
    extra.component_type = 5126;
    extra.type = "VEC3";
    extra.view = ev.id;
    extra.offset = 0;
    extra.explicit_stride = 12;
    extra.vertices.push_back(Vertex("ev0", 3, 0, 0));
    extra.vertices.push_back(Vertex("ev1", 4, 0, 0));
    extra.vertices.push_back(Vertex("ev2", 3, 1, 0));
    p.accessors.push_back(extra);
    p.prims[2].triangles[0].v[0] = "ev0";
    p.prims[2].triangles[0].v[1] = "ev1";
    p.prims[2].triangles[0].v[2] = "ev2";

    sync_index_values(&p);
    rebuild_layout(&p);
    return p;
}

} // namespace

static int graph_generation_contract(void) {
    Plan p = generate_plan(42);
    TASK_CHECK(p.nodes.size() <= 64);
    TASK_CHECK(p.accessors.size() <= 64);
    std::size_t buffer_bytes = 0;
    for (std::size_t i = 0; i < p.buffers.size(); ++i)
        buffer_bytes += p.buffers[i].bytes.size();
    TASK_CHECK(buffer_bytes <= 64u * 1024u);

    Witness w = find_witness(p);
    TASK_CHECK(w.found);
    TASK_CHECK(w.offset != 0);
    TASK_CHECK(w.stride > 12);
    TASK_CHECK(p.meshes.size() >= 2);
    TASK_CHECK(p.nodes.size() >= 2);

    bool nonshared_position = false;
    for (std::size_t ai = 0; ai < p.accessors.size(); ++ai) {
        const AccessorModel &a = p.accessors[ai];
        TASK_CHECK(a.role != "POSITION" || a.vertices.size() > 0);
        for (std::size_t vi = 0; vi < a.vertices.size(); ++vi) {
            TASK_CHECK(std::isfinite(a.vertices[vi].x));
            TASK_CHECK(std::isfinite(a.vertices[vi].y));
            TASK_CHECK(std::isfinite(a.vertices[vi].z));
        }
        if (a.role != "POSITION") continue;
        std::size_t users = 0;
        for (std::size_t pi = 0; pi < p.prims.size(); ++pi)
            if (p.prims[pi].position == a.id) ++users;
        if (users == 1) nonshared_position = true;
    }
    TASK_CHECK(nonshared_position);

    std::vector<std::string> alive = scene_closure(p);
    Metrics m = compute_metrics(p, 0);
    TASK_CHECK(alive.size() < m.objects + 1);

    std::string dir = case_dir("st-graph");
    std::string error;
    TASK_CHECK(write_case_files(&p, dir, NULL, &error));
    VerifyResult vr = verify_directory_real(dir);
    if (!vr.ok)
        std::fprintf(stderr, "graph verify %s %s: %s\n", vr.first_id.c_str(),
                     vr.element.c_str(), vr.message.c_str());
    TASK_CHECK(vr.ok);

    TASK_CHECK(plan_to_string(generate_plan(7)) == plan_to_string(generate_plan(7)));
    return 0;
}

static int graph_plan_identity_roundtrip(void) {
    Plan p = generate_plan(8);
    std::string text = plan_to_string(p);
    Plan q;
    std::string error;
    TASK_CHECK(plan_from_string(text, &q, &error));
    TASK_CHECK(plan_to_string(q) == text);

    std::vector<std::string> ids;
    for (std::size_t i = 0; i < p.nodes.size(); ++i) ids.push_back(p.nodes[i].id);
    for (std::size_t i = 0; i < p.meshes.size(); ++i) ids.push_back(p.meshes[i].id);
    for (std::size_t i = 0; i < p.prims.size(); ++i) ids.push_back(p.prims[i].id);
    for (std::size_t i = 0; i < p.accessors.size(); ++i) ids.push_back(p.accessors[i].id);
    std::sort(ids.begin(), ids.end());
    TASK_CHECK(std::unique(ids.begin(), ids.end()) == ids.end());
    return 0;
}

static int layout_shared_interleaved_combinations(void) {
    for (int shared = 0; shared <= 1; ++shared) {
        for (int interleaved = 0; interleaved <= 1; ++interleaved) {
            Plan p = make_layout_combo(shared != 0, interleaved != 0);
            if (shared && interleaved) {
                Witness w = find_witness(p);
                TASK_CHECK(w.found);
                TASK_CHECK(w.offset == 12);
                TASK_CHECK(w.stride == 24);
            }
            std::string dir = case_dir("st-layout-combo");
            std::string error;
            TASK_CHECK(write_case_files(&p, dir, NULL, &error));
            VerifyResult vr = verify_directory_real(dir);
            if (!vr.ok)
                std::fprintf(stderr, "layout combo shared=%d inter=%d: %s %s %s\n",
                             shared, interleaved, vr.first_id.c_str(),
                             vr.element.c_str(), vr.message.c_str());
            TASK_CHECK(vr.ok);
        }
    }
    return 0;
}

static int layout_compact_binary_rearrangement(void) {
    Plan p;
    p.seed = 9;
    p.scene.id = "scene-0001";
    add_basic_buffer(&p);

    ViewModel va;
    va.id = "view-a"; va.buffer = "buf-0001"; va.stride = 12; va.target = 34962;
    ViewModel vb = va; vb.id = "view-b";
    p.views.push_back(va);
    p.views.push_back(vb);

    AccessorModel aa;
    aa.id = "acc-pos-a"; aa.role = "POSITION"; aa.type = "VEC3";
    aa.component_type = 5126; aa.view = va.id; aa.explicit_stride = 12;
    aa.vertices.push_back(Vertex("va0", 9, 8, 7));
    aa.vertices.push_back(Vertex("va1", 9, 8, 8));
    aa.vertices.push_back(Vertex("va2", 9, 9, 7));
    p.accessors.push_back(aa);

    AccessorModel ab = aa;
    ab.id = "acc-pos-b"; ab.view = vb.id;
    ab.vertices.clear();
    ab.vertices.push_back(Vertex("vb0", -7.5f, 2.25f, 1.0f));
    ab.vertices.push_back(Vertex("vb1", -6.5f, 2.25f, 1.0f));
    ab.vertices.push_back(Vertex("vb2", -7.5f, 3.25f, 1.0f));
    p.accessors.push_back(ab);

    add_triangle_prim_node(&p, "a", "acc-pos-a");
    add_triangle_prim_node(&p, "b", "acc-pos-b");
    sync_index_values(&p);
    rebuild_layout(&p);
    TASK_CHECK(p.views[0].offset == 0);
    TASK_CHECK(p.views[1].offset == 36);

    TASK_CHECK(delete_prim(&p, "prim-a"));
    std::string reason;
    TASK_CHECK(delete_resource_if_unreferenced(&p, "acc-pos-a", &reason));
    TASK_CHECK(delete_resource_if_unreferenced(&p, "view-a", &reason));
    rebuild_layout(&p);

    TASK_CHECK(find_accessor(p, "acc-pos-a") < 0);
    TASK_CHECK(find_view(p, "view-a") < 0);
    TASK_CHECK(find_accessor(p, "acc-pos-b") == 0);
    TASK_CHECK(p.views.size() >= 1);
    int vb_index = find_view(p, "view-b");
    TASK_CHECK(vb_index >= 0);
    TASK_CHECK(p.views[vb_index].offset == 0);
    int buf_i = find_buffer(p, "buf-0001");
    TASK_CHECK(buf_i >= 0);
    TASK_CHECK(p.buffers[buf_i].bytes.size() >= 4);
    unsigned char b0 = p.buffers[buf_i].bytes[0];
    unsigned char b1 = p.buffers[buf_i].bytes[1];
    unsigned char b2 = p.buffers[buf_i].bytes[2];
    unsigned char b3 = p.buffers[buf_i].bytes[3];
    std::uint32_t bits = static_cast<std::uint32_t>(b0) |
                         (static_cast<std::uint32_t>(b1) << 8) |
                         (static_cast<std::uint32_t>(b2) << 16) |
                         (static_cast<std::uint32_t>(b3) << 24);
    float first_x;
    std::memcpy(&first_x, &bits, sizeof(first_x));
    TASK_CHECK(first_x == -7.5f);

    std::string dir = case_dir("st-layout-compact");
    std::string error;
    TASK_CHECK(write_case_files(&p, dir, NULL, &error));
    VerifyResult vr = verify_directory_real(dir);
    if (!vr.ok)
        std::fprintf(stderr, "compact verify %s %s: %s\n", vr.first_id.c_str(),
                     vr.element.c_str(), vr.message.c_str());
    TASK_CHECK(vr.ok);
    return 0;
}

static int reduce_full_commits_are_read_by_production(void) {
    std::string in = case_dir("st-reduce-in");
    std::string out = case_dir("st-reduce-out");
    Plan original = generate_plan(123);
    std::string error;
    TASK_CHECK(generate_to_dir(123, in, &error));

    int real_calls = 0;
    int real_ok = 0;
    ReduceOptions opt;
    opt.in_dir = in;
    opt.out_dir = out;
    opt.keep = KEEP_SHARED_STRIDED;
    opt.budget = 200;
    opt.resume = false;
    opt.verifier = [&](const std::string &stage) {
        ++real_calls;
        VerifyResult r = verify_directory_real(stage);
        if (r.ok) ++real_ok;
        return r;
    };

    ReduceOutcome ro = reduce_run(opt);
    TASK_CHECK(ro.return_code == 0);
    TASK_CHECK(ro.done);
    TASK_CHECK(ro.accepted > 0);
    TASK_CHECK(real_calls >= static_cast<int>(ro.accepted));
    TASK_CHECK(real_ok >= static_cast<int>(ro.accepted));

    VerifyResult final_vr = verify_directory_real(out);
    TASK_CHECK(final_vr.ok);

    Plan reduced;
    TASK_CHECK(load_plan(out + "/case.plan", &reduced, &error));
    Witness rw = find_witness(reduced);
    TASK_CHECK(rw.found && rw.offset != 0);

    std::string in_gltf = read_text(in + "/case.gltf");
    std::string out_gltf = read_text(out + "/case.gltf");
    Metrics before = compute_metrics(original, in_gltf.size());
    Metrics after = compute_metrics(reduced, out_gltf.size());
    TASK_CHECK(after.lex_less(before));

    /* Survivors must retain stable IDs and exact semantic values. */
    for (std::size_t ai = 0; ai < reduced.accessors.size(); ++ai) {
        const AccessorModel &ra = reduced.accessors[ai];
        int oi = find_accessor(original, ra.id);
        TASK_CHECK(oi >= 0);
        const AccessorModel &oa = original.accessors[oi];
        if (ra.role == "POSITION") {
            for (std::size_t vi = 0; vi < ra.vertices.size(); ++vi) {
                const Vertex *ov = NULL;
                for (std::size_t j = 0; j < oa.vertices.size(); ++j)
                    if (oa.vertices[j].id == ra.vertices[vi].id)
                        ov = &oa.vertices[j];
                TASK_CHECK(ov != NULL);
                TASK_CHECK(ov->x == ra.vertices[vi].x);
                TASK_CHECK(ov->y == ra.vertices[vi].y);
                TASK_CHECK(ov->z == ra.vertices[vi].z);
            }
        }
    }
    for (std::size_t pi = 0; pi < reduced.prims.size(); ++pi) {
        int opi = find_prim(original, reduced.prims[pi].id);
        TASK_CHECK(opi >= 0);
        const PrimModel &op = original.prims[opi];
        TASK_CHECK(reduced.prims[pi].triangles.size() <= op.triangles.size());
        for (std::size_t ti = 0; ti < reduced.prims[pi].triangles.size(); ++ti) {
            const Triangle &rt = reduced.prims[pi].triangles[ti];
            const Triangle *ot = NULL;
            for (std::size_t j = 0; j < op.triangles.size(); ++j)
                if (op.triangles[j].id == rt.id) ot = &op.triangles[j];
            TASK_CHECK(ot != NULL);
            TASK_CHECK(rt.v[0] == ot->v[0]);
            TASK_CHECK(rt.v[1] == ot->v[1]);
            TASK_CHECK(rt.v[2] == ot->v[2]);
        }
    }

    std::string log = read_text(out + "/reduction.log");
    TASK_CHECK(log.find("CANDIDATE_ORDER") != std::string::npos);
    TASK_CHECK(log.find("ACCEPT index=") != std::string::npos);
    TASK_CHECK(log.find("production_read=OK") != std::string::npos);
    return 0;
}

static int resume_consecutive_rejects_and_budget_edge(void) {
    Plan fixture = make_resume_fixture();
    std::vector<Candidate> candidates = build_candidates(fixture);
    TASK_CHECK(candidates.size() > 3);
    TASK_CHECK(candidates[0].phase == PHASE_NODE);
    TASK_CHECK(candidates[0].target_id == "node-0001");
    TASK_CHECK(candidates[1].target_id == "node-0002");
    TASK_CHECK(candidates[2].target_id == "node-0003");

    std::string in = case_dir("st-resume-fixture");
    std::string out_a = case_dir("st-resume-a");
    std::string out_b = case_dir("st-resume-b");
    std::string error;
    TASK_CHECK(write_case_files(&fixture, in, NULL, &error));

    ReduceOptions first;
    first.in_dir = in; first.out_dir = out_a; first.keep = "shared-strided";
    first.budget = 2; first.resume = false;
    ReduceOutcome ro1 = reduce_run(first);
    TASK_CHECK(ro1.return_code == 0);
    TASK_CHECK(ro1.observed == 2);
    TASK_CHECK(ro1.accepted == 0);
    TASK_CHECK(ro1.next_index == 2);
    TASK_CHECK(read_text(out_a + "/case.plan") == read_text(in + "/case.plan"));
    TASK_CHECK(read_text(out_a + "/case.gltf") == read_text(in + "/case.gltf"));

    std::string log_before = read_text(out_a + "/reduction.log");
    TASK_CHECK(log_before.find("REJECT index=0") != std::string::npos);
    TASK_CHECK(log_before.find("REJECT index=1") != std::string::npos);

    ReduceOptions resume;
    resume.in_dir = in; resume.out_dir = out_a; resume.keep = "shared-strided";
    resume.budget = 1; resume.resume = true;
    ReduceOutcome ro2 = reduce_run(resume);
    TASK_CHECK(ro2.return_code == 0);
    TASK_CHECK(ro2.observed == 1);
    TASK_CHECK(ro2.accepted == 1);
    TASK_CHECK(ro2.next_index == 3);

    ReduceOptions exact;
    exact.in_dir = in; exact.out_dir = out_b; exact.keep = "shared-strided";
    exact.budget = 3; exact.resume = false;
    ReduceOutcome ro3 = reduce_run(exact);
    TASK_CHECK(ro3.return_code == 0);
    TASK_CHECK(ro3.observed == 3);
    TASK_CHECK(ro3.accepted == 1);
    TASK_CHECK(ro3.next_index == 3);

    TASK_CHECK(read_text(out_a + "/case.plan") == read_text(out_b + "/case.plan"));
    TASK_CHECK(read_text(out_a + "/case.gltf") == read_text(out_b + "/case.gltf"));

    std::string log_after = read_text(out_a + "/reduction.log");
    TASK_CHECK(count_occurrences(log_after, "OBSERVE index=0 ") == 1);
    TASK_CHECK(log_after.find("RESUME start next_index=2") != std::string::npos);

    std::string other = case_dir("st-resume-other");
    TASK_CHECK(generate_to_dir(777, other, &error));
    ReduceOptions mismatch;
    mismatch.in_dir = other; mismatch.out_dir = out_a;
    mismatch.keep = "shared-strided"; mismatch.budget = 1; mismatch.resume = true;
    ReduceOutcome bad = reduce_run(mismatch);
    TASK_CHECK(bad.return_code == 2);

    ReduceOptions keep_bad;
    keep_bad.in_dir = in; keep_bad.out_dir = out_a;
    keep_bad.keep = "some-other-keep"; keep_bad.budget = 1; keep_bad.resume = true;
    ReduceOutcome bad_keep = reduce_run(keep_bad);
    TASK_CHECK(bad_keep.return_code == 2);
    return 0;
}

static int resume_segmented_budgets_match_total(void) {
    std::string in = case_dir("st-seed-in");
    std::string seg = case_dir("st-seg-out");
    std::string total = case_dir("st-total-out");
    std::string error;
    TASK_CHECK(generate_to_dir(31337, in, &error));

    ReduceOptions r1;
    r1.in_dir = in; r1.out_dir = seg; r1.keep = "shared-strided"; r1.budget = 4;
    ReduceOutcome o1 = reduce_run(r1);
    TASK_CHECK(o1.return_code == 0);
    TASK_CHECK(!o1.done);
    TASK_CHECK(o1.next_index == 4);

    ReduceOptions r2;
    r2.in_dir = in; r2.out_dir = seg; r2.keep = "shared-strided";
    r2.budget = 7; r2.resume = true;
    ReduceOutcome o2 = reduce_run(r2);
    TASK_CHECK(o2.return_code == 0);
    TASK_CHECK(o2.next_index == 11);

    ReduceOptions r3;
    r3.in_dir = in; r3.out_dir = seg; r3.keep = "shared-strided";
    r3.budget = 200; r3.resume = true;
    ReduceOutcome o3 = reduce_run(r3);
    TASK_CHECK(o3.return_code == 0);
    TASK_CHECK(o3.done);

    ReduceOptions rt;
    rt.in_dir = in; rt.out_dir = total; rt.keep = "shared-strided"; rt.budget = 211;
    ReduceOutcome ot = reduce_run(rt);
    TASK_CHECK(ot.return_code == 0);
    TASK_CHECK(ot.done);
    TASK_CHECK(o3.next_index == ot.next_index);

    TASK_CHECK(read_text(seg + "/case.plan") == read_text(total + "/case.plan"));
    TASK_CHECK(read_text(seg + "/case.gltf") == read_text(total + "/case.gltf"));
    TASK_CHECK(verify_directory_real(seg).ok);
    TASK_CHECK(verify_directory_real(total).ok);
    return 0;
}

static int process_same_seed_independent_replay(void) {
    std::string exe = structured_exe();
    TASK_CHECK(::access(exe.c_str(), X_OK) == 0);
    std::string d1 = case_dir("st-replay-a");
    std::string d2 = case_dir("st-replay-b");
    ProcResult r1 = run_process(std::vector<std::string>{exe, "generate", "18446744073709551615", d1});
    ProcResult r2 = run_process(std::vector<std::string>{exe, "generate", "18446744073709551615", d2});
    TASK_CHECK(r1.clean_exit_zero());
    TASK_CHECK(r2.clean_exit_zero());
    TASK_CHECK(read_text(d1 + "/case.plan") == read_text(d2 + "/case.plan"));
    TASK_CHECK(read_text(d1 + "/case.gltf") == read_text(d2 + "/case.gltf"));
    return 0;
}

static int process_worker_empty_and_status_handling(void) {
    std::string exe = structured_exe();
    TASK_CHECK(::access(exe.c_str(), X_OK) == 0);
    std::string dir = case_dir("st-worker-good");
    std::string empty_dir = case_dir("st-worker-empty");
    std::string error;
    TASK_CHECK(generate_to_dir(4242, dir, &error));

    Plan plan;
    TASK_CHECK(load_plan(dir + "/case.plan", &plan, &error));

    ProcResult not_spawned;
    VerifyResult v0 = evaluate_worker_result(plan, not_spawned);
    TASK_CHECK(!v0.ok);
    TASK_CHECK(v0.first_id == "process");

    ProcResult empty_ok;
    empty_ok.spawn_ok = true;
    empty_ok.exit_code = 0;
    VerifyResult v1 = evaluate_worker_result(plan, empty_ok);
    TASK_CHECK(!v1.ok);
    TASK_CHECK(v1.first_id == "worker");

    ProcResult good = run_worker_process(exe, dir);
    TASK_CHECK(good.clean_exit_zero());
    VerifyResult good_v = evaluate_worker_result(plan, good);
    if (!good_v.ok)
        std::fprintf(stderr, "good worker eval %s %s: %s\n", good_v.first_id.c_str(),
                     good_v.element.c_str(), good_v.message.c_str());
    TASK_CHECK(good_v.ok);

    ProcResult bad_exit = good;
    bad_exit.exit_code = 9;
    bad_exit.signaled = false;
    VerifyResult v2 = evaluate_worker_result(plan, bad_exit);
    TASK_CHECK(!v2.ok);
    TASK_CHECK(v2.first_id == "process");

    ProcResult truncated = good;
    truncated.output = "CGLTF_STRUCTURED_WORKER 1\nGLTF version=2.0 scene=0\n";
    VerifyResult v3 = evaluate_worker_result(plan, truncated);
    TASK_CHECK(!v3.ok);

    ProcResult missing = run_worker_process(exe, empty_dir);
    TASK_CHECK(!missing.clean_exit_zero());
    VerifyResult v4 = evaluate_worker_result(plan, missing);
    TASK_CHECK(!v4.ok);

    ProcResult check = run_process(std::vector<std::string>{exe, "check", dir});
    TASK_CHECK(check.clean_exit_zero());
    TASK_CHECK(check.output.find("VALID") != std::string::npos);
    return 0;
}

extern const struct task_case task_cases[] = {
    {"graph", "generation-contract", graph_generation_contract},
    {"graph", "plan-identity-roundtrip", graph_plan_identity_roundtrip},
    {"layout", "shared-nonshared-interleaved-combinations", layout_shared_interleaved_combinations},
    {"layout", "compact-array-and-binary-rearrangement", layout_compact_binary_rearrangement},
    {"reduce", "full-reduction-commits-use-production-reads", reduce_full_commits_are_read_by_production},
    {"resume", "consecutive-rejects-budget-edge-and-cursor", resume_consecutive_rejects_and_budget_edge},
    {"resume", "segmented-budgets-match-total", resume_segmented_budgets_match_total},
    {"process", "same-seed-independent-process-replay", process_same_seed_independent_replay},
    {"process", "worker-empty-truncated-and-nonzero-exit", process_worker_empty_and_status_handling},
};

extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
