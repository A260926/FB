#include "fuzz/structured/driver.h"

#include "cgltf.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace cgltf_structured {

namespace {

std::vector<std::string> split_lines(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (text[i] != '\r') {
            cur.push_back(text[i]);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::map<std::string, std::string> key_values(const std::string &line) {
    std::map<std::string, std::string> kv;
    std::istringstream in(line);
    std::string token;
    in >> token; /* record tag */
    while (in >> token) {
        std::size_t eq = token.find('=');
        if (eq == std::string::npos) continue;
        kv[token.substr(0, eq)] = token.substr(eq + 1);
    }
    return kv;
}

std::string kv(const std::map<std::string, std::string> &m,
               const std::string &k, const std::string &d = "") {
    std::map<std::string, std::string>::const_iterator it = m.find(k);
    return it == m.end() ? d : it->second;
}

std::vector<std::string> csv(const std::string &s) {
    std::vector<std::string> out;
    if (s.empty() || s == "-") return out;
    std::string cur;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(s[i]);
        }
    }
    return out;
}

int ptr_accessor(cgltf_data *d, cgltf_accessor *p) {
    if (!p) return -1;
    for (cgltf_size i = 0; i < d->accessors_count; ++i)
        if (&d->accessors[i] == p) return static_cast<int>(i);
    return -1;
}

int ptr_view(cgltf_data *d, cgltf_buffer_view *p) {
    if (!p) return -1;
    for (cgltf_size i = 0; i < d->buffer_views_count; ++i)
        if (&d->buffer_views[i] == p) return static_cast<int>(i);
    return -1;
}

int ptr_buffer(cgltf_data *d, cgltf_buffer *p) {
    if (!p) return -1;
    for (cgltf_size i = 0; i < d->buffers_count; ++i)
        if (&d->buffers[i] == p) return static_cast<int>(i);
    return -1;
}

int ptr_node(cgltf_data *d, cgltf_node *p) {
    if (!p) return -1;
    for (cgltf_size i = 0; i < d->nodes_count; ++i)
        if (&d->nodes[i] == p) return static_cast<int>(i);
    return -1;
}

std::string cstr(const char *s) { return s ? std::string(s) : std::string(); }

std::string num(double x) {
    if (x == 0.0) x = 0.0;
    std::ostringstream out;
    out << std::fixed;
    out.precision(6);
    out << x;
    return out.str();
}

void add_share(std::vector<ProdShare> *shares, const std::string &kind,
               int accessor, const std::string &user) {
    if (accessor < 0) return;
    for (std::size_t i = 0; i < shares->size(); ++i) {
        if ((*shares)[i].kind == kind && (*shares)[i].accessor == accessor) {
            (*shares)[i].users.push_back(user);
            std::sort((*shares)[i].users.begin(), (*shares)[i].users.end());
            (*shares)[i].use_count = (*shares)[i].users.size();
            return;
        }
    }
    ProdShare s;
    s.kind = kind;
    s.accessor = accessor;
    s.use_count = 1;
    s.users.push_back(user);
    shares->push_back(s);
}

std::string format_report(const ProductionData &d) {
    std::ostringstream out;
    out << "CGLTF_STRUCTURED_WORKER 1\n";
    out << "GLTF version=" << d.gltf_version << " scene=" << d.scene << "\n";
    for (std::size_t i = 0; i < d.buffers.size(); ++i) {
        const ProdBuffer &b = d.buffers[i];
        out << "BUFFER i=" << i << " name=" << b.name << " size=" << b.size << "\n";
    }
    for (std::size_t i = 0; i < d.views.size(); ++i) {
        const ProdView &v = d.views[i];
        out << "VIEW i=" << i << " name=" << v.name << " buffer=" << v.buffer
            << " offset=" << v.offset << " size=" << v.size
            << " stride=" << v.stride << "\n";
    }
    for (std::size_t i = 0; i < d.accessors.size(); ++i) {
        const ProdAccessor &a = d.accessors[i];
        out << "ACCESSOR i=" << i << " name=" << a.name
            << " component_type=" << a.component_type << " type=" << a.type
            << " count=" << a.count << " view=" << a.view
            << " offset=" << a.offset << " stride=" << a.stride << "\n";
    }
    for (std::size_t i = 0; i < d.prims.size(); ++i) {
        const ProdPrim &p = d.prims[i];
        out << "PRIM mesh=" << p.mesh_index << " prim=" << p.prim_index
            << " mode=" << p.mode << " indices=" << p.indices
            << " position=" << p.position << "\n";
    }
    for (std::size_t i = 0; i < d.nodes.size(); ++i) {
        const ProdNode &n = d.nodes[i];
        out << "NODE i=" << i << " name=" << n.name << " mesh=" << n.mesh
            << " children=";
        for (std::size_t j = 0; j < n.children.size(); ++j) {
            if (j) out << ",";
            out << n.children[j];
        }
        out << "\n";
    }
    for (std::size_t i = 0; i < d.scenes.size(); ++i) {
        out << "SCENE i=" << i << " roots=";
        for (std::size_t j = 0; j < d.scenes[i].size(); ++j) {
            if (j) out << ",";
            out << d.scenes[i][j];
        }
        out << "\n";
    }
    for (std::size_t i = 0; i < d.shares.size(); ++i) {
        const ProdShare &s = d.shares[i];
        out << "SHARE kind=" << s.kind << " accessor=" << s.accessor
            << " count=" << s.use_count << " users=";
        for (std::size_t j = 0; j < s.users.size(); ++j) {
            if (j) out << ",";
            out << s.users[j];
        }
        out << "\n";
    }
    for (std::size_t i = 0; i < d.values.size(); ++i) {
        const ProdValues &v = d.values[i];
        out << "VALUES accessor=" << v.accessor << " count=" << v.count
            << " components=" << v.components << "\n";
        for (std::size_t e = 0; e < v.count; ++e) {
            out << "VALUE accessor=" << v.accessor << " index=" << e
                << " components=" << v.components << " values=";
            for (std::size_t c = 0; c < v.components; ++c) {
                if (c) out << ",";
                out << num(v.values[e * v.components + c]);
            }
            out << "\n";
        }
    }
    out << "END OK\n";
    return out.str();
}

bool nearly(double a, double b) {
    double scale = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= 1e-5 * scale + 1e-6;
}

} // namespace

