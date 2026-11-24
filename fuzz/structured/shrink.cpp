#include "fuzz/structured/driver.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace cgltf_structured {

namespace {

std::string fnv1a(const std::string &s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (std::size_t i = 0; i < s.size(); ++i) {
        h ^= static_cast<unsigned char>(s[i]);
        h *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << h;
    return out.str();
}

std::string fingerprint_file(const std::string &path) {
    std::string text, error;
    if (!read_file(path, &text, &error)) return "missing";
    std::ostringstream out;
    out << text.size() << ":" << fnv1a(text);
    return out.str();
}

bool copy_file_bytes(const std::string &from, const std::string &to,
                     std::string *error) {
    std::string data;
    if (!read_file(from, &data, error)) return false;
    return write_file_atomic(to, data, error);
}

std::vector<std::string> sorted_ids(const std::vector<std::string> &ids) {
    std::vector<std::string> out = ids;
    std::sort(out.begin(), out.end());
    return out;
}

void append_log(const std::string &path, const std::string &line) {
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::app);
    if (out) out << line << "\n";
}

std::string phase_name(CandidatePhase phase) {
    switch (phase) {
    case PHASE_OUTSIDE: return "outside";
    case PHASE_NODE: return "node-subtree";
    case PHASE_PRIMITIVE: return "mesh-primitive";
    case PHASE_RESOURCE: return "unreferenced-resource";
    case PHASE_TRIANGLE: return "triangle";
    }
    return "unknown";
}

struct RecoveryState {
    std::string source_plan_fp;
    std::string source_gltf_fp;
    std::string keep;
    std::size_t next_index;
    std::size_t total;
    std::uint64_t observed_total;
    std::uint64_t accepted_total;
    bool done;
    RecoveryState()
        : next_index(0), total(0), observed_total(0), accepted_total(0),
          done(false) {}
};

std::string state_to_string(const RecoveryState &st) {
    std::ostringstream out;
    out << "CGLTF-STRUCTURED-REDUCTION-STATE 1\n";
    out << "source_plan_fp=" << st.source_plan_fp << "\n";
    out << "source_gltf_fp=" << st.source_gltf_fp << "\n";
    out << "keep=" << st.keep << "\n";
    out << "next_index=" << st.next_index << "\n";
    out << "total=" << st.total << "\n";
    out << "observed_total=" << st.observed_total << "\n";
    out << "accepted_total=" << st.accepted_total << "\n";
    out << "done=" << (st.done ? 1 : 0) << "\n";
    return out.str();
}

bool state_from_string(const std::string &text, RecoveryState *st) {
    if (text.find("CGLTF-STRUCTURED-REDUCTION-STATE 1") == std::string::npos)
        return false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "source_plan_fp") st->source_plan_fp = val;
        else if (key == "source_gltf_fp") st->source_gltf_fp = val;
        else if (key == "keep") st->keep = val;
        else if (key == "next_index") st->next_index = std::strtoull(val.c_str(), NULL, 10);
        else if (key == "total") st->total = std::strtoull(val.c_str(), NULL, 10);
        else if (key == "observed_total") st->observed_total = std::strtoull(val.c_str(), NULL, 10);
        else if (key == "accepted_total") st->accepted_total = std::strtoull(val.c_str(), NULL, 10);
        else if (key == "done") st->done = val == "1";
    }
    return true;
}

std::string candidate_target_kind(const Plan &p, const std::string &id) {
    if (find_node(p, id) >= 0) return "node";
    if (find_mesh(p, id) >= 0) return "mesh";
    if (find_prim(p, id) >= 0) return "prim";
    if (find_accessor(p, id) >= 0) return "accessor";
    if (find_view(p, id) >= 0) return "view";
    if (find_buffer(p, id) >= 0) return "buffer";
    return "unknown";
}

} // namespace

