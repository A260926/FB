#include "fuzz/structured/driver.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace cgltf_structured {

namespace {

struct IdFactory {
    std::map<std::string, int> counts;
    std::string next(const std::string &prefix) {
        int n = ++counts[prefix];
        std::ostringstream out;
        out << prefix << '-' << std::setw(4) << std::setfill('0') << n;
        return out.str();
    }
};

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed ? seed : 0x9e3779b97f4a7c15ULL) {}
    std::uint64_t next_u64() {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    int range(int lo, int hi) {
        if (hi <= lo) return lo;
        return lo + static_cast<int>(next_u64() % static_cast<std::uint64_t>(hi - lo + 1));
    }
    float coord() {
        static const int den = 4;
        return static_cast<float>(range(-6, 6)) +
               static_cast<float>(range(0, den - 1)) / static_cast<float>(den);
    }
};

std::string trim(const std::string &s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(s[i]);
        }
    }
    out.push_back(cur);
    return out;
}

std::string csv(const std::vector<std::string> &items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += ",";
        out += items[i];
    }
    return out;
}

std::vector<std::string> split_csv(const std::string &s) {
    if (s.empty() || s == "-") return std::vector<std::string>();
    return split(s, ',');
}

std::string f6(float x) {
    if (x == 0.0f) x = 0.0f; /* collapse -0 */
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << static_cast<double>(x);
    return out.str();
}

float parse_f(const std::string &s) {
    return static_cast<float>(std::strtod(s.c_str(), NULL));
}

std::size_t parse_size(const std::string &s) {
    return static_cast<std::size_t>(std::strtoull(s.c_str(), NULL, 10));
}

std::uint64_t parse_u64(const std::string &s) {
    return std::strtoull(s.c_str(), NULL, 10);
}

std::string kv_get(const std::map<std::string, std::string> &kv,
                   const std::string &key, const std::string &def = "") {
    std::map<std::string, std::string>::const_iterator it = kv.find(key);
    return it == kv.end() ? def : it->second;
}

std::map<std::string, std::string> parse_kv(const std::string &line) {
    std::map<std::string, std::string> kv;
    std::size_t first_space = line.find(' ');
    std::string rest = first_space == std::string::npos ? "" : line.substr(first_space + 1);
    std::vector<std::string> toks = split(rest, ' ');
    for (std::size_t i = 0; i < toks.size(); ++i) {
        std::size_t eq = toks[i].find('=');
        if (eq == std::string::npos) continue;
        kv[toks[i].substr(0, eq)] = toks[i].substr(eq + 1);
    }
    return kv;
}

void collect_node_subtree(const Plan &p, const std::string &id,
                          std::vector<std::string> *out) {
    if (std::find(out->begin(), out->end(), id) != out->end()) return;
    int ni = find_node(p, id);
    if (ni < 0) return;
    out->push_back(id);
    for (std::size_t i = 0; i < p.nodes[ni].children.size(); ++i)
        collect_node_subtree(p, p.nodes[ni].children[i], out);
}

} // namespace

bool read_file(const std::string &path, std::string *out, std::string *error) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        if (error) *error = "cannot read " + path + ": " + std::strerror(errno);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in.good() && !in.eof()) {
        if (error) *error = "failed reading " + path;
        return false;
    }
    if (out) *out = ss.str();
    return true;
}

bool write_file_atomic(const std::string &path, const std::string &data,
                       std::string *error) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + tmp + ": " + std::strerror(errno);
            return false;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            if (error) *error = "short write " + tmp;
            return false;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (error) *error = "rename " + tmp + " -> " + path + ": " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

bool make_directories(const std::string &path, std::string *error) {
    if (path.empty()) return true;
    struct stat st;
    if (::stat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) return true;
        if (error) *error = path + " exists and is not a directory";
        return false;
    }
    std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) {
        std::string parent = path.substr(0, slash);
        if (!parent.empty() && !make_directories(parent, error)) return false;
    }
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        if (error) *error = "mkdir " + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

std::string temp_directory(const std::string &prefix) {
    const char *base = std::getenv("TMPDIR");
    std::string dir = base && *base ? base : "/tmp";
    std::string pat = dir + "/" + prefix + "-XXXXXX";
    std::vector<char> buf(pat.begin(), pat.end());
    buf.push_back('\0');
    char *made = ::mkdtemp(buf.data());
    return made ? std::string(made) : std::string();
}