VerifyResult compare_production(const Plan &p, const ProductionData &a) {
    if (!a.complete)
        return VerifyResult::failure("worker", "complete", "incomplete worker report");
    if (a.buffers.size() != p.buffers.size())
        return VerifyResult::failure("buffers", "count", "buffer count mismatch");
    if (a.views.size() != p.views.size())
        return VerifyResult::failure("bufferViews", "count", "view count mismatch");
    if (a.accessors.size() != p.accessors.size())
        return VerifyResult::failure("accessors", "count", "accessor count mismatch");
    if (a.nodes.size() != p.nodes.size())
        return VerifyResult::failure("nodes", "count", "node count mismatch");

    for (std::size_t i = 0; i < p.buffers.size(); ++i) {
        const BufferModel &e = p.buffers[i];
        const ProdBuffer &g = a.buffers[i];
        if (g.name != e.id)
            return VerifyResult::failure(e.id, "name", "buffer stable identity mismatch: " + g.name);
        if (g.size != e.bytes.size())
            return VerifyResult::failure(e.id, "byteLength", "buffer size mismatch");
    }
    for (std::size_t i = 0; i < p.views.size(); ++i) {
        const ViewModel &e = p.views[i];
        const ProdView &g = a.views[i];
        if (g.name != e.id)
            return VerifyResult::failure(e.id, "name", "view stable identity mismatch");
        int bi = find_buffer(p, e.buffer);
        if (g.buffer != bi)
            return VerifyResult::failure(e.id, "buffer", "view buffer reference mismatch");
        if (g.offset != e.offset)
            return VerifyResult::failure(e.id, "byteOffset", "view offset mismatch");
        if (g.size != e.size)
            return VerifyResult::failure(e.id, "byteLength", "view size mismatch");
        if (g.stride != e.stride)
            return VerifyResult::failure(e.id, "byteStride", "view stride mismatch");
    }
    for (std::size_t i = 0; i < p.accessors.size(); ++i) {
        const AccessorModel &e = p.accessors[i];
        const ProdAccessor &g = a.accessors[i];
        if (g.name != e.id)
            return VerifyResult::failure(e.id, "name", "accessor stable identity mismatch");
        if (g.component_type != e.component_type)
            return VerifyResult::failure(e.id, "componentType", "component type mismatch");
        if (g.type != e.type)
            return VerifyResult::failure(e.id, "type", "accessor type mismatch");
        if (g.count != e.count())
            return VerifyResult::failure(e.id, "count", "accessor count mismatch");
        int vi = find_view(p, e.view);
        if (g.view != vi)
            return VerifyResult::failure(e.id, "bufferView", "accessor view reference mismatch");
        if (g.offset != e.offset)
            return VerifyResult::failure(e.id, "byteOffset", "accessor offset mismatch");
        std::size_t expected_stride = e.element_size();
        if (vi >= 0 && p.views[vi].stride) expected_stride = p.views[vi].stride;
        if (g.stride != expected_stride)
            return VerifyResult::failure(e.id, "stride", "accessor stride mismatch");
    }

    std::size_t expected_prim_count = p.prims.size();
    if (a.prims.size() != expected_prim_count)
        return VerifyResult::failure("primitives", "count", "primitive count mismatch");
    for (std::size_t mi = 0; mi < p.meshes.size(); ++mi) {
        std::size_t local = 0;
        for (std::size_t pi = 0; pi < p.prims.size(); ++pi) {
            if (p.prims[pi].mesh != p.meshes[mi].id) continue;
            if (local >= a.prims.size())
                return VerifyResult::failure(p.prims[pi].id, "identity", "missing worker primitive");
            /* a.prims is emitted in mesh/local order; find matching record. */
            const ProdPrim *g = NULL;
            for (std::size_t k = 0; k < a.prims.size(); ++k) {
                if (a.prims[k].mesh_index == mi && a.prims[k].prim_index == local) {
                    g = &a.prims[k];
                    break;
                }
            }
            if (!g)
                return VerifyResult::failure(p.prims[pi].id, "identity", "worker did not report primitive");
            if (g->mode != p.prims[pi].mode)
                return VerifyResult::failure(p.prims[pi].id, "mode", "primitive mode mismatch");
            if (g->indices != find_accessor(p, p.prims[pi].indices))
                return VerifyResult::failure(p.prims[pi].id, "indices", "index sharing/reference mismatch");
            if (g->position != find_accessor(p, p.prims[pi].position))
                return VerifyResult::failure(p.prims[pi].id, "POSITION", "position sharing/reference mismatch");
            ++local;
        }
    }

    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        const NodeModel &e = p.nodes[i];
        const ProdNode &g = a.nodes[i];
        if (g.name != e.id)
            return VerifyResult::failure(e.id, "name", "node stable identity mismatch");
        int mi = e.mesh.empty() ? -1 : find_mesh(p, e.mesh);
        if (g.mesh != mi)
            return VerifyResult::failure(e.id, "mesh", "node mesh reference mismatch");
        if (g.children.size() != e.children.size())
            return VerifyResult::failure(e.id, "children.count", "child count mismatch");
        for (std::size_t c = 0; c < e.children.size(); ++c) {
            int ci = find_node(p, e.children[c]);
            if (c >= g.children.size() || static_cast<int>(g.children[c]) != ci)
                return VerifyResult::failure(e.id, "children[" + std::to_string(c) + "]",
                                             "child stable reference mismatch");
        }
    }
    if (a.scenes.size() != 1 || a.scene != 0)
        return VerifyResult::failure(p.scene.id, "scene", "scene identity/count mismatch");
    std::vector<int> expected_roots;
    for (std::size_t i = 0; i < p.scene.roots.size(); ++i)
        expected_roots.push_back(find_node(p, p.scene.roots[i]));
    if (a.scenes[0].size() != expected_roots.size())
        return VerifyResult::failure(p.scene.id, "nodes.count", "scene root count mismatch");
    for (std::size_t i = 0; i < expected_roots.size(); ++i) {
        if (static_cast<int>(a.scenes[0][i]) != expected_roots[i])
            return VerifyResult::failure(p.scene.id, "nodes[" + std::to_string(i) + "]",
                                         "scene root mismatch");
    }

    /* Reference sharing relationships, independently derived from the plan. */
    std::vector<ProdShare> expected_shares;
    for (std::size_t mi = 0; mi < p.meshes.size(); ++mi) {
        std::size_t local = 0;
        for (std::size_t pi = 0; pi < p.prims.size(); ++pi) {
            const PrimModel &pr = p.prims[pi];
            if (pr.mesh != p.meshes[mi].id) continue;
            std::string user = std::to_string(mi) + ":" + std::to_string(local);
            add_share(&expected_shares, "POSITION", find_accessor(p, pr.position), user);
            add_share(&expected_shares, "INDEX", find_accessor(p, pr.indices), user);
            ++local;
        }
    }
    if (expected_shares.size() != a.shares.size())
        return VerifyResult::failure("sharing", "count", "sharing relationship count mismatch");
    for (std::size_t i = 0; i < expected_shares.size(); ++i) {
        const ProdShare &e = expected_shares[i];
        const ProdShare *g = NULL;
        for (std::size_t j = 0; j < a.shares.size(); ++j) {
            if (a.shares[j].kind == e.kind && a.shares[j].accessor == e.accessor) {
                g = &a.shares[j];
                break;
            }
        }
        if (!g) {
            std::string id = (e.accessor >= 0 && static_cast<std::size_t>(e.accessor) < p.accessors.size())
                                 ? p.accessors[e.accessor].id
                                 : "sharing";
            return VerifyResult::failure(id, "share." + e.kind, "missing sharing relationship");
        }
        if (g->use_count != e.use_count || g->users != e.users) {
            std::string id = p.accessors[e.accessor].id;
            return VerifyResult::failure(id, "share." + e.kind, "sharing users mismatch");
        }
    }

    /* Every index/POSITION numeric value, compared to the independent model. */
    for (std::size_t ai = 0; ai < p.accessors.size(); ++ai) {
        const AccessorModel &e = p.accessors[ai];
        const ProdValues *g = NULL;
        for (std::size_t vi = 0; vi < a.values.size(); ++vi)
            if (a.values[vi].accessor == static_cast<int>(ai)) {
                g = &a.values[vi];
                break;
            }
        if (!g)
            return VerifyResult::failure(e.id, "values", "worker omitted accessor values");
        if (g->count != e.count() || g->components != e.element_components())
            return VerifyResult::failure(e.id, "values.count", "value shape mismatch");
        for (std::size_t el = 0; el < e.count(); ++el) {
            for (std::size_t c = 0; c < e.element_components(); ++c) {
                double ev = 0.0;
                if (e.role == "POSITION") {
                    const Vertex &v = e.vertices[el];
                    ev = c == 0 ? v.x : (c == 1 ? v.y : v.z);
                } else {
                    ev = e.index_values[el];
                }
                double gv = g->values[el * e.element_components() + c];
                if (!nearly(ev, gv))
                    return VerifyResult::failure(
                        e.id, "values[" + std::to_string(el) + "][" + std::to_string(c) + "]",
                        "numeric mismatch expected=" + num(ev) + " got=" + num(gv));
            }
        }
    }
    return VerifyResult::success();
}

