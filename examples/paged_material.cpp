// @file paged_material.cpp
// @brief End-to-end example of paged texture rendering with a capacity-1
//        PagedCache.
//
// The program generates a textured quad mesh and an 8x8 RGBA8 texture split
// into four 4x4 pages, then renders it with a PagedRenderJob whose cache can
// hold only one page at a time.  Pages are generated on demand in C++ (there
// is no disk format or loader) and supplied through PagedCache::supply().
//
// Outputs, written into the directory given as argv[1]:
//   paged.ppm      — the finished image (the job's own pixel buffer)
//   residency.txt  — cache/residency counters and the dense-render comparison
//
// Build (see task_support/run.sh):
//   c++ -std=c++17 -O2 -Isrc/core -Isrc/third_party \
//       src/core/paged_texture.cpp src/core/paged_renderer.cpp \
//       examples/paged_material.cpp -o paged_material

#include <scimesh/paged_renderer.h>
#include <scimesh/paged_texture.h>
#include <scimesh/renderer.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace scimesh;

namespace {

// Deterministic procedural texel: the same function fills the dense reference
// texture and every delivered page, so the two renders can be compared
// byte-for-byte.
void texel(int x, int y, uint8_t &r, uint8_t &g, uint8_t &b, uint8_t &a) {
    r = static_cast<uint8_t>(32 + x * 25);
    g = static_cast<uint8_t>(32 + y * 25);
    b = 160;
    a = 255;  // paged textures must be fully opaque
}

std::vector<uint8_t> make_page_bytes(const PagedCache &cache,
                                     uint64_t version,
                                     int page_x, int page_y) {
    int w = 0, h = 0;
    cache.page_rect(version, page_x, page_y, w, h);
    const TextureDescriptor *desc = cache.descriptor(version);
    std::vector<uint8_t> bytes(static_cast<size_t>(w) * h * 4u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t r, g, b, a;
            texel(page_x * desc->page_size + x,
                  page_y * desc->page_size + y, r, g, b, a);
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4u;
            bytes[idx] = r;
            bytes[idx + 1] = g;
            bytes[idx + 2] = b;
            bytes[idx + 3] = a;
        }
    }
    return bytes;
}