int find_buffer(const Plan &p, const std::string &id) {
    for (std::size_t i = 0; i < p.buffers.size(); ++i) if (p.buffers[i].id == id) return static_cast<int>(i);
    return -1;
}
int find_view(const Plan &p, const std::string &id) {
    for (std::size_t i = 0; i < p.views.size(); ++i) if (p.views[i].id == id) return static_cast<int>(i);
    return -1;
}
int find_accessor(const Plan &p, const std::string &id) {
    for (std::size_t i = 0; i < p.accessors.size(); ++i) if (p.accessors[i].id == id) return static_cast<int>(i);
    return -1;
}
int find_prim(const Plan &p, const std::string &id) {
    for (std::size_t i = 0; i < p.prims.size(); ++i) if (p.prims[i].id == id) return static_cast<int>(i);
    return -1;
}
int find_mesh(const Plan &p, const std::string &id) {
    for (std::size_t i = 0; i < p.meshes.size(); ++i) if (p.meshes[i].id == id) return static_cast<int>(i);
    return -1;
}
int find_node(const Plan &p, const std::string &id) {
    for (std::size_t i = 0; i < p.nodes.size(); ++i) if (p.nodes[i].id == id) return static_cast<int>(i);
    return -1;
}

Metrics compute_metrics(const Plan &p, std::size_t text_bytes) {
    Metrics m;
    m.objects = p.buffers.size() + p.views.size() + p.accessors.size() +
                p.prims.size() + p.meshes.size() + p.nodes.size() + 1;
    m.text_bytes = text_bytes;
    for (std::size_t i = 0; i < p.prims.size(); ++i)
        m.triangles += p.prims[i].triangles.size();
    for (std::size_t i = 0; i < p.accessors.size(); ++i) {
        if (p.accessors[i].role == "POSITION")
            m.vertices += p.accessors[i].vertices.size();
    }
    for (std::size_t i = 0; i < p.buffers.size(); ++i)
        m.buffer_bytes += p.buffers[i].bytes.size();
    return m;
}

std::vector<std::string> scene_closure(const Plan &p) {
    std::vector<std::string> alive;
    alive.push_back(p.scene.id);
    std::vector<std::string> nodes;
    for (std::size_t i = 0; i < p.scene.roots.size(); ++i)
        collect_node_subtree(p, p.scene.roots[i], &nodes);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (std::find(alive.begin(), alive.end(), nodes[i]) == alive.end())
            alive.push_back(nodes[i]);
        int ni = find_node(p, nodes[i]);
        if (ni < 0 || p.nodes[ni].mesh.empty() || p.nodes[ni].mesh == "-") continue;
        std::string mesh_id = p.nodes[ni].mesh;
        if (std::find(alive.begin(), alive.end(), mesh_id) == alive.end()) alive.push_back(mesh_id);
        int mi = find_mesh(p, mesh_id);
        if (mi < 0) continue;
        for (std::size_t pi = 0; pi < p.prims.size(); ++pi) {
            if (p.prims[pi].mesh != mesh_id) continue;
            const PrimModel &pr = p.prims[pi];
            if (std::find(alive.begin(), alive.end(), pr.id) == alive.end()) alive.push_back(pr.id);
            int ai = find_accessor(p, pr.position);
            if (ai >= 0 && std::find(alive.begin(), alive.end(), pr.position) == alive.end()) {
                alive.push_back(pr.position);
            }
            ai = find_accessor(p, pr.indices);
            if (ai >= 0 && std::find(alive.begin(), alive.end(), pr.indices) == alive.end()) {
                alive.push_back(pr.indices);
            }
        }
    }
    /* Expand accessor -> view -> buffer. */
    std::size_t cursor = 0;
    while (cursor < alive.size()) {
        std::string id = alive[cursor++];
        int ai = find_accessor(p, id);
        if (ai >= 0) {
            std::string v = p.accessors[ai].view;
            int vi = find_view(p, v);
            if (vi >= 0 && std::find(alive.begin(), alive.end(), v) == alive.end()) alive.push_back(v);
            continue;
        }
        int vi = find_view(p, id);
        if (vi >= 0) {
            std::string b = p.views[vi].buffer;
            if (find_buffer(p, b) >= 0 && std::find(alive.begin(), alive.end(), b) == alive.end()) alive.push_back(b);
        }
    }
    return alive;
}