std::vector<Candidate> build_candidates(const Plan &source) {
    std::vector<Candidate> out;
    std::size_t index = 0;
    auto add = [&](CandidatePhase phase, const std::string &id,
                   const std::string &kind, const std::string &target_id,
                   const std::string &prim_id = "",
                   const std::string &tri_id = "") {
        Candidate c;
        c.index = index++;
        c.phase = phase;
        c.phase_name = phase_name(phase);
        c.id = id;
        c.target_kind = kind;
        c.target_id = target_id;
        c.prim_id = prim_id;
        c.triangle_id = tri_id;
        out.push_back(c);
    };

    std::vector<std::string> alive = scene_closure(source);

    /* Phase 1: stable-ID order for objects outside the scene closure.
     * Nodes and meshes are graph objects; resources have their own phase. */
    std::vector<std::string> outside;
    for (std::size_t i = 0; i < source.nodes.size(); ++i)
        if (std::find(alive.begin(), alive.end(), source.nodes[i].id) == alive.end())
            outside.push_back(source.nodes[i].id);
    for (std::size_t i = 0; i < source.meshes.size(); ++i)
        if (std::find(alive.begin(), alive.end(), source.meshes[i].id) == alive.end())
            outside.push_back(source.meshes[i].id);
    for (std::size_t i = 0; i < outside.size(); ++i) {
        std::string kind = candidate_target_kind(source, outside[i]);
        add(PHASE_OUTSIDE, "outside:" + outside[i], kind, outside[i]);
    }
    /* Sort only the IDs just added while retaining phase numbering. */
    std::sort(out.begin(), out.end(), [](const Candidate &a, const Candidate &b) {
        return a.target_id < b.target_id;
    });
    for (std::size_t i = 0; i < out.size(); ++i) out[i].index = i;
    index = out.size();

    /* Phase 2: every node subtree. */
    std::vector<std::string> nodes;
    for (std::size_t i = 0; i < source.nodes.size(); ++i) nodes.push_back(source.nodes[i].id);
    nodes = sorted_ids(nodes);
    for (std::size_t i = 0; i < nodes.size(); ++i)
        add(PHASE_NODE, "node-subtree:" + nodes[i], "node", nodes[i]);

    /* Phase 3: every mesh primitive. */
    std::vector<std::string> prims;
    for (std::size_t i = 0; i < source.prims.size(); ++i) prims.push_back(source.prims[i].id);
    prims = sorted_ids(prims);
    for (std::size_t i = 0; i < prims.size(); ++i)
        add(PHASE_PRIMITIVE, "mesh-primitive:" + prims[i], "prim", prims[i]);

    /* Phase 4: resources in dependency-safe class order, stable IDs inside
     * each class.  Referenced resources are observed and rejected. */
    std::vector<std::string> resources[3];
    for (std::size_t i = 0; i < source.accessors.size(); ++i) resources[0].push_back(source.accessors[i].id);
    for (std::size_t i = 0; i < source.views.size(); ++i) resources[1].push_back(source.views[i].id);
    for (std::size_t i = 0; i < source.buffers.size(); ++i) resources[2].push_back(source.buffers[i].id);
    for (int cls = 0; cls < 3; ++cls) {
        resources[cls] = sorted_ids(resources[cls]);
        for (std::size_t i = 0; i < resources[cls].size(); ++i) {
            std::string kind = cls == 0 ? "accessor" : (cls == 1 ? "view" : "buffer");
            add(PHASE_RESOURCE, "resource:" + resources[cls][i], kind, resources[cls][i]);
        }
    }

    /* Phase 5: triangles in their original per-primitive order. Primitives
     * are visited in stable-ID order. */
    std::vector<std::string> prim_order;
    for (std::size_t i = 0; i < source.prims.size(); ++i) prim_order.push_back(source.prims[i].id);
    std::sort(prim_order.begin(), prim_order.end());
    for (std::size_t pi = 0; pi < prim_order.size(); ++pi) {
        int pidx = find_prim(source, prim_order[pi]);
        if (pidx < 0) continue;
        const PrimModel &pr = source.prims[pidx];
        for (std::size_t ti = 0; ti < pr.triangles.size(); ++ti) {
            add(PHASE_TRIANGLE,
                "triangle:" + pr.id + ":" + pr.triangles[ti].id,
                "triangle", pr.triangles[ti].id, pr.id, pr.triangles[ti].id);
        }
    }

    for (std::size_t i = 0; i < out.size(); ++i) out[i].index = i;
    return out;
}