Mesh build_mesh() {
    Mesh mesh;
    mesh.vertices = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
    // Image-space UVs: (0, 0) is the top-left texel, matching Image.
    mesh.uvs = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    mesh.colors.assign(4, Color(1, 1, 1, 1));
    mesh.normals.assign(4, Vec3(0, 0, 1));
    return mesh;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <output-directory>\n";
        return 2;
    }
    const std::string out_dir = argv[1];

    // --- Texture: 8x8 texels, 4x4 pages => a 2x2 page grid. ---------------
    constexpr int kTexSize = 8;
    constexpr int kPageSize = 4;
    constexpr size_t kCapacity = 1;  // the demanding case: one resident page

    PagedCache cache(kCapacity);
    const uint64_t version =
        cache.publish(TextureDescriptor{kTexSize, kTexSize, kPageSize, 42});

    // --- Scene, camera and options (copied into the job). -----------------
    Scene scene;
    scene.add(build_mesh());

    Camera cam;
    cam.eye = Vec3(0, 0, 4);
    cam.center = Vec3(0, 0, 0);
    cam.up = Vec3(0, 1, 0);

    RenderOptions opts;
    opts.width = 48;
    opts.height = 48;
    opts.threads = 1;
    opts.aa_samples = 1;
    opts.background_color = Color(0, 0, 0, 1);
    opts.backface_culling = false;

    PagedRenderJob job(cache, scene, cam, opts,
                       {PagedTextureBinding{0, version}}, /*K=*/4);

    // --- Incremental render: small step budgets, explicit deliveries. -----
    uint64_t rounds = 0;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    size_t peak_resident = 0;
    size_t peak_pending = 0;
    std::set<uint64_t> seen_tickets;
    PagedJobStatus status = PagedJobStatus::READY;

    for (int guard = 0; guard < 100000; ++guard) {
        status = job.step(/*budget=*/3);
        ++rounds;
        peak_resident = std::max(peak_resident, cache.resident_page_count());
        peak_pending = std::max(peak_pending, job.pending_fragment_count());

        if (status == PagedJobStatus::COMPLETE ||
            status == PagedJobStatus::FAILED ||
            status == PagedJobStatus::CANCELLED) {
            break;
        }

        std::vector<PageRequest> requests = job.outstanding_requests();
        // Deliveries are allowed in any order; alternate direction to prove
        // out-of-order completion works.
        if (rounds % 2 == 0) {
            std::reverse(requests.begin(), requests.end());
        }
        for (const PageRequest &req : requests) {
            seen_tickets.insert(req.ticket);
            int page_w = 0, page_h = 0;
            cache.page_rect(req.version, req.page_x, req.page_y,
                            page_w, page_h);
            std::vector<uint8_t> bytes =
                make_page_bytes(cache, req.version, req.page_x, req.page_y);
            PageSupplyOutcome outcome = cache.supply(
                req.ticket, page_w, page_h, bytes.data(), bytes.size());
            if (outcome == PageSupplyOutcome::ACCEPTED) {
                ++accepted;
            } else {
                ++rejected;
            }
        }
    }

    if (status != PagedJobStatus::COMPLETE) {
        std::cerr << "paged render did not complete (status "
                  << static_cast<int>(status) << ")\n";
        return 1;
    }

    // --- Dense reference render with the identical texel function. --------
    Mesh dense_mesh = build_mesh();
    dense_mesh.texture = Image(kTexSize, kTexSize);
    for (int y = 0; y < kTexSize; ++y) {
        for (int x = 0; x < kTexSize; ++x) {
            uint8_t r, g, b, a;
            texel(x, y, r, g, b, a);
            dense_mesh.texture.set_pixel(x, y, r, g, b, a);
        }
    }
    Scene dense_scene;
    dense_scene.add(dense_mesh);
    Image dense = Renderer().render_scene(dense_scene, cam, opts);

    const Image &paged = job.image();
    bool matches_dense =
        paged.width == dense.width && paged.height == dense.height &&
        paged.pixels == dense.pixels;

    // --- Outputs. ----------------------------------------------------------
    const std::string ppm_path = out_dir + "/paged.ppm";
    if (!job.image().write_ppm(ppm_path)) {
        std::cerr << "failed to write " << ppm_path << "\n";
        return 1;
    }

    std::ofstream residency(out_dir + "/residency.txt");
    if (!residency) {
        std::cerr << "failed to open residency.txt\n";
        return 1;
    }
    residency << "capacity=" << cache.capacity() << "\n";
    residency << "texture_width=" << kTexSize << "\n";
    residency << "texture_height=" << kTexSize << "\n";
    residency << "page_size=" << kPageSize << "\n";
    residency << "page_grid=2x2\n";
    residency << "status=COMPLETE\n";
    residency << "step_rounds=" << rounds << "\n";
    residency << "total_raster_steps=" << job.total_steps() << "\n";
    residency << "submissions=" << job.submission_count() << "\n";
    residency << "supplies_accepted=" << accepted << "\n";
    residency << "supplies_rejected=" << rejected << "\n";
    residency << "distinct_request_tickets=" << seen_tickets.size() << "\n";
    residency << "peak_resident_pages=" << peak_resident << "\n";
    residency << "peak_pending_fragments=" << peak_pending << "\n";
    residency << "final_resident_pages=" << cache.resident_page_count() << "\n";
    residency << "matches_dense=" << (matches_dense ? "yes" : "no") << "\n";

    std::cout << "paged render complete: " << paged.width << "x"
              << paged.height << ", " << job.submission_count()
              << " submissions, " << seen_tickets.size()
              << " distinct page requests, matches_dense="
              << (matches_dense ? "yes" : "no") << "\n";

    return matches_dense ? 0 : 1;
}