std::vector<char *> exec_argv_storage(const std::vector<std::string> &argv,
                                      std::vector<char> *storage) {
    (void)storage;
    std::vector<char *> pointers;
    for (std::size_t i = 0; i < argv.size(); ++i)
        pointers.push_back(const_cast<char *>(argv[i].c_str()));
    pointers.push_back(NULL);
    return pointers;
}

ProcResult run_process(const std::vector<std::string> &argv) {
    ProcResult result;
    if (argv.empty()) return result;
    int pipefd[2];
    if (pipe(pipefd) != 0) return result;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return result;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        std::vector<char *> args;
        for (std::size_t i = 0; i < argv.size(); ++i)
            args.push_back(const_cast<char *>(argv[i].c_str()));
        args.push_back(NULL);
        execvp(args[0], args.data());
        _exit(127);
    }
    result.spawn_ok = true;
    close(pipefd[1]);
    char buf[4096];
    while (true) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) result.output.append(buf, static_cast<std::size_t>(n));
        else if (n == 0) break;
        else if (errno != EINTR) break;
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) {
        result.signaled = true;
        result.signal_number = WTERMSIG(status);
        result.exit_code = -1;
    }
    return result;
}

ProcResult run_worker_process(const std::string &self_exe,
                              const std::string &dir) {
    std::string exe = self_exe.empty() ? "cgltf-defense" : self_exe;
    return run_process(std::vector<std::string>{exe, "worker", dir});
}

