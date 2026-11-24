#include "fuzz/structured/driver.h"

#include <cmath>
#include <cstring>
#include <sstream>

namespace cgltf_structured {

namespace {

void write_f32(std::vector<unsigned char> *buffer, std::size_t at, float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    if (at + 4 <= buffer->size()) {
        (*buffer)[at + 0] = static_cast<unsigned char>(bits & 0xff);
        (*buffer)[at + 1] = static_cast<unsigned char>((bits >> 8) & 0xff);
        (*buffer)[at + 2] = static_cast<unsigned char>((bits >> 16) & 0xff);
        (*buffer)[at + 3] = static_cast<unsigned char>((bits >> 24) & 0xff);
    }
}

std::string json_float(float x) {
    if (!std::isfinite(x)) x = 0.0f;
    if (x == 0.0f) x = 0.0f;
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(6);
    out << static_cast<double>(x);
    return out.str();
}

std::string json_index_list(const std::vector<std::string> &ids, const Plan &p,
                            int (*finder)(const Plan &, const std::string &)) {
    std::string out = "[";
    bool first = true;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        int idx = finder(p, ids[i]);
        if (idx < 0) continue;
        if (!first) out += ",";
        first = false;
        out += std::to_string(idx);
    }
    out += "]";
    return out;
}

int node_finder(const Plan &p, const std::string &id) { return find_node(p, id); }

} // namespace