Witness find_witness(const Plan &p) {
    Witness w;
    std::vector<std::string> alive = scene_closure(p);
    for (std::size_t ai = 0; ai < p.accessors.size(); ++ai) {
        const AccessorModel &a = p.accessors[ai];
        if (a.role != "POSITION" || a.offset == 0) continue;
        std::size_t stride = a.explicit_stride;
        int vi = find_view(p, a.view);
        if (vi >= 0 && p.views[vi].stride) stride = p.views[vi].stride;
        if (stride <= 12) continue;
        std::vector<std::string> users;
        for (std::size_t pi = 0; pi < p.prims.size(); ++pi) {
            const PrimModel &pr = p.prims[pi];
            if (pr.position != a.id) continue;
            if (std::find(alive.begin(), alive.end(), pr.id) == alive.end()) continue;
            users.push_back(pr.id);
        }
        if (users.size() >= 2) {
            w.found = true;
            w.accessor_id = a.id;
            w.first_prim = users[0];
            w.second_prim = users[1];
            w.offset = a.offset;
            w.stride = stride;
            return w;
        }
    }
    return w;
}

bool has_required_keep(const Plan &p, const std::string &keep,
                       std::string *reason) {
    if (keep != KEEP_SHARED_STRIDED) {
        if (reason) *reason = "unsupported keep constraint: " + keep;
        return false;
    }
    Witness w = find_witness(p);
    if (!w.found) {
        if (reason) *reason = "shared-strided witness is missing";
        return false;
    }
    return true;
}

std::string plan_to_string(const Plan &p) {
    std::ostringstream out;
    out << "CGLTF-STRUCTURED-PLAN " << FORMAT_VERSION << "\n";
    out << "SEED " << p.seed << "\n";
    out << "SCENE id=" << p.scene.id << " roots=" << csv(p.scene.roots) << "\n";

    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        const NodeModel &n = p.nodes[i];
        out << "NODE id=" << n.id
            << " mesh=" << (n.mesh.empty() ? "-" : n.mesh)
            << " children=" << csv(n.children) << "\n";
    }
    for (std::size_t i = 0; i < p.meshes.size(); ++i)
        out << "MESH id=" << p.meshes[i].id << "\n";
    for (std::size_t i = 0; i < p.prims.size(); ++i) {
        const PrimModel &pr = p.prims[i];
        out << "PRIM id=" << pr.id << " mesh=" << pr.mesh
            << " mode=" << pr.mode << " indices=" << pr.indices
            << " position=" << pr.position << "\n";
        for (std::size_t ti = 0; ti < pr.triangles.size(); ++ti) {
            const Triangle &t = pr.triangles[ti];
            out << "TRI prim=" << pr.id << " id=" << t.id
                << " a=" << t.v[0] << " b=" << t.v[1] << " c=" << t.v[2] << "\n";
        }
    }
    for (std::size_t i = 0; i < p.accessors.size(); ++i) {
        const AccessorModel &a = p.accessors[i];
        out << "ACCESSOR id=" << a.id << " role=" << a.role
            << " componentType=" << a.component_type << " type=" << a.type
            << " view=" << a.view << " offset=" << a.offset
            << " explicitStride=" << a.explicit_stride << "\n";
        if (a.role == "POSITION") {
            for (std::size_t vi = 0; vi < a.vertices.size(); ++vi) {
                const Vertex &v = a.vertices[vi];
                out << "VERTEX accessor=" << a.id << " id=" << v.id
                    << " x=" << f6(v.x) << " y=" << f6(v.y)
                    << " z=" << f6(v.z) << "\n";
            }
        } else {
            out << "INDEXES accessor=" << a.id << " values=";
            for (std::size_t vi = 0; vi < a.index_values.size(); ++vi) {
                if (vi) out << ",";
                out << a.index_values[vi];
            }
            out << "\n";
        }
    }
    for (std::size_t i = 0; i < p.views.size(); ++i) {
        const ViewModel &v = p.views[i];
        out << "VIEW id=" << v.id << " buffer=" << v.buffer
            << " offset=" << v.offset << " size=" << v.size
            << " stride=" << v.stride << " target=" << v.target << "\n";
    }
    for (std::size_t i = 0; i < p.buffers.size(); ++i) {
        const BufferModel &b = p.buffers[i];
        out << "BUFFER id=" << b.id << " length=" << b.bytes.size() << "\n";
    }
    return out.str();
}