bool parse_worker_output(const std::string &text, ProductionData *out,
                         std::string *error) {
    if (!out) return false;
    ProductionData d;
    std::vector<std::string> lines = split_lines(text);
    bool began = false;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        std::string line = lines[li];
        if (line.empty()) continue;
        if (line == "CGLTF_STRUCTURED_WORKER 1") {
            began = true;
            continue;
        }
        if (line == "END OK") {
            d.complete = began;
            *out = d;
            return began;
        }
        if (!began) continue;
        std::map<std::string, std::string> m = key_values(line);
        std::string tag;
        {
            std::istringstream first(line);
            first >> tag;
        }
        if (tag == "GLTF") {
            d.gltf_version = kv(m, "version");
            d.scene = static_cast<std::size_t>(std::strtoull(kv(m, "scene", "0").c_str(), NULL, 10));
        } else if (tag == "BUFFER") {
            ProdBuffer b;
            b.index = std::strtoull(kv(m, "i", "0").c_str(), NULL, 10);
            b.name = kv(m, "name");
            b.size = std::strtoull(kv(m, "size", "0").c_str(), NULL, 10);
            d.buffers.push_back(b);
        } else if (tag == "VIEW") {
            ProdView v;
            v.index = std::strtoull(kv(m, "i", "0").c_str(), NULL, 10);
            v.name = kv(m, "name");
            v.buffer = std::atoi(kv(m, "buffer", "-1").c_str());
            v.offset = std::strtoull(kv(m, "offset", "0").c_str(), NULL, 10);
            v.size = std::strtoull(kv(m, "size", "0").c_str(), NULL, 10);
            v.stride = std::strtoull(kv(m, "stride", "0").c_str(), NULL, 10);
            d.views.push_back(v);
        } else if (tag == "ACCESSOR") {
            ProdAccessor a;
            a.index = std::strtoull(kv(m, "i", "0").c_str(), NULL, 10);
            a.name = kv(m, "name");
            a.component_type = std::atoi(kv(m, "component_type", "0").c_str());
            a.type = kv(m, "type");
            a.count = std::strtoull(kv(m, "count", "0").c_str(), NULL, 10);
            a.view = std::atoi(kv(m, "view", "-1").c_str());
            a.offset = std::strtoull(kv(m, "offset", "0").c_str(), NULL, 10);
            a.stride = std::strtoull(kv(m, "stride", "0").c_str(), NULL, 10);
            d.accessors.push_back(a);
        } else if (tag == "PRIM") {
            ProdPrim p;
            p.mesh_index = std::strtoull(kv(m, "mesh", "0").c_str(), NULL, 10);
            p.prim_index = std::strtoull(kv(m, "prim", "0").c_str(), NULL, 10);
            p.mode = std::atoi(kv(m, "mode", "4").c_str());
            p.indices = std::atoi(kv(m, "indices", "-1").c_str());
            p.position = std::atoi(kv(m, "position", "-1").c_str());
            d.prims.push_back(p);
        } else if (tag == "NODE") {
            ProdNode n;
            n.index = std::strtoull(kv(m, "i", "0").c_str(), NULL, 10);
            n.name = kv(m, "name");
            n.mesh = std::atoi(kv(m, "mesh", "-1").c_str());
            std::vector<std::string> kids = csv(kv(m, "children"));
            for (std::size_t i = 0; i < kids.size(); ++i)
                n.children.push_back(std::strtoull(kids[i].c_str(), NULL, 10));
            d.nodes.push_back(n);
        } else if (tag == "SCENE") {
            std::vector<std::string> roots = csv(kv(m, "roots"));
            std::vector<std::size_t> r;
            for (std::size_t i = 0; i < roots.size(); ++i)
                r.push_back(std::strtoull(roots[i].c_str(), NULL, 10));
            d.scenes.push_back(r);
        } else if (tag == "SHARE") {
            ProdShare s;
            s.kind = kv(m, "kind");
            s.accessor = std::atoi(kv(m, "accessor", "-1").c_str());
            s.use_count = std::strtoull(kv(m, "count", "0").c_str(), NULL, 10);
            s.users = csv(kv(m, "users"));
            d.shares.push_back(s);
        } else if (tag == "VALUES") {
            int acc = std::atoi(kv(m, "accessor", "-1").c_str());
            ProdValues v;
            v.accessor = acc;
            v.count = 0;
            v.components = std::strtoull(kv(m, "components", "1").c_str(), NULL, 10);
            d.values.push_back(v);
        } else if (tag == "VALUE") {
            int acc = std::atoi(kv(m, "accessor", "-1").c_str());
            std::size_t components = std::strtoull(kv(m, "components", "1").c_str(), NULL, 10);
            std::vector<std::string> vals = csv(kv(m, "values"));
            ProdValues *target = NULL;
            for (std::size_t i = 0; i < d.values.size(); ++i)
                if (d.values[i].accessor == acc) target = &d.values[i];
            if (!target) {
                ProdValues v;
                v.accessor = acc;
                v.count = 0;
                v.components = components;
                d.values.push_back(v);
                target = &d.values.back();
            }
            if (components) target->components = components;
            for (std::size_t i = 0; i < vals.size(); ++i)
                target->values.push_back(std::strtod(vals[i].c_str(), NULL));
            ++target->count;
        }
    }
    if (error) *error = "missing END OK marker";
    return false;
}