std::string base64_encode(const std::vector<unsigned char> &data) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        unsigned int n = (static_cast<unsigned int>(data[i]) << 16) |
                         (static_cast<unsigned int>(data[i + 1]) << 8) |
                         static_cast<unsigned int>(data[i + 2]);
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(table[(n >> 6) & 63]);
        out.push_back(table[n & 63]);
        i += 3;
    }
    if (i < data.size()) {
        unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<unsigned int>(data[i + 1]) << 8;
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? table[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

void sync_index_values(Plan *p) {
    if (!p) return;

    std::vector<std::string> referenced_index_ids;
    for (std::size_t pi = 0; pi < p->prims.size(); ++pi) {
        const std::string &id = p->prims[pi].indices;
        if (std::find(referenced_index_ids.begin(), referenced_index_ids.end(), id) ==
            referenced_index_ids.end())
            referenced_index_ids.push_back(id);
    }

    for (std::size_t ai = 0; ai < p->accessors.size(); ++ai) {
        AccessorModel &a = p->accessors[ai];
        if (a.role != "INDEX") continue;
        bool referenced =
            std::find(referenced_index_ids.begin(), referenced_index_ids.end(), a.id) !=
            referenced_index_ids.end();
        /* Unreferenced resources retain their independent last numeric model.
         * Referenced streams are rebuilt below from stable triangle refs. */
        if (referenced) a.index_values.clear();
    }

    for (std::size_t pi = 0; pi < p->prims.size(); ++pi) {
        PrimModel &pr = p->prims[pi];
        int ii = find_accessor(*p, pr.indices);
        int pos_i = find_accessor(*p, pr.position);
        if (ii < 0 || pos_i < 0) continue;
        AccessorModel &idx = p->accessors[ii];
        const AccessorModel &pos = p->accessors[pos_i];

        std::map<std::string, std::uint16_t> compact;
        for (std::size_t vi = 0; vi < pos.vertices.size(); ++vi) {
            if (pos.vertices[vi].id.empty()) continue;
            compact[pos.vertices[vi].id] = static_cast<std::uint16_t>(vi);
        }

        for (std::size_t ti = 0; ti < pr.triangles.size(); ++ti) {
            for (int k = 0; k < 3; ++k) {
                std::map<std::string, std::uint16_t>::const_iterator it =
                    compact.find(pr.triangles[ti].v[k]);
                idx.index_values.push_back(it == compact.end() ? 0 : it->second);
            }
        }
    }
}

void recompute_bounds(const AccessorModel &a, float *out_min, float *out_max,
                      bool *has_bounds) {
    if (!out_min || !out_max || !has_bounds) return;
    *has_bounds = false;
    if (a.role != "POSITION" || a.vertices.empty()) return;
    float mn[3] = {a.vertices[0].x, a.vertices[0].y, a.vertices[0].z};
    float mx[3] = {a.vertices[0].x, a.vertices[0].y, a.vertices[0].z};
    for (std::size_t i = 1; i < a.vertices.size(); ++i) {
        mn[0] = std::min(mn[0], a.vertices[i].x);
        mn[1] = std::min(mn[1], a.vertices[i].y);
        mn[2] = std::min(mn[2], a.vertices[i].z);
        mx[0] = std::max(mx[0], a.vertices[i].x);
        mx[1] = std::max(mx[1], a.vertices[i].y);
        mx[2] = std::max(mx[2], a.vertices[i].z);
    }
    for (int i = 0; i < 3; ++i) {
        out_min[i] = mn[i];
        out_max[i] = mx[i];
    }
    *has_bounds = true;
}

void rebuild_layout(Plan *p) {
    if (!p) return;
    sync_index_values(p);

    /* Compute compact byte lengths for every view from its accessors. */
    for (std::size_t vi = 0; vi < p->views.size(); ++vi) {
        ViewModel &view = p->views[vi];
        std::size_t extent = 0;
        bool used = false;
        for (std::size_t ai = 0; ai < p->accessors.size(); ++ai) {
            const AccessorModel &a = p->accessors[ai];
            if (a.view != view.id) continue;
            used = true;
            std::size_t natural = a.element_size();
            std::size_t stride = view.stride ? view.stride : natural;
            std::size_t count = a.count();
            std::size_t need;
            if (count == 0) {
                need = a.offset + natural;
            } else {
                need = a.offset + stride * (count - 1) + natural;
            }
            extent = std::max(extent, need);
        }
        if (used) view.size = extent;
        if (view.size == 0) view.size = 4; /* legal non-empty bufferView */
    }

    /* Pack views tightly, preserving their compact array order. */
    for (std::size_t bi = 0; bi < p->buffers.size(); ++bi) {
        BufferModel &buffer = p->buffers[bi];
        std::size_t cursor = 0;
        bool has_view = false;
        for (std::size_t vi = 0; vi < p->views.size(); ++vi) {
            ViewModel &view = p->views[vi];
            if (view.buffer != buffer.id) continue;
            has_view = true;
            cursor = (cursor + 3u) & ~static_cast<std::size_t>(3u);
            view.offset = cursor;
            cursor += view.size;
        }
        if (has_view) buffer.bytes.assign(cursor, 0);
        /* A buffer without views is an unused resource.  Keep its semantic
         * marker bytes rather than inventing cross-object layout state. */
    }

    /* Encode all accessor values. */
    for (std::size_t ai = 0; ai < p->accessors.size(); ++ai) {
        const AccessorModel &a = p->accessors[ai];
        int vi = find_view(*p, a.view);
        if (vi < 0) continue;
        ViewModel &view = p->views[vi];
        int bi = find_buffer(*p, view.buffer);
        if (bi < 0) continue;
        BufferModel &buffer = p->buffers[bi];
        std::size_t base = view.offset + a.offset;
        std::size_t natural = a.element_size();
        std::size_t stride = view.stride ? view.stride : natural;

        if (a.role == "POSITION" && a.type == "VEC3" && a.component_type == 5126) {
            for (std::size_t i = 0; i < a.vertices.size(); ++i) {
                std::size_t at = base + stride * i;
                write_f32(&buffer.bytes, at + 0, a.vertices[i].x);
                write_f32(&buffer.bytes, at + 4, a.vertices[i].y);
                write_f32(&buffer.bytes, at + 8, a.vertices[i].z);
            }
        } else if (a.role == "INDEX" && a.component_type == 5123) {
            for (std::size_t i = 0; i < a.index_values.size(); ++i) {
                std::size_t at = base + 2 * i;
                if (at + 2 <= buffer.bytes.size()) {
                    buffer.bytes[at] = static_cast<unsigned char>(a.index_values[i] & 0xff);
                    buffer.bytes[at + 1] = static_cast<unsigned char>((a.index_values[i] >> 8) & 0xff);
                }
            }
        }
    }
}

std::string serialize_gltf(const Plan &p) {
    std::ostringstream j;
    j << "{\n";
    j << "  \"asset\": {\"version\": \"2.0\", \"generator\": \"cgltf-structured-defense\"},\n";
    j << "  \"scene\": 0,\n";

    j << "  \"scenes\": [{\"name\": \"" << p.scene.id << "\", \"nodes\": "
      << json_index_list(p.scene.roots, p, node_finder) << "}],\n";

    j << "  \"nodes\": [";
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        if (i) j << ",";
        const NodeModel &n = p.nodes[i];
        j << "{\"name\":\"" << n.id << "\"";
        if (!n.mesh.empty()) {
            int mi = find_mesh(p, n.mesh);
            if (mi >= 0) j << ",\"mesh\":" << mi;
        }
        if (!n.children.empty()) {
            j << ",\"children\":" << json_index_list(n.children, p, node_finder);
        }
        j << "}";
    }
    j << "],\n";

    j << "  \"meshes\": [";
    bool first_mesh = true;
    for (std::size_t mi = 0; mi < p.meshes.size(); ++mi) {
        if (!first_mesh) j << ",";
        first_mesh = false;
        j << "{\"name\":\"" << p.meshes[mi].id << "\",\"primitives\":[";
        bool first_prim = true;
        for (std::size_t pi = 0; pi < p.prims.size(); ++pi) {
            const PrimModel &pr = p.prims[pi];
            if (pr.mesh != p.meshes[mi].id) continue;
            if (!first_prim) j << ",";
            first_prim = false;
            int pos = find_accessor(p, pr.position);
            int idx = find_accessor(p, pr.indices);
            j << "{\"mode\":" << pr.mode;
            if (idx >= 0) j << ",\"indices\":" << idx;
            if (pos >= 0) j << ",\"attributes\":{\"POSITION\":" << pos << "}";
            j << "}";
        }
        j << "]}";
    }
    j << "],\n";

    j << "  \"accessors\": [";
    for (std::size_t i = 0; i < p.accessors.size(); ++i) {
        if (i) j << ",";
        const AccessorModel &a = p.accessors[i];
        int vi = find_view(p, a.view);
        j << "{\"name\":\"" << a.id << "\"";
        if (vi >= 0) j << ",\"bufferView\":" << vi;
        j << ",\"componentType\":" << a.component_type
          << ",\"count\":" << a.count()
          << ",\"type\":\"" << a.type << "\"";
        if (a.offset) j << ",\"byteOffset\":" << a.offset;
        float mn[3], mx[3];
        bool has = false;
        recompute_bounds(a, mn, mx, &has);
        if (has) {
            j << ",\"min\":[" << json_float(mn[0]) << "," << json_float(mn[1])
              << "," << json_float(mn[2]) << "]";
            j << ",\"max\":[" << json_float(mx[0]) << "," << json_float(mx[1])
              << "," << json_float(mx[2]) << "]";
        }
        if (a.role == "INDEX" && !a.index_values.empty()) {
            std::uint16_t lo = a.index_values[0], hi = lo;
            for (std::size_t k = 1; k < a.index_values.size(); ++k) {
                lo = std::min(lo, a.index_values[k]);
                hi = std::max(hi, a.index_values[k]);
            }
            j << ",\"min\":[" << lo << "],\"max\":[" << hi << "]";
        }
        j << "}";
    }
    j << "],\n";

    j << "  \"bufferViews\": [";
    for (std::size_t i = 0; i < p.views.size(); ++i) {
        if (i) j << ",";
        const ViewModel &v = p.views[i];
        int bi = find_buffer(p, v.buffer);
        j << "{\"name\":\"" << v.id << "\"";
        if (bi >= 0) j << ",\"buffer\":" << bi;
        j << ",\"byteOffset\":" << v.offset
          << ",\"byteLength\":" << v.size;
        if (v.stride) j << ",\"byteStride\":" << v.stride;
        if (v.target) j << ",\"target\":" << v.target;
        j << "}";
    }
    j << "],\n";

    j << "  \"buffers\": [";
    for (std::size_t i = 0; i < p.buffers.size(); ++i) {
        if (i) j << ",";
        const BufferModel &b = p.buffers[i];
        std::string b64 = base64_encode(b.bytes);
        j << "{\"name\":\"" << b.id << "\",\"byteLength\":" << b.bytes.size()
          << ",\"uri\":\"data:application/octet-stream;base64," << b64 << "\"}";
    }
    j << "]\n";
    j << "}\n";
    return j.str();
}

bool write_case_files(Plan *plan, const std::string &dir,
                      Metrics *metrics, std::string *error) {
    if (!plan) return false;
    if (!make_directories(dir, error)) return false;
    rebuild_layout(plan);
    std::string gltf = serialize_gltf(*plan);
    if (!write_file_atomic(dir + "/case.gltf", gltf, error)) return false;
    if (!save_plan(*plan, dir + "/case.plan", error)) return false;
    if (metrics) *metrics = compute_metrics(*plan, gltf.size());
    return true;
}

} // namespace cgltf_structured