bool plan_from_string(const std::string &text, Plan *plan,
                      std::string *error) {
    if (!plan) return false;
    Plan p;
    std::vector<std::string> lines = split(text, '\n');
    bool header_ok = false;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        std::string line = trim(lines[li]);
        if (line.empty() || line[0] == '#') continue;
        std::string tag = line.substr(0, line.find(' '));
        if (tag == "CGLTF-STRUCTURED-PLAN") {
            header_ok = line.find(" 1") != std::string::npos;
            continue;
        }
        if (tag == "SEED") {
            p.seed = parse_u64(trim(line.substr(5)));
            continue;
        }
        std::map<std::string, std::string> kv = parse_kv(line);
        if (tag == "SCENE") {
            p.scene.id = kv_get(kv, "id", "scene-0001");
            p.scene.roots = split_csv(kv_get(kv, "roots"));
        } else if (tag == "NODE") {
            NodeModel n;
            n.id = kv_get(kv, "id");
            n.mesh = kv_get(kv, "mesh");
            if (n.mesh == "-") n.mesh.clear();
            n.children = split_csv(kv_get(kv, "children"));
            p.nodes.push_back(n);
        } else if (tag == "MESH") {
            MeshModel m;
            m.id = kv_get(kv, "id");
            p.meshes.push_back(m);
        } else if (tag == "PRIM") {
            PrimModel pr;
            pr.id = kv_get(kv, "id");
            pr.mesh = kv_get(kv, "mesh");
            pr.mode = static_cast<int>(parse_size(kv_get(kv, "mode", "4")));
            pr.indices = kv_get(kv, "indices");
            pr.position = kv_get(kv, "position");
            p.prims.push_back(pr);
        } else if (tag == "TRI") {
            std::string prim_id = kv_get(kv, "prim");
            int pi = find_prim(p, prim_id);
            if (pi < 0) {
                if (error) *error = "TRI references unknown prim " + prim_id;
                return false;
            }
            Triangle t(kv_get(kv, "id"), kv_get(kv, "a"), kv_get(kv, "b"), kv_get(kv, "c"));
            p.prims[pi].triangles.push_back(t);
        } else if (tag == "ACCESSOR") {
            AccessorModel a;
            a.id = kv_get(kv, "id");
            a.role = kv_get(kv, "role");
            a.component_type = static_cast<int>(parse_size(kv_get(kv, "componentType", "5126")));
            a.type = kv_get(kv, "type");
            a.view = kv_get(kv, "view");
            a.offset = parse_size(kv_get(kv, "offset", "0"));
            a.explicit_stride = parse_size(kv_get(kv, "explicitStride", "0"));
            p.accessors.push_back(a);
        } else if (tag == "VERTEX") {
            std::string aid = kv_get(kv, "accessor");
            int ai = find_accessor(p, aid);
            if (ai < 0) {
                if (error) *error = "VERTEX references unknown accessor " + aid;
                return false;
            }
            Vertex v(kv_get(kv, "id"), parse_f(kv_get(kv, "x", "0")),
                     parse_f(kv_get(kv, "y", "0")), parse_f(kv_get(kv, "z", "0")));
            p.accessors[ai].vertices.push_back(v);
        } else if (tag == "INDEXES") {
            std::string aid = kv_get(kv, "accessor");
            int ai = find_accessor(p, aid);
            if (ai < 0) {
                if (error) *error = "INDEXES references unknown accessor " + aid;
                return false;
            }
            std::vector<std::string> vals = split_csv(kv_get(kv, "values"));
            for (std::size_t i = 0; i < vals.size(); ++i)
                p.accessors[ai].index_values.push_back(
                    static_cast<std::uint16_t>(parse_size(vals[i])));
        } else if (tag == "VIEW") {
            ViewModel v;
            v.id = kv_get(kv, "id");
            v.buffer = kv_get(kv, "buffer");
            v.offset = parse_size(kv_get(kv, "offset", "0"));
            v.size = parse_size(kv_get(kv, "size", "0"));
            v.stride = parse_size(kv_get(kv, "stride", "0"));
            v.target = static_cast<int>(parse_size(kv_get(kv, "target", "0")));
            p.views.push_back(v);
        } else if (tag == "BUFFER") {
            BufferModel b;
            b.id = kv_get(kv, "id");
            std::size_t len = parse_size(kv_get(kv, "length", "0"));
            /* Plan deliberately stores identity and semantic values, not bytes.
             * A deterministic marker is enough for an unreferenced buffer. */
            b.bytes.assign(len, static_cast<unsigned char>(0x5a));
            p.buffers.push_back(b);
        } else {
            /* Forward-compatible comments/records are ignored. */
        }
    }
    if (!header_ok) {
        if (error) *error = "missing CGLTF-STRUCTURED-PLAN header";
        return false;
    }
    sync_index_values(&p);
    *plan = p;
    return true;
}