VerifyResult evaluate_worker_result(const Plan &expected,
                                    const ProcResult &result) {
    if (!result.spawn_ok)
        return VerifyResult::failure("process", "spawn", "failed to spawn worker process");
    if (result.signaled)
        return VerifyResult::failure("process", "signal", "worker terminated by signal");
    if (!result.clean_exit_zero())
        return VerifyResult::failure("process", "exit_code",
                                     "worker exited nonzero code=" + std::to_string(result.exit_code));
    if (result.output.empty())
        return VerifyResult::failure("worker", "output", "worker produced no output");
    ProductionData parsed;
    std::string parse_error;
    if (!parse_worker_output(result.output, &parsed, &parse_error) || !parsed.complete)
        return VerifyResult::failure("worker", "complete", "worker output was incomplete: " + parse_error);
    return compare_production(expected, parsed);
}

VerifyResult production_read_gltf(const std::string &gltf_path,
                                  ProductionData *out_data) {
    cgltf_options options;
    std::memset(&options, 0, sizeof(options));
    cgltf_data *d = NULL;
    cgltf_result r = cgltf_parse_file(&options, gltf_path.c_str(), &d);
    if (r != cgltf_result_success || !d)
        return VerifyResult::failure("cgltf", "parse_file", "cgltf_parse_file failed result=" + std::to_string(r));
    r = cgltf_load_buffers(&options, d, gltf_path.c_str());
    if (r != cgltf_result_success) {
        cgltf_free(d);
        return VerifyResult::failure("cgltf", "load_buffers", "cgltf_load_buffers failed result=" + std::to_string(r));
    }
    r = cgltf_validate(d);
    if (r != cgltf_result_success) {
        cgltf_free(d);
        return VerifyResult::failure("cgltf", "validate", "cgltf_validate failed result=" + std::to_string(r));
    }

    ProductionData out;
    out.gltf_version = "2.0";
    out.scene = 0;

    for (cgltf_size i = 0; i < d->buffers_count; ++i) {
        ProdBuffer b;
        b.index = i;
        b.name = cstr(d->buffers[i].name);
        b.size = d->buffers[i].size;
        out.buffers.push_back(b);
    }
    for (cgltf_size i = 0; i < d->buffer_views_count; ++i) {
        ProdView v;
        v.index = i;
        v.name = cstr(d->buffer_views[i].name);
        v.buffer = ptr_buffer(d, d->buffer_views[i].buffer);
        v.offset = d->buffer_views[i].offset;
        v.size = d->buffer_views[i].size;
        v.stride = d->buffer_views[i].stride;
        out.views.push_back(v);
    }
    for (cgltf_size i = 0; i < d->accessors_count; ++i) {
        cgltf_accessor *a = &d->accessors[i];
        ProdAccessor pa;
        pa.index = i;
        pa.name = cstr(a->name);
        switch (a->component_type) {
        case cgltf_component_type_r_8: pa.component_type = 5120; break;
        case cgltf_component_type_r_8u: pa.component_type = 5121; break;
        case cgltf_component_type_r_16: pa.component_type = 5122; break;
        case cgltf_component_type_r_16u: pa.component_type = 5123; break;
        case cgltf_component_type_r_32u: pa.component_type = 5125; break;
        case cgltf_component_type_r_32f: pa.component_type = 5126; break;
        default: pa.component_type = 0; break;
        }
        switch (a->type) {
        case cgltf_type_scalar: pa.type = "SCALAR"; break;
        case cgltf_type_vec2: pa.type = "VEC2"; break;
        case cgltf_type_vec3: pa.type = "VEC3"; break;
        case cgltf_type_vec4: pa.type = "VEC4"; break;
        default: pa.type = "UNKNOWN"; break;
        }
        pa.count = a->count;
        pa.view = ptr_view(d, a->buffer_view);
        pa.offset = a->offset;
        pa.stride = a->stride;
        out.accessors.push_back(pa);

        ProdValues vals;
        vals.accessor = static_cast<int>(i);
        vals.count = a->count;
        vals.components = cgltf_num_components(a->type);
        vals.values.resize(a->count * vals.components);
        for (cgltf_size e = 0; e < a->count; ++e) {
            if (a->component_type == cgltf_component_type_r_16u ||
                a->component_type == cgltf_component_type_r_8u ||
                a->component_type == cgltf_component_type_r_32u) {
                cgltf_uint u[16] = {0};
                if (!cgltf_accessor_read_uint(a, e, u, vals.components)) {
                    cgltf_free(d);
                    return VerifyResult::failure(pa.name, "read_uint[" + std::to_string(e) + "]",
                                                 "cgltf_accessor_read_uint failed");
                }
                for (std::size_t c = 0; c < vals.components; ++c)
                    vals.values[e * vals.components + c] = static_cast<double>(u[c]);
            } else {
                float f[16] = {0};
                if (!cgltf_accessor_read_float(a, e, f, vals.components)) {
                    cgltf_free(d);
                    return VerifyResult::failure(pa.name, "read_float[" + std::to_string(e) + "]",
                                                 "cgltf_accessor_read_float failed");
                }
                for (std::size_t c = 0; c < vals.components; ++c) {
                    if (!std::isfinite(f[c])) {
                        cgltf_free(d);
                        return VerifyResult::failure(pa.name, "nonfinite[" + std::to_string(e) + "]",
                                                     "non-finite accessor value");
                    }
                    vals.values[e * vals.components + c] = static_cast<double>(f[c]);
                }
            }
        }
        out.values.push_back(vals);
    }

    for (cgltf_size mi = 0; mi < d->meshes_count; ++mi) {
        cgltf_mesh *mesh = &d->meshes[mi];
        for (cgltf_size pi = 0; pi < mesh->primitives_count; ++pi) {
            cgltf_primitive *pr = &mesh->primitives[pi];
            ProdPrim pp;
            pp.mesh_index = mi;
            pp.prim_index = pi;
            pp.mode = static_cast<int>(pr->type) - 1; /* cgltf has an invalid zero */
            pp.indices = ptr_accessor(d, pr->indices);
            pp.position = -1;
            for (cgltf_size ai = 0; ai < pr->attributes_count; ++ai) {
                if (pr->attributes[ai].type == cgltf_attribute_type_position)
                    pp.position = ptr_accessor(d, pr->attributes[ai].data);
            }
            out.prims.push_back(pp);
            std::string user = std::to_string(mi) + ":" + std::to_string(pi);
            add_share(&out.shares, "POSITION", pp.position, user);
            add_share(&out.shares, "INDEX", pp.indices, user);
        }
    }
    for (cgltf_size i = 0; i < d->nodes_count; ++i) {
        ProdNode n;
        n.index = i;
        n.name = cstr(d->nodes[i].name);
        n.mesh = -1;
        if (d->nodes[i].mesh) {
            for (cgltf_size m = 0; m < d->meshes_count; ++m)
                if (&d->meshes[m] == d->nodes[i].mesh) n.mesh = static_cast<int>(m);
        }
        for (cgltf_size c = 0; c < d->nodes[i].children_count; ++c)
            n.children.push_back(ptr_node(d, d->nodes[i].children[c]));
        out.nodes.push_back(n);
    }
    for (cgltf_size si = 0; si < d->scenes_count; ++si) {
        std::vector<std::size_t> roots;
        for (cgltf_size n = 0; n < d->scenes[si].nodes_count; ++n)
            roots.push_back(ptr_node(d, d->scenes[si].nodes[n]));
        out.scenes.push_back(roots);
    }
    out.complete = true;
    if (out_data) *out_data = out;
    cgltf_free(d);
    return VerifyResult::success();
}