bool delete_node_subtree(Plan *p, const std::string &id) {
    if (!p || find_node(*p, id) < 0) return false;
    std::vector<std::string> removed;
    /* collect_node_subtree is local to model.cpp's anonymous namespace, so
     * duplicate the small deterministic traversal here. */
    std::vector<std::string> stack;
    stack.push_back(id);
    while (!stack.empty()) {
        std::string cur = stack.back();
        stack.pop_back();
        if (std::find(removed.begin(), removed.end(), cur) != removed.end()) continue;
        int ni = find_node(*p, cur);
        if (ni < 0) continue;
        removed.push_back(cur);
        for (std::size_t i = 0; i < p->nodes[ni].children.size(); ++i)
            stack.push_back(p->nodes[ni].children[i]);
    }

    for (std::size_t i = 0; i < p->nodes.size();) {
        if (std::find(removed.begin(), removed.end(), p->nodes[i].id) != removed.end())
            p->nodes.erase(p->nodes.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    p->scene.roots.erase(
        std::remove_if(p->scene.roots.begin(), p->scene.roots.end(),
                       [&](const std::string &r) {
                           return std::find(removed.begin(), removed.end(), r) != removed.end();
                       }),
        p->scene.roots.end());
    for (std::size_t i = 0; i < p->nodes.size(); ++i) {
        p->nodes[i].children.erase(
            std::remove_if(p->nodes[i].children.begin(), p->nodes[i].children.end(),
                           [&](const std::string &r) {
                               return std::find(removed.begin(), removed.end(), r) != removed.end();
                           }),
            p->nodes[i].children.end());
    }
    return true;
}

bool delete_prim(Plan *p, const std::string &id) {
    if (!p || find_prim(*p, id) < 0) return false;
    std::string mesh_id;
    for (std::size_t i = 0; i < p->prims.size(); ++i) {
        if (p->prims[i].id != id) continue;
        mesh_id = p->prims[i].mesh;
        p->prims.erase(p->prims.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }

    /* A glTF mesh has at least one primitive.  When the last primitive goes,
     * remove the now-empty mesh as dependency closure and detach node refs. */
    if (!mesh_id.empty() && find_mesh(*p, mesh_id) >= 0) {
        bool any_prim = false;
        for (std::size_t i = 0; i < p->prims.size(); ++i) {
            if (p->prims[i].mesh == mesh_id) {
                any_prim = true;
                break;
            }
        }
        if (!any_prim) {
            for (std::size_t i = 0; i < p->nodes.size(); ++i) {
                if (p->nodes[i].mesh == mesh_id) p->nodes[i].mesh.clear();
            }
            int mi = find_mesh(*p, mesh_id);
            if (mi >= 0)
                p->meshes.erase(p->meshes.begin() + static_cast<std::ptrdiff_t>(mi));
        }
    }
    return true;
}

bool delete_resource_if_unreferenced(Plan *p, const std::string &id,
                                     std::string *reason) {
    if (!p) return false;
    int ai = find_accessor(*p, id);
    if (ai >= 0) {
        for (std::size_t i = 0; i < p->prims.size(); ++i) {
            if (p->prims[i].position == id || p->prims[i].indices == id) {
                if (reason) *reason = "accessor-still-referenced";
                return false;
            }
        }
        p->accessors.erase(p->accessors.begin() + static_cast<std::ptrdiff_t>(ai));
        return true;
    }
    int vi = find_view(*p, id);
    if (vi >= 0) {
        for (std::size_t i = 0; i < p->accessors.size(); ++i) {
            if (p->accessors[i].view == id) {
                if (reason) *reason = "view-still-referenced";
                return false;
            }
        }
        p->views.erase(p->views.begin() + static_cast<std::ptrdiff_t>(vi));
        return true;
    }
    int bi = find_buffer(*p, id);
    if (bi >= 0) {
        for (std::size_t i = 0; i < p->views.size(); ++i) {
            if (p->views[i].buffer == id) {
                if (reason) *reason = "buffer-still-referenced";
                return false;
            }
        }
        p->buffers.erase(p->buffers.begin() + static_cast<std::ptrdiff_t>(bi));
        return true;
    }
    if (reason) *reason = "resource-not-found";
    return false;
}

void collect_used_vertices_and_compact(Plan *p) {
    if (!p) return;
    for (std::size_t ai = 0; ai < p->accessors.size(); ++ai) {
        AccessorModel &a = p->accessors[ai];
        if (a.role != "POSITION") continue;
        bool used_by_prim = false;
        std::vector<std::string> used_ids;
        for (std::size_t pi = 0; pi < p->prims.size(); ++pi) {
            const PrimModel &pr = p->prims[pi];
            if (pr.position != a.id) continue;
            used_by_prim = true;
            for (std::size_t ti = 0; ti < pr.triangles.size(); ++ti) {
                for (int k = 0; k < 3; ++k) {
                    const std::string &vid = pr.triangles[ti].v[k];
                    if (std::find(used_ids.begin(), used_ids.end(), vid) == used_ids.end())
                        used_ids.push_back(vid);
                }
            }
        }
        if (!used_by_prim) continue; /* unused accessor is a resource candidate */
        std::vector<Vertex> compact;
        for (std::size_t vi = 0; vi < a.vertices.size(); ++vi) {
            if (std::find(used_ids.begin(), used_ids.end(), a.vertices[vi].id) != used_ids.end())
                compact.push_back(a.vertices[vi]);
        }
        a.vertices.swap(compact);
    }
    sync_index_values(p);
}

bool apply_candidate(Plan *plan, const Candidate &c, std::string *reason) {
    if (!plan) return false;
    if (c.phase == PHASE_OUTSIDE || c.phase == PHASE_NODE) {
        if (c.target_kind == "node") {
            if (!delete_node_subtree(plan, c.target_id)) {
                if (reason) *reason = "node-already-absent";
                return false;
            }
            return true;
        }
        if (c.target_kind == "mesh") {
            if (find_mesh(*plan, c.target_id) < 0) {
                if (reason) *reason = "mesh-already-absent";
                return false;
            }
            for (std::size_t i = 0; i < plan->prims.size();) {
                if (plan->prims[i].mesh == c.target_id)
                    plan->prims.erase(plan->prims.begin() + static_cast<std::ptrdiff_t>(i));
                else ++i;
            }
            int mi = find_mesh(*plan, c.target_id);
            if (mi >= 0) plan->meshes.erase(plan->meshes.begin() + static_cast<std::ptrdiff_t>(mi));
            return true;
        }
        if (reason) *reason = "unsupported-outside-target";
        return false;
    }
    if (c.phase == PHASE_PRIMITIVE) {
        if (!delete_prim(plan, c.target_id)) {
            if (reason) *reason = "primitive-already-absent";
            return false;
        }
        return true;
    }
    if (c.phase == PHASE_RESOURCE) {
        return delete_resource_if_unreferenced(plan, c.target_id, reason);
    }
    if (c.phase == PHASE_TRIANGLE) {
        int pi = find_prim(*plan, c.prim_id);
        if (pi < 0) {
            if (reason) *reason = "primitive-already-absent";
            return false;
        }
        PrimModel &pr = plan->prims[pi];
        for (std::size_t ti = 0; ti < pr.triangles.size(); ++ti) {
            if (pr.triangles[ti].id != c.triangle_id) continue;
            pr.triangles.erase(pr.triangles.begin() + static_cast<std::ptrdiff_t>(ti));
            collect_used_vertices_and_compact(plan);
            return true;
        }
        if (reason) *reason = "triangle-already-absent";
        return false;
    }
    if (reason) *reason = "unknown-candidate-phase";
    return false;
}

bool structured_legal_model(const Plan &p, std::string *reason) {
    for (std::size_t i = 0; i < p.accessors.size(); ++i) {
        if (p.accessors[i].count() == 0) {
            if (reason) *reason = "zero-count accessor " + p.accessors[i].id;
            return false;
        }
    }
    for (std::size_t i = 0; i < p.prims.size(); ++i) {
        const PrimModel &pr = p.prims[i];
        if (pr.triangles.empty()) {
            if (reason) *reason = "primitive has no triangles " + pr.id;
            return false;
        }
        int pi = find_accessor(p, pr.position);
        int ii = find_accessor(p, pr.indices);
        if (pi < 0 || ii < 0) {
            if (reason) *reason = "primitive references missing accessor " + pr.id;
            return false;
        }
        const AccessorModel &pos = p.accessors[pi];
        const AccessorModel &idx = p.accessors[ii];
        if (idx.index_values.size() != pr.triangles.size() * 3) {
            if (reason) *reason = "index stream does not match triangles " + pr.id;
            return false;
        }
        for (std::size_t k = 0; k < idx.index_values.size(); ++k) {
            if (idx.index_values[k] >= pos.vertices.size()) {
                if (reason) *reason = "index out of range in " + pr.id;
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        if (!p.nodes[i].mesh.empty() && find_mesh(p, p.nodes[i].mesh) < 0) {
            if (reason) *reason = "node references missing mesh " + p.nodes[i].id;
            return false;
        }
        for (std::size_t j = 0; j < p.nodes[i].children.size(); ++j) {
            if (find_node(p, p.nodes[i].children[j]) < 0) {
                if (reason) *reason = "node references missing child " + p.nodes[i].id;
                return false;
            }
        }
    }
    for (std::size_t i = 0; i < p.scene.roots.size(); ++i) {
        if (find_node(p, p.scene.roots[i]) < 0) {
            if (reason) *reason = "scene root missing";
            return false;
        }
    }
    return true;
}

ReduceOutcome reduce_run(const ReduceOptions &options) {
    ReduceOutcome outcome;
    std::string error;
    ReduceOptions opt = options;
    if (opt.keep.empty()) opt.keep = KEEP_SHARED_STRIDED;
    if (!opt.verifier) {
        opt.verifier = [](const std::string &stage_dir) {
            return verify_directory_real(stage_dir);
        };
    }

    Plan source;
    if (!load_plan(opt.in_dir + "/case.plan", &source, &error)) {
        outcome.return_code = 2;
        outcome.message = "cannot load input plan: " + error;
        return outcome;
    }
    std::string source_keep_reason;
    if (!has_required_keep(source, opt.keep, &source_keep_reason)) {
        outcome.return_code = 2;
        outcome.message = "input does not match requested keep: " + source_keep_reason;
        return outcome;
    }

    if (!make_directories(opt.out_dir, &error)) {
        outcome.return_code = 2;
        outcome.message = error;
        return outcome;
    }
    std::string log_path = opt.out_dir + "/reduction.log";
    std::string state_path = opt.out_dir + "/reduction.state";
    std::string src_plan_fp = fingerprint_file(opt.in_dir + "/case.plan");
    std::string src_gltf_fp = fingerprint_file(opt.in_dir + "/case.gltf");
    std::vector<Candidate> candidates = build_candidates(source);

    RecoveryState st;
    Plan current;
    Metrics current_metrics;

    struct stat state_st;
    bool state_exists = ::stat(state_path.c_str(), &state_st) == 0;
    if (opt.resume) {
        if (!state_exists) {
            outcome.return_code = 2;
            outcome.message = "--resume requested but no reduction.state exists";
            return outcome;
        }
        std::string state_text;
        if (!read_file(state_path, &state_text, &error) || !state_from_string(state_text, &st)) {
            outcome.return_code = 2;
            outcome.message = "cannot read recovery state: " + error;
            return outcome;
        }
        if (st.source_plan_fp != src_plan_fp || st.source_gltf_fp != src_gltf_fp) {
            outcome.return_code = 2;
            outcome.message = "input mismatch: source fingerprint does not match reduction.state";
            return outcome;
        }
        if (st.keep != opt.keep) {
            outcome.return_code = 2;
            outcome.message = "keep mismatch: state has " + st.keep + ", command requested " + opt.keep;
            return outcome;
        }
        if (!load_plan(opt.out_dir + "/case.plan", &current, &error)) {
            outcome.return_code = 2;
            outcome.message = "cannot load committed OUT/case.plan: " + error;
            return outcome;
        }
        std::string keep_reason;
        if (!has_required_keep(current, opt.keep, &keep_reason)) {
            outcome.return_code = 2;
            outcome.message = "committed output no longer matches keep: " + keep_reason;
            return outcome;
        }
        st.total = candidates.size();
        outcome.observed = 0;
        append_log(log_path, "RESUME start next_index=" + std::to_string(st.next_index) +
                             " budget=" + std::to_string(opt.budget));
    } else {
        if (state_exists) {
            outcome.return_code = 2;
            outcome.message = "OUT already contains reduction.state; pass --resume to continue";
            return outcome;
        }
        current = source;
        if (!copy_file_bytes(opt.in_dir + "/case.plan",
                             opt.out_dir + "/case.plan", &error) ||
            !copy_file_bytes(opt.in_dir + "/case.gltf",
                             opt.out_dir + "/case.gltf", &error)) {
            outcome.return_code = 2;
            outcome.message = "cannot initialize output: " + error;
            return outcome;
        }
        st.source_plan_fp = src_plan_fp;
        st.source_gltf_fp = src_gltf_fp;
        st.keep = opt.keep;
        st.next_index = 0;
        st.total = candidates.size();
        st.observed_total = 0;
        st.accepted_total = 0;
        st.done = false;

        std::ofstream log(log_path.c_str(), std::ios::binary | std::ios::trunc);
        if (log) {
            log << "# cgltf structured reduction log v1\n";
            log << "keep=" << opt.keep << "\n";
            log << "source_plan_fp=" << src_plan_fp << "\n";
            log << "source_gltf_fp=" << src_gltf_fp << "\n";
            log << "candidate_count=" << candidates.size() << "\n";
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                const Candidate &c = candidates[i];
                log << "CANDIDATE_ORDER index=" << i << " phase=" << c.phase_name
                    << " id=" << c.id << " target_kind=" << c.target_kind
                    << " target_id=" << c.target_id;
                if (!c.prim_id.empty()) log << " prim_id=" << c.prim_id;
                if (!c.triangle_id.empty()) log << " triangle_id=" << c.triangle_id;
                log << "\n";
            }
        }
        append_log(log_path, "START budget=" + std::to_string(opt.budget));
        write_file_atomic(state_path, state_to_string(st), &error);
    }

    /* Recover text metric from the committed glTF. */
    {
        std::string gltf;
        if (read_file(opt.out_dir + "/case.gltf", &gltf, NULL))
            current_metrics = compute_metrics(current, gltf.size());
    }

    std::string stage_dir = opt.out_dir + "/.stage";
    if (!make_directories(stage_dir, &error)) {
        outcome.return_code = 2;
        outcome.message = error;
        return outcome;
    }

    std::uint64_t run_observed = 0;
    while (run_observed < opt.budget && st.next_index < candidates.size()) {
        Candidate c = candidates[st.next_index];
        ++run_observed;
        ++st.observed_total;
        outcome.observed = run_observed;
        outcome.next_index = st.next_index + 1;
        outcome.total_candidates = candidates.size();

        append_log(log_path, "OBSERVE index=" + std::to_string(st.next_index) +
                             " run_number=" + std::to_string(run_observed) +
                             " total_observed=" + std::to_string(st.observed_total) +
                             " phase=" + c.phase_name + " id=" + c.id);

        Plan trial = current;
        std::string apply_reason;
        bool applicable = apply_candidate(&trial, c, &apply_reason);
        bool accepted = false;
        std::string reject_reason;
        Metrics trial_metrics;
        VerifyResult verify;

        if (!applicable) {
            reject_reason = "not-applicable:" + apply_reason;
        } else {
            sync_index_values(&trial);
            std::string legal_reason;
            std::string write_error;
            if (!structured_legal_model(trial, &legal_reason)) {
                reject_reason = "illegal-model:" + legal_reason;
            } else if (!write_case_files(&trial, stage_dir, &trial_metrics, &write_error)) {
                reject_reason = "write-failed:" + write_error;
            } else {
                verify = opt.verifier(stage_dir);
                std::string keep_reason;
                bool keep_ok = has_required_keep(trial, opt.keep, &keep_reason);
                if (!verify.ok) {
                    reject_reason = "production-read-failed:" + verify.first_id + ":" +
                                    verify.element + ":" + verify.message;
                } else if (!keep_ok) {
                    reject_reason = "keep-lost:" + keep_reason;
                } else if (!trial_metrics.lex_less(current_metrics)) {
                    reject_reason = "metric-not-strictly-smaller";
                } else {
                    std::string cp_err;
                    if (!copy_file_bytes(stage_dir + "/case.plan",
                                         opt.out_dir + "/case.plan", &cp_err) ||
                        !copy_file_bytes(stage_dir + "/case.gltf",
                                         opt.out_dir + "/case.gltf", &cp_err)) {
                        reject_reason = "commit-copy-failed:" + cp_err;
                    } else {
                        accepted = true;
                    }
                }
            }
        }

        if (accepted) {
            current = trial;
            current_metrics = trial_metrics;
            ++st.accepted_total;
            ++outcome.accepted;
            append_log(log_path,
                       "ACCEPT index=" + std::to_string(st.next_index) +
                       " id=" + c.id +
                       " objects=" + std::to_string(trial_metrics.objects) +
                       " triangles=" + std::to_string(trial_metrics.triangles) +
                       " vertices=" + std::to_string(trial_metrics.vertices) +
                       " buffer_bytes=" + std::to_string(trial_metrics.buffer_bytes) +
                       " text_bytes=" + std::to_string(trial_metrics.text_bytes) +
                       " production_read=OK");
        } else {
            append_log(log_path, "REJECT index=" + std::to_string(st.next_index) +
                                 " id=" + c.id + " reason=" + reject_reason);
        }

        ++st.next_index;
        if (st.next_index >= candidates.size()) st.done = true;
        std::string state_error;
        if (!write_file_atomic(state_path, state_to_string(st), &state_error)) {
            outcome.return_code = 2;
            outcome.message = "cannot persist recovery state: " + state_error;
            return outcome;
        }
    }

    outcome.next_index = st.next_index;
    outcome.total_candidates = candidates.size();
    outcome.done = st.done;
    if (st.done) {
        outcome.return_code = 0;
        outcome.message = "REDUCTION_COMPLETE";
        append_log(log_path, "COMPLETE accepted_total=" + std::to_string(st.accepted_total));
    } else {
        outcome.return_code = 0;
        outcome.message = "BUDGET_EXHAUSTED next_index=" + std::to_string(st.next_index);
        append_log(log_path, "BUDGET_EXHAUSTED next_index=" + std::to_string(st.next_index) +
                             " run_observed=" + std::to_string(run_observed));
    }
    return outcome;
}

} // namespace cgltf_structured