bool save_plan(const Plan &plan, const std::string &path, std::string *error) {
    return write_file_atomic(path, plan_to_string(plan), error);
}

bool load_plan(const std::string &path, Plan *plan, std::string *error) {
    std::string text;
    if (!read_file(path, &text, error)) return false;
    return plan_from_string(text, plan, error);
}

bool generate_to_dir(std::uint64_t seed, const std::string &dir,
                     std::string *error) {
    if (!make_directories(dir, error)) return false;
    Plan p = generate_plan(seed);
    Metrics m;
    if (!write_case_files(&p, dir, &m, error)) return false;
    return true;
}

Plan generate_plan(std::uint64_t seed) {
    Plan p;
    p.seed = seed;
    Rng rng(seed);
    IdFactory ids;

    const std::string bmain = ids.next("buf");
    const std::string bextra = ids.next("buf");
    p.buffers.push_back(BufferModel());
    p.buffers[0].id = bmain;
    p.buffers[0].bytes.resize(64, 0);
    p.buffers.push_back(BufferModel());
    p.buffers[1].id = bextra;
    static const char marker[] = "UNUSED-BUFFER-MARKER";
    p.buffers[1].bytes.assign(marker, marker + sizeof(marker) - 1);

    auto add_view = [&](const std::string &buffer, std::size_t stride, int target) {
        ViewModel v;
        v.id = ids.next("view");
        v.buffer = buffer;
        v.stride = stride;
        v.target = target;
        p.views.push_back(v);
        return v.id;
    };

    const std::string v_inter = add_view(bmain, 24, 34962);
    const std::string v_packed = add_view(bmain, 12, 34962);
    const std::string v_outpos = add_view(bmain, 12, 34962);
    const std::string v_unused = add_view(bextra, 12, 34962);

    auto add_pos = [&](const std::string &view, std::size_t offset,
                       std::size_t stride, std::size_t count) {
        AccessorModel a;
        a.id = ids.next("acc");
        a.role = "POSITION";
        a.component_type = 5126;
        a.type = "VEC3";
        a.view = view;
        a.offset = offset;
        a.explicit_stride = stride;
        for (std::size_t i = 0; i < count; ++i) {
            float x = rng.coord(), y = rng.coord(), z = rng.coord();
            a.vertices.push_back(Vertex(ids.next("vert"), x, y, z));
        }
        p.accessors.push_back(a);
        return a.id;
    };

    /* Interleaved record: [companion vec3 @0][shared POSITION vec3 @12]. */
    const std::string a_companion = add_pos(v_inter, 0, 24, 6);
    const std::string a_shared = add_pos(v_inter, 12, 24, 6);
    const std::string a_packed = add_pos(v_packed, 0, 12, 4);
    const std::string a_outpos = add_pos(v_outpos, 0, 12, 3);
    const std::string a_unused = add_pos(v_unused, 0, 12, 3);

    auto add_index_accessor = [&]() {
        AccessorModel a;
        a.id = ids.next("acc");
        a.role = "INDEX";
        a.component_type = 5123;
        a.type = "SCALAR";
        std::string v = add_view(bmain, 0, 34963);
        a.view = v;
        a.offset = 0;
        p.accessors.push_back(a);
        return a.id;
    };

    const std::string i_w1 = add_index_accessor();
    const std::string i_w2 = add_index_accessor();
    const std::string i_comp = add_index_accessor();
    const std::string i_packed = add_index_accessor();
    const std::string i_out = add_index_accessor();
    const std::string i_orphan = add_index_accessor();

    auto tri = [&](const AccessorModel &pos, std::size_t x, std::size_t y,
                   std::size_t z) {
        return Triangle(ids.next("tri"), pos.vertices[x].id,
                        pos.vertices[y].id, pos.vertices[z].id);
    };
    const AccessorModel &shared = p.accessors[find_accessor(p, a_shared)];
    const AccessorModel &companion = p.accessors[find_accessor(p, a_companion)];
    const AccessorModel &packed = p.accessors[find_accessor(p, a_packed)];
    const AccessorModel &outpos = p.accessors[find_accessor(p, a_outpos)];

    auto add_mesh = [&]() {
        MeshModel m;
        m.id = ids.next("mesh");
        p.meshes.push_back(m);
        return m.id;
    };
    auto add_prim = [&](const std::string &mesh, const std::string &pos,
                        const std::string &idx,
                        const std::vector<Triangle> &tris) {
        PrimModel pr;
        pr.id = ids.next("prim");
        pr.mesh = mesh;
        pr.position = pos;
        pr.indices = idx;
        pr.triangles = tris;
        p.prims.push_back(pr);
        return pr.id;
    };

    const std::string m_w1 = add_mesh();
    const std::string m_w2 = add_mesh();
    const std::string m_extra = add_mesh();
    const std::string m_out = add_mesh();
    const std::string m_orphan = add_mesh();

    add_prim(m_w1, a_shared, i_w1,
             std::vector<Triangle>{tri(shared, 0, 1, 2), tri(shared, 0, 2, 3)});
    add_prim(m_w2, a_shared, i_w2,
             std::vector<Triangle>{tri(shared, 3, 4, 5), tri(shared, 3, 5, 0)});
    add_prim(m_extra, a_companion, i_comp,
             std::vector<Triangle>{tri(companion, 0, 1, 2), tri(companion, 2, 3, 4)});
    add_prim(m_extra, a_packed, i_packed,
             std::vector<Triangle>{tri(packed, 0, 1, 2), tri(packed, 0, 2, 3)});
    add_prim(m_out, a_outpos, i_out,
             std::vector<Triangle>{tri(outpos, 0, 1, 2)});
    add_prim(m_orphan, a_packed, i_orphan,
             std::vector<Triangle>{tri(packed, 1, 2, 3)});

    auto add_node = [&](const std::string &mesh,
                        const std::vector<std::string> &children) {
        NodeModel n;
        n.id = ids.next("node");
        n.mesh = mesh;
        n.children = children;
        p.nodes.push_back(n);
        return n.id;
    };

    /* Create leaves first, then parents. IDs are still stable. */
    const std::string n_w1 = add_node(m_w1, std::vector<std::string>());
    const std::string n_w2 = add_node(m_w2, std::vector<std::string>());
    const std::string n_extra = add_node(m_extra, std::vector<std::string>());
    const std::string n_group = add_node("", std::vector<std::string>{n_w1, n_w2});
    const std::string n_root = add_node("", std::vector<std::string>{n_group, n_extra});
    const std::string n_outchild = add_node(m_out, std::vector<std::string>());
    const std::string n_outroot = add_node("", std::vector<std::string>{n_outchild});

    p.scene.id = ids.next("scene");
    p.scene.roots.push_back(n_root);
    /* n_outroot is deliberately outside p.scene.roots; m_orphan has no node. */

    sync_index_values(&p);
    rebuild_layout(&p);

    /* Hard guarantees for the structured path. */
    if (p.nodes.size() > 64 || p.accessors.size() > 64) {
        std::abort();
    }
    std::size_t buffer_bytes = 0;
    for (std::size_t i = 0; i < p.buffers.size(); ++i) buffer_bytes += p.buffers[i].bytes.size();
    if (buffer_bytes > 64 * 1024) std::abort();
    Witness w = find_witness(p);
    if (!w.found || w.offset == 0) std::abort();
    return p;
}

} // namespace cgltf_structured