VerifyResult verify_directory_real(const std::string &dir) {
    Plan plan;
    std::string error;
    if (!load_plan(dir + "/case.plan", &plan, &error))
        return VerifyResult::failure("plan", "read", error);
    ProductionData data;
    VerifyResult r = production_read_gltf(dir + "/case.gltf", &data);
    if (!r.ok) return r;
    return compare_production(plan, data);
}

int worker_main(const std::string &dir) {
    ProductionData data;
    VerifyResult r = production_read_gltf(dir + "/case.gltf", &data);
    if (!r.ok) {
        std::fprintf(stderr, "WORKER_ERROR id=%s element=%s %s\n",
                     r.first_id.c_str(), r.element.c_str(), r.message.c_str());
        return 1;
    }
    std::string report = format_report(data);
    std::fwrite(report.data(), 1, report.size(), stdout);
    std::fflush(stdout);
    return 0;
}

int check_main(const std::string &dir, const std::string &self_exe) {
    Plan plan;
    std::string error;
    if (!load_plan(dir + "/case.plan", &plan, &error)) {
        std::fprintf(stderr, "INVALID first_id=plan element=read message=%s\n", error.c_str());
        return 2;
    }
    ProcResult proc = run_worker_process(self_exe, dir);
    VerifyResult r = evaluate_worker_result(plan, proc);
    if (!r.ok) {
        std::printf("INVALID first_id=%s element=%s message=%s spawn=%d exit=%d signaled=%d\n",
                    r.first_id.c_str(), r.element.c_str(), r.message.c_str(),
                    proc.spawn_ok ? 1 : 0, proc.exit_code, proc.signaled ? 1 : 0);
        std::fflush(stdout);
        return 1;
    }
    std::printf("VALID\n");
    return 0;
}

int reduce_command(int argc, char **argv, const std::string &self_exe) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: reduce IN OUT --keep shared-strided --budget N [--resume]\n");
        return 2;
    }
    ReduceOptions opt;
    opt.in_dir = argv[2];
    opt.out_dir = argv[3];
    opt.keep = KEEP_SHARED_STRIDED;
    opt.budget = 0;
    opt.resume = false;
    for (int i = 4; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--keep" && i + 1 < argc) {
            opt.keep = argv[++i];
        } else if (a == "--budget" && i + 1 < argc) {
            opt.budget = std::strtoull(argv[++i], NULL, 10);
        } else if (a == "--resume") {
            opt.resume = true;
        } else {
            std::fprintf(stderr, "unknown reduce argument: %s\n", a.c_str());
            return 2;
        }
    }
    if (opt.keep != KEEP_SHARED_STRIDED || opt.budget == 0) {
        std::fprintf(stderr, "reduce requires --keep shared-strided and --budget N>0\n");
        return 2;
    }
    (void)self_exe; /* Candidate verification uses the real in-process C API. */
    ReduceOutcome out = reduce_run(opt);
    std::printf("%s observed=%llu accepted=%llu next_index=%zu/%zu\n",
                out.message.c_str(),
                static_cast<unsigned long long>(out.observed),
                static_cast<unsigned long long>(out.accepted),
                out.next_index, out.total_candidates);
    if (out.return_code != 0) std::fprintf(stderr, "%s\n", out.message.c_str());
    return out.return_code;
}

} // namespace cgltf_structured

extern "C" int cgltf_defense_main(int argc, char **argv) {
    using namespace cgltf_structured;
    try {
        if (argc < 2 || !argv || !argv[0]) {
            std::fprintf(stderr, "cgltf_defense_main requires argv including argv[0]\n");
            return 2;
        }
        std::string self_exe = argv[0];
        std::string command = argv[1];
        if (command == "generate") {
            if (argc != 4) {
                std::fprintf(stderr, "usage: %s generate SEED DIR\n", argv[0]);
                return 2;
            }
            errno = 0;
            const char *seed_text = argv[2];
            bool decimal = seed_text[0] != '\0';
            for (const char *q = seed_text; decimal && *q; ++q)
                decimal = *q >= '0' && *q <= '9';
            char *end = NULL;
            unsigned long long seed = decimal ? std::strtoull(seed_text, &end, 10) : 0;
            if (!decimal || errno == ERANGE || !end || *end != '\0') {
                std::fprintf(stderr, "SEED must be decimal uint64\n");
                return 2;
            }
            std::string error;
            if (!generate_to_dir(static_cast<std::uint64_t>(seed), argv[3], &error)) {
                std::fprintf(stderr, "generate failed: %s\n", error.c_str());
                return 1;
            }
            std::printf("GENERATED seed=%llu dir=%s\n", seed, argv[3]);
            return 0;
        }
        if (command == "worker") {
            if (argc != 3) {
                std::fprintf(stderr, "usage: %s worker DIR\n", argv[0]);
                return 2;
            }
            return worker_main(argv[2]);
        }
        if (command == "check") {
            if (argc != 3) {
                std::fprintf(stderr, "usage: %s check DIR\n", argv[0]);
                return 2;
            }
            return check_main(argv[2], self_exe);
        }
        if (command == "reduce") {
            return reduce_command(argc, argv, self_exe);
        }
        std::fprintf(stderr, "unknown command: %s\n", command.c_str());
        return 2;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "structured defense exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "structured defense unknown exception\n");
        return 1;
    }
}
