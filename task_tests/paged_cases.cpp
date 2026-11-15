// @file paged_cases.cpp
// @brief Task tests for PagedCache and PagedRenderJob.
//
// Registered through task_support/check.h as three groups — residency,
// continuation, lifetime — and driven by task_support/driver.c (which supplies
// main()).  Every case uses TASK_CHECK for its assertions and returns 0 on
// success / 1 on the first failed check.

#include "../task_support/check.h"

#include <scimesh/paged_renderer.h>
#include <scimesh/paged_texture.h>
#include <scimesh/renderer.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace scimesh;

namespace {

using TexelFn = std::function<void(int, int, uint8_t &, uint8_t &,
                                   uint8_t &, uint8_t &)>;

// The procedural texture used by most cases: every alpha is 255.
void checker_texel(int x, int y, uint8_t &r, uint8_t &g, uint8_t &b,
                   uint8_t &a) {
    r = static_cast<uint8_t>(20 + x * 17);
    g = static_cast<uint8_t>(40 + y * 23);
    b = static_cast<uint8_t>((x * 31 + y * 13) & 0xff);
    a = 255;
}

void solid_texel(int x, int y, uint8_t &r, uint8_t &g, uint8_t &b,
                 uint8_t &a) {
    r = static_cast<uint8_t>(100 + (x & 1) * 50);
    g = static_cast<uint8_t>(120 + (y & 1) * 40);
    b = 90;
    a = 255;
}

std::vector<uint8_t> page_bytes(const PagedCache &cache, uint64_t version,
                                int page_x, int page_y, const TexelFn &fn) {
    int w = 0, h = 0;
    cache.page_rect(version, page_x, page_y, w, h);
    const TextureDescriptor *desc = cache.descriptor(version);
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4u);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t r, g, b, a;
            fn(page_x * desc->page_size + x,
               page_y * desc->page_size + y, r, g, b, a);
            const size_t idx = (static_cast<size_t>(y) * w + x) * 4u;
            out[idx] = r;
            out[idx + 1] = g;
            out[idx + 2] = b;
            out[idx + 3] = a;
        }
    }
    return out;
}

// Fulfill every request a job currently advertises; when `reverse` is set the
// deliveries happen in reverse request order.  Returns the number accepted.
int deliver_all(PagedCache &cache, const PagedRenderJob &job,
                const TexelFn &fn, bool reverse = false) {
    std::vector<PageRequest> reqs = job.outstanding_requests();
    if (reverse) std::reverse(reqs.begin(), reqs.end());
    int accepted = 0;
    for (const PageRequest &req : reqs) {
        int w = 0, h = 0;
        cache.page_rect(req.version, req.page_x, req.page_y, w, h);
        std::vector<uint8_t> bytes =
            page_bytes(cache, req.version, req.page_x, req.page_y, fn);
        if (cache.supply(req.ticket, w, h, bytes.data(), bytes.size()) ==
            PageSupplyOutcome::ACCEPTED) {
            ++accepted;
        }
    }
    return accepted;
}

// Drive one job to completion (or failure/cancel), one small step budget at a
// time.  Records residency peaks and every ticket observed.
struct PumpStats {
    uint64_t rounds = 0;
    uint64_t steps = 0;
    size_t peak_resident = 0;
    size_t peak_pending = 0;
    std::set<uint64_t> tickets;
    std::set<std::pair<int, int>> page_coords;
};

PagedJobStatus pump(PagedCache &cache, PagedRenderJob &job,
                    const TexelFn &fn, size_t budget = 3,
                    PumpStats *stats = nullptr, bool reverse = false) {
    PagedJobStatus status = job.status();
    for (int guard = 0; guard < 1000000; ++guard) {
        status = job.step(budget);
        if (stats) {
            ++stats->rounds;
            stats->steps = job.total_steps();
            stats->peak_resident =
                std::max(stats->peak_resident, cache.resident_page_count());
            stats->peak_pending =
                std::max(stats->peak_pending, job.pending_fragment_count());
            for (const PageRequest &req : job.outstanding_requests()) {
                stats->tickets.insert(req.ticket);
                stats->page_coords.insert({req.page_x, req.page_y});
            }
        }
        if (status == PagedJobStatus::COMPLETE ||
            status == PagedJobStatus::FAILED ||
            status == PagedJobStatus::CANCELLED) {
            return status;
        }
        deliver_all(cache, job, fn, reverse);
    }
    return status;
}

Mesh textured_quad_mesh(bool with_uv_zero = false) {
    Mesh mesh;
    mesh.vertices = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
    if (with_uv_zero) {
        mesh.uvs = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
    } else {
        mesh.uvs = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    }
    mesh.colors.assign(4, Color(1, 1, 1, 1));
    mesh.normals.assign(4, Vec3(0, 0, 1));
    return mesh;
}

Image make_dense_image(int width, int height, const TexelFn &fn) {
    Image image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t r, g, b, a;
            fn(x, y, r, g, b, a);
            image.set_pixel(x, y, r, g, b, a);
        }
    }
    return image;
}

Scene dense_scene_for(const Mesh &paged_mesh, int tex_w, int tex_h,
                      const TexelFn &fn) {
    Mesh dense = paged_mesh;
    dense.texture = make_dense_image(tex_w, tex_h, fn);
    Scene scene;
    scene.add(dense);
    return scene;
}

Camera default_camera() {
    Camera cam;
    cam.eye = Vec3(0, 0, 4);
    cam.center = Vec3(0, 0, 0);
    cam.up = Vec3(0, 1, 0);
    return cam;
}

RenderOptions default_options(int width = 40, int height = 32) {
    RenderOptions opts;
    opts.width = width;
    opts.height = height;
    opts.threads = 1;
    opts.aa_samples = 1;
    opts.background_color = Color(0, 0, 0, 1);
    opts.backface_culling = false;
    return opts;
}

bool images_equal(const Image &a, const Image &b) {
    return a.width == b.width && a.height == b.height &&
           a.pixels == b.pixels;
}

// Step a job until at least one fragment is parked (or the job reaches a
// terminal state).  Fixed step counts cannot be used for this because the
// first covered pixel may lie well inside a triangle's bounding box.
PagedJobStatus until_parked(PagedRenderJob &job, size_t budget = 8) {
    PagedJobStatus status = job.status();
    for (int guard = 0; guard < 1000000; ++guard) {
        status = job.step(budget);
        if (status == PagedJobStatus::FAILED ||
            status == PagedJobStatus::COMPLETE ||
            status == PagedJobStatus::CANCELLED) {
            return status;
        }
        if (job.pending_fragment_count() > 0) return status;
    }
    return status;
}

// ---------------------------------------------------------------------------
// residency group
// ---------------------------------------------------------------------------

int residency_capacity1_four_page_filter() {
    PagedCache cache(1);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 1});

    Scene scene;
    scene.add(textured_quad_mesh());
    Camera cam = default_camera();
    RenderOptions opts = default_options();

    PagedRenderJob job(cache, scene, cam, opts,
                       {PagedTextureBinding{0, version}}, /*K=*/64);
    PumpStats stats;
    PagedJobStatus status = pump(cache, job, checker_texel, 2, &stats);
    TASK_CHECK(status == PagedJobStatus::COMPLETE);
    TASK_CHECK(cache.resident_page_count() <= 1);
    TASK_CHECK(stats.peak_resident == 1);
    // Four distinct pages exist, and capacity 1 forces at least one page to be
    // requested a second time after eviction.
    TASK_CHECK(stats.page_coords.size() == 4);
    TASK_CHECK(stats.tickets.size() > 4);
    TASK_CHECK(job.pending_fragment_count() == 0);

    Scene dense = dense_scene_for(textured_quad_mesh(), 8, 8, checker_texel);
    Image reference = Renderer().render_scene(dense, cam, opts);
    TASK_CHECK(images_equal(job.image(), reference));
    return 0;
}

int residency_request_coalescing() {
    PagedCache cache(2);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 7});

    Scene scene1;
    scene1.add(textured_quad_mesh());
    Scene scene2;
    scene2.add(textured_quad_mesh());
    Camera cam = default_camera();
    RenderOptions opts = default_options();

    PagedRenderJob job1(cache, scene1, cam, opts,
                        {PagedTextureBinding{0, version}}, 4);
    PagedRenderJob job2(cache, scene2, cam, opts,
                        {PagedTextureBinding{0, version}}, 4);

    // Park fragments in both jobs; they must share in-flight requests.
    PagedJobStatus s1 = until_parked(job1);
    PagedJobStatus s2 = until_parked(job2);
    TASK_CHECK(s1 == PagedJobStatus::WAITING || s1 == PagedJobStatus::READY);
    TASK_CHECK(s2 == PagedJobStatus::WAITING || s2 == PagedJobStatus::READY);
    TASK_CHECK(job1.pending_fragment_count() > 0);
    TASK_CHECK(job2.pending_fragment_count() > 0);

    std::map<std::pair<int, int>, uint64_t> coord_ticket;
    for (const PageRequest &req : cache.pending_requests()) {
        coord_ticket[{req.page_x, req.page_y}] = req.ticket;
    }
    TASK_CHECK(!coord_ticket.empty());
    // One cache-level request per coordinate, even with two waiting jobs.
    TASK_CHECK(coord_ticket.size() == cache.pending_requests().size());
    for (const PageRequest &req : job1.outstanding_requests()) {
        const std::pair<int, int> coord = {req.page_x, req.page_y};
        TASK_CHECK(coord_ticket[coord] == req.ticket);
    }
    for (const PageRequest &req : job2.outstanding_requests()) {
        const std::pair<int, int> coord = {req.page_x, req.page_y};
        TASK_CHECK(coord_ticket[coord] == req.ticket);
    }

    TASK_CHECK(pump(cache, job1, checker_texel, 3) ==
               PagedJobStatus::COMPLETE);
    // job1's deliveries also fed job2's waiters through the shared requests.
    TASK_CHECK(pump(cache, job2, checker_texel, 3) ==
               PagedJobStatus::COMPLETE);
    TASK_CHECK(images_equal(job1.image(), job2.image()));
    return 0;
}

int residency_supply_validation() {
    PagedCache cache(2);
    const uint64_t version =
        cache.publish(TextureDescriptor{6, 6, 4, 2});  // edge pages exist

    Scene scene;
    scene.add(textured_quad_mesh());
    PagedRenderJob job(cache, scene, default_camera(), default_options(),
                       {PagedTextureBinding{0, version}}, 8);
    TASK_CHECK(until_parked(job) != PagedJobStatus::COMPLETE);
    TASK_CHECK(job.pending_fragment_count() > 0);

    std::vector<PageRequest> reqs = job.outstanding_requests();
    TASK_CHECK(!reqs.empty());
    const PageRequest target = reqs.front();
    int w = 0, h = 0;
    cache.page_rect(target.version, target.page_x, target.page_y, w, h);
    std::vector<uint8_t> good =
        page_bytes(cache, target.version, target.page_x, target.page_y,
                   checker_texel);
    const size_t residents_before = cache.resident_page_count();
    const size_t pending_before = job.pending_fragment_count();

    // Wrong byte count.
    std::vector<uint8_t> short_bytes(good.size() - 4);
    TASK_CHECK(cache.supply(target.ticket, w, h, short_bytes.data(),
                            short_bytes.size()) ==
               PageSupplyOutcome::REJECTED);
    // Wrong dimensions.
    TASK_CHECK(cache.supply(target.ticket, w + 1, h, good.data(),
                            good.size()) ==
               PageSupplyOutcome::REJECTED);
    // Unknown ticket.
    TASK_CHECK(cache.supply(target.ticket + 99999, w, h, good.data(),
                            good.size()) ==
               PageSupplyOutcome::REJECTED);
    // Null data.
    TASK_CHECK(cache.supply(target.ticket, w, h, nullptr, good.size()) ==
               PageSupplyOutcome::REJECTED);
    TASK_CHECK(cache.resident_page_count() == residents_before);
    TASK_CHECK(job.pending_fragment_count() == pending_before);

    // The genuine response is still accepted, and a duplicate has no effect.
    TASK_CHECK(cache.supply(target.ticket, w, h, good.data(),
                            good.size()) ==
               PageSupplyOutcome::ACCEPTED);
    TASK_CHECK(cache.supply(target.ticket, w, h, good.data(),
                            good.size()) ==
               PageSupplyOutcome::REJECTED);
    TASK_CHECK(pump(cache, job, checker_texel, 4) ==
               PagedJobStatus::COMPLETE);
    return 0;
}

int residency_zero_weight_taps_skipped() {
    PagedCache cache(4);
    const uint64_t version =
        cache.publish(TextureDescriptor{4, 4, 2, 3});

    Scene scene;
    scene.add(textured_quad_mesh(/*with_uv_zero=*/true));
    PagedRenderJob job(cache, scene, default_camera(), default_options(),
                       {PagedTextureBinding{0, version}}, 16);
    TASK_CHECK(until_parked(job) != PagedJobStatus::FAILED);

    // UV (0, 0) collapses the bilinear filter onto a single texel, so only
    // page (0, 0) is ever requested.
    for (const PageRequest &req : job.outstanding_requests()) {
        TASK_CHECK(req.page_x == 0);
        TASK_CHECK(req.page_y == 0);
    }
    TASK_CHECK(pump(cache, job, solid_texel, 4) ==
               PagedJobStatus::COMPLETE);
    return 0;
}

int residency_deliver_then_evict_keeps_tap() {
    PagedCache cache(1);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 4});

    Scene scene;
    scene.add(textured_quad_mesh());
    PagedRenderJob job(cache, scene, default_camera(), default_options(),
                       {PagedTextureBinding{0, version}}, 64);

    // Park a fragment that needs taps from several pages.
    TASK_CHECK(until_parked(job) != PagedJobStatus::COMPLETE);
    TASK_CHECK(job.pending_fragment_count() > 0);

    std::vector<PageRequest> first = job.outstanding_requests();
    TASK_CHECK(!first.empty());
    const uint64_t first_ticket = first.front().ticket;
    int w = 0, h = 0;
    cache.page_rect(first.front().version, first.front().page_x,
                    first.front().page_y, w, h);
    std::vector<uint8_t> bytes = page_bytes(
        cache, first.front().version, first.front().page_x,
        first.front().page_y, checker_texel);
    TASK_CHECK(cache.supply(first_ticket, w, h, bytes.data(),
                            bytes.size()) ==
               PageSupplyOutcome::ACCEPTED);

    // Supplying every other page churns the first one out of the cache.
    for (const PageRequest &req : job.outstanding_requests()) {
        if (req.ticket == first_ticket) continue;
        int rw = 0, rh = 0;
        cache.page_rect(req.version, req.page_x, req.page_y, rw, rh);
        std::vector<uint8_t> more = page_bytes(
            cache, req.version, req.page_x, req.page_y, checker_texel);
        cache.supply(req.ticket, rw, rh, more.data(), more.size());
    }
    TASK_CHECK(cache.resident_page_count() <= 1);

    // The delivered ticket must never be requested again: its tap is held by
    // the parked fragment itself.
    for (const PageRequest &req : job.outstanding_requests()) {
        TASK_CHECK(req.ticket != first_ticket);
    }
    TASK_CHECK(pump(cache, job, checker_texel, 4) ==
               PagedJobStatus::COMPLETE);

    Scene dense = dense_scene_for(textured_quad_mesh(), 8, 8, checker_texel);
    TASK_CHECK(images_equal(
        job.image(),
        Renderer().render_scene(dense, default_camera(),
                                default_options())));
    return 0;
}

// ---------------------------------------------------------------------------
// continuation group
// ---------------------------------------------------------------------------

int continuation_budget_zero_and_counters() {
    PagedCache cache(2);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 5});
    Scene scene;
    scene.add(textured_quad_mesh());
    PagedRenderJob job(cache, scene, default_camera(), default_options(),
                       {PagedTextureBinding{0, version}}, 16);

    TASK_CHECK(job.step(0) == PagedJobStatus::READY);
    TASK_CHECK(job.total_steps() == 0);
    TASK_CHECK(job.submission_count() == 0);
    TASK_CHECK(job.pending_fragment_count() == 0);

    TASK_CHECK(pump(cache, job, checker_texel, 1) ==
               PagedJobStatus::COMPLETE);
    TASK_CHECK(job.total_steps() > 0);
    TASK_CHECK(job.submission_count() > 0);
    TASK_CHECK(job.outstanding_requests().empty());
    return 0;
}

int continuation_small_budget_clipped_matches_dense() {
    PagedCache cache(1);

    RenderOptions opts = default_options(36, 36);
    // World-space user clip plane plus the near plane exercised by the
    // pipeline; keeps roughly the x <= 0 half of the quad.
    opts.clip_planes.push_back(ClipPlane{Vec3(-1, 0, 0), 0.0f});

    Camera cam = default_camera();

    // Untextured, vertex-colored clipped mesh: this path carries no UV at
    // all, so paged and dense are deterministic and byte-identical.
    Scene scene;
    scene.add(textured_quad_mesh());
    PagedRenderJob job(cache, scene, cam, opts,
                       /*no paged binding=*/{}, 32);
    PumpStats stats;
    TASK_CHECK(pump(cache, job, checker_texel, 1, &stats,
                    /*reverse=*/true) == PagedJobStatus::COMPLETE);
    TASK_CHECK(stats.rounds > 1);  // genuinely incremental

    Scene dense;
    dense.add(textured_quad_mesh());
    Image reference = Renderer().render_scene(dense, cam, opts);
    TASK_CHECK(images_equal(job.image(), reference));
    return 0;
}

int continuation_textured_clip_is_deterministic() {
    // Textured triangles through a user clip plane: the dense renderer's
    // re-projection drops the UVs onto an uninitialized glm::vec2 (latent UB
    // in Renderer), so the paged job instead defines that value as (0, 0).
    // It must still be deterministic across runs and only ever ask for the
    // top-left page, while covering exactly the clipped geometry.
    PagedCache cache(1);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 16});

    RenderOptions clip_opts = default_options(36, 36);
    clip_opts.clip_planes.push_back(ClipPlane{Vec3(-1, 0, 0), 0.0f});
    Camera cam = default_camera();

    Scene scene;
    scene.add(textured_quad_mesh());
    PagedRenderJob job1(cache, scene, cam, clip_opts,
                        {PagedTextureBinding{0, version}}, 32);
    TASK_CHECK(pump(cache, job1, checker_texel, 2) ==
               PagedJobStatus::COMPLETE);

    // Every textured request generated by the clipped triangles is page (0,0).
    PagedCache cache2(1);
    const uint64_t version2 = cache2.publish(
        TextureDescriptor{8, 8, 4, 17});
    PagedRenderJob job2(cache2, scene, cam, clip_opts,
                        {PagedTextureBinding{0, version2}}, 32);
    std::set<std::pair<int, int>> coords;
    PagedJobStatus st = PagedJobStatus::READY;
    while (st != PagedJobStatus::COMPLETE &&
           st != PagedJobStatus::FAILED) {
        st = job2.step(2);
        for (const PageRequest &req : job2.outstanding_requests()) {
            coords.insert({req.page_x, req.page_y});
        }
        deliver_all(cache2, job2, checker_texel);
    }
    TASK_CHECK(st == PagedJobStatus::COMPLETE);
    TASK_CHECK(!coords.empty());
    for (const std::pair<int, int> &coord : coords) {
        TASK_CHECK(coord.first == 0);
        TASK_CHECK(coord.second == 0);
    }

    // Two independent runs are byte-identical: no uninitialized reads.
    PagedCache cache3(1);
    const uint64_t version3 = cache3.publish(
        TextureDescriptor{8, 8, 4, 18});
    PagedRenderJob job3(cache3, scene, cam, clip_opts,
                        {PagedTextureBinding{0, version3}}, 32);
    TASK_CHECK(pump(cache3, job3, checker_texel, 2) ==
               PagedJobStatus::COMPLETE);
    TASK_CHECK(images_equal(job1.image(), job3.image()));

    // Covered pixel set matches the (untextured) clipped geometry exactly.
    RenderOptions plain_opts = default_options(36, 36);
    plain_opts.clip_planes = clip_opts.clip_planes;
    Scene plain_scene;
    plain_scene.add(textured_quad_mesh());
    PagedRenderJob coverage(cache3, plain_scene, cam, plain_opts,
                            /*no binding=*/{}, 32);
    TASK_CHECK(pump(cache3, coverage, checker_texel, 2) ==
               PagedJobStatus::COMPLETE);
    const Image &a = job1.image();
    const Image &b = coverage.image();
    TASK_CHECK(a.width == b.width && a.height == b.height);
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            uint8_t ar, ag, ab, aa, br, bg, bb, ba;
            a.get_pixel(x, y, ar, ag, ab, aa);
            b.get_pixel(x, y, br, bg, bb, ba);
            const bool a_background = (ar == 0 && ag == 0 && ab == 0);
            const bool b_background = (br == 0 && bg == 0 && bb == 0);
            TASK_CHECK(a_background == b_background);
        }
    }
    return 0;
}

int continuation_k_limit_waits() {
    PagedCache cache(4);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 8});
    Scene scene;
    scene.add(textured_quad_mesh());
    PagedRenderJob job(cache, scene, default_camera(), default_options(),
                       {PagedTextureBinding{0, version}}, /*K=*/1);

    bool saw_waiting = false;
    PagedJobStatus status = PagedJobStatus::READY;
    for (int guard = 0; guard < 100000; ++guard) {
        status = job.step(2);
        if (status == PagedJobStatus::WAITING) saw_waiting = true;
        if (status == PagedJobStatus::COMPLETE ||
            status == PagedJobStatus::FAILED) break;
        TASK_CHECK(job.pending_fragment_count() <= 1);
        deliver_all(cache, job, checker_texel);
    }
    TASK_CHECK(saw_waiting);
    TASK_CHECK(status == PagedJobStatus::COMPLETE);
    return 0;
}

int continuation_resume_submits_once() {
    PagedCache cache(1);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 9});
    Scene scene;
    scene.add(textured_quad_mesh());
    RenderOptions opts = default_options();
    PagedRenderJob job(cache, scene, default_camera(), opts,
                       {PagedTextureBinding{0, version}}, 64);
    TASK_CHECK(pump(cache, job, checker_texel, 2, nullptr, true) ==
               PagedJobStatus::COMPLETE);

    // One opaque quad, fill rule guarantees every covered pixel is written
    // exactly once, so submissions equal the number of non-background pixels.
    const Image &img = job.image();
    size_t colored = 0;
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            uint8_t r, g, b, a;
            img.get_pixel(x, y, r, g, b, a);
            if (r != 0 || g != 0 || b != 0) ++colored;
        }
    }
    TASK_CHECK(colored > 0);
    TASK_CHECK(job.submission_count() == colored);
    return 0;
}

int continuation_equal_depth_order() {
    // Two coincident quads at the same depth with different textures.  The
    // first scene node wins every equal-depth tie in the dense renderer; the
    // paged job must preserve that even when the second node's pages arrive
    // first.
    PagedCache cache(2);
    const uint64_t v1 = cache.publish(TextureDescriptor{4, 4, 2, 11});
    const uint64_t v2 = cache.publish(TextureDescriptor{4, 4, 2, 12});

    Scene scene;
    scene.add(textured_quad_mesh());
    scene.add(textured_quad_mesh());
    RenderOptions opts = default_options();
    PagedRenderJob job(cache, scene, default_camera(), opts,
                       {PagedTextureBinding{0, v1},
                        PagedTextureBinding{1, v2}}, 64);

    for (int guard = 0; guard < 100000; ++guard) {
        PagedJobStatus status = job.step(3);
        if (status == PagedJobStatus::COMPLETE ||
            status == PagedJobStatus::FAILED) break;
        // Deliver v2 pages before v1 pages on every round.
        std::vector<PageRequest> reqs = job.outstanding_requests();
        std::sort(reqs.begin(), reqs.end(),
                  [](const PageRequest &a, const PageRequest &b) {
                      return a.version > b.version;
                  });
        for (const PageRequest &req : reqs) {
            int w = 0, h = 0;
            cache.page_rect(req.version, req.page_x, req.page_y, w, h);
            const TexelFn &fn =
                req.version == v1 ? checker_texel : solid_texel;
            std::vector<uint8_t> bytes = page_bytes(
                cache, req.version, req.page_x, req.page_y, fn);
            cache.supply(req.ticket, w, h, bytes.data(), bytes.size());
        }
    }
    TASK_CHECK(job.status() == PagedJobStatus::COMPLETE);

    Mesh d1 = textured_quad_mesh();
    d1.texture = make_dense_image(4, 4, checker_texel);
    Mesh d2 = textured_quad_mesh();
    d2.texture = make_dense_image(4, 4, solid_texel);
    Scene dense;
    dense.add(d1);
    dense.add(d2);
    TASK_CHECK(images_equal(
        job.image(),
        Renderer().render_scene(dense, default_camera(), opts)));
    return 0;
}

int continuation_untextured_fragment_waits_behind_textured() {
    // A paged-textured quad (first node) is parked on missing pages while a
    // second, coincident, *untextured* quad is scanned.  The untextured
    // fragments must not jump ahead and write the shared pixels; they are
    // held in order and only submitted once the earlier textured fragments
    // resolve.  Final bytes match the dense renderer, where the first node
    // wins every equal-depth tie.
    PagedCache cache(1);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 14});

    Mesh behind = textured_quad_mesh();
    behind.colors.assign(4, Color(0.9f, 0.1f, 0.1f, 1.0f));  // distinctive red

    Scene scene;
    scene.add(textured_quad_mesh());  // node 0: paged texture
    scene.add(behind);                // node 1: untextured, same footprint
    RenderOptions opts = default_options();
    // K is large enough to hold every parked fragment of the first node so
    // the scan reaches the second, untextured node and order-gates it.
    PagedRenderJob job(cache, scene, default_camera(), opts,
                       {PagedTextureBinding{0, version}}, 4000);

    // Park the textured node, then keep stepping (without supplying) so the
    // untextured node is scanned and its fragments get order-gated.
    TASK_CHECK(until_parked(job) != PagedJobStatus::FAILED);
    const size_t textured_parks = job.pending_fragment_count();
    TASK_CHECK(textured_parks > 0);
    for (int i = 0; i < 2000 && job.status() == PagedJobStatus::READY; ++i) {
        job.step(8);
        if (job.pending_fragment_count() > textured_parks) break;
    }
    TASK_CHECK(job.pending_fragment_count() > textured_parks);

    // Nothing has been written yet: the textured fragments are all parked and
    // every untextured fragment is order-gated behind one.  The only code
    // path that writes color/depth/normals increments the submission counter,
    // so zero submissions proves no pixel was touched out of order.
    TASK_CHECK(job.submission_count() == 0);

    TASK_CHECK(pump(cache, job, checker_texel, 3) ==
               PagedJobStatus::COMPLETE);

    Mesh d0 = textured_quad_mesh();
    d0.texture = make_dense_image(8, 8, checker_texel);
    Scene dense;
    dense.add(d0);
    dense.add(behind);
    TASK_CHECK(images_equal(
        job.image(),
        Renderer().render_scene(dense, default_camera(), opts)));
    return 0;
}

int continuation_orthographic_flat_shading() {
    PagedCache cache(2);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 13});

    RenderOptions opts = default_options();
    opts.shading = ShadingMode::FLAT;
    Camera cam = default_camera();
    cam.projection = ProjectionType::ORTHOGRAPHIC;

    Scene scene;
    scene.add(textured_quad_mesh());
    PagedRenderJob job(cache, scene, cam, opts,
                       {PagedTextureBinding{0, version}}, 16);
    TASK_CHECK(pump(cache, job, checker_texel, 5) ==
               PagedJobStatus::COMPLETE);

    Scene dense = dense_scene_for(textured_quad_mesh(), 8, 8, checker_texel);
    TASK_CHECK(images_equal(
        job.image(), Renderer().render_scene(dense, cam, opts)));
    return 0;
}

// ---------------------------------------------------------------------------
// lifetime group
// ---------------------------------------------------------------------------

int lifetime_input_copies_are_independent() {
    PagedCache cache(2);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 21});

    Scene scene;
    scene.add(textured_quad_mesh());
    Camera cam = default_camera();
    RenderOptions opts = default_options();

    PagedRenderJob job(cache, scene, cam, opts,
                       {PagedTextureBinding{0, version}}, 16);

    // Mutate and clear every caller-side input after construction.
    scene.meshes.clear();
    scene.lines.clear();
    cam.eye = Vec3(99, 99, 99);
    opts.width = opts.height = 7;

    TASK_CHECK(pump(cache, job, checker_texel, 4) ==
               PagedJobStatus::COMPLETE);

    Scene dense = dense_scene_for(textured_quad_mesh(), 8, 8, checker_texel);
    Camera dense_cam = default_camera();
    RenderOptions dense_opts = default_options();
    TASK_CHECK(images_equal(
        job.image(),
        Renderer().render_scene(dense, dense_cam, dense_opts)));
    return 0;
}

int lifetime_completed_image_survives_eviction_and_release() {
    struct DummyWaiter : PagedPageWaiter {
        void paged_page_ready(uint64_t, uint64_t,
                              const ResidentPage &) override {}
    } dummy;

    PagedCache cache(1);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 22});
    Scene scene;
    scene.add(textured_quad_mesh());
    RenderOptions opts = default_options();
    PagedRenderJob job(cache, scene, default_camera(), opts,
                       {PagedTextureBinding{0, version}}, 32);
    TASK_CHECK(pump(cache, job, checker_texel, 3) ==
               PagedJobStatus::COMPLETE);

    Image finished = job.take_image();
    const std::vector<uint8_t> snapshot = finished.pixels;

    // Churn the cache with a second version: each accepted page evicts the
    // previous one at capacity 1.
    const uint64_t other = cache.publish(TextureDescriptor{4, 4, 2, 23});
    const int other_cols = 2, other_rows = 2;
    for (int py = 0; py < other_rows; ++py) {
        for (int px = 0; px < other_cols; ++px) {
            uint64_t ticket = cache.request_page(other, px, py, &dummy);
            TASK_CHECK(ticket != 0);
            int pw = 0, ph = 0;
            cache.page_rect(other, px, py, pw, ph);
            std::vector<uint8_t> bytes(
                static_cast<size_t>(pw) * ph * 4u, uint8_t(123));
            TASK_CHECK(cache.supply(ticket, pw, ph, bytes.data(),
                                    bytes.size()) ==
                       PageSupplyOutcome::ACCEPTED);
            TASK_CHECK(cache.resident_page_count() == 1);
        }
    }
    cache.release(version);
    TASK_CHECK(cache.descriptor(version) == nullptr);
    TASK_CHECK(finished.pixels == snapshot);
    TASK_CHECK(finished.width == opts.width);

    cache.detach_waiter(&dummy);
    return 0;
}

int lifetime_cancel_withdraws_waiters() {
    PagedCache cache(2);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 24});

    Scene scene1;
    scene1.add(textured_quad_mesh());
    Scene scene2;
    scene2.add(textured_quad_mesh());
    RenderOptions opts = default_options();
    PagedRenderJob job1(cache, scene1, default_camera(), opts,
                        {PagedTextureBinding{0, version}}, 8);
    PagedRenderJob job2(cache, scene2, default_camera(), opts,
                        {PagedTextureBinding{0, version}}, 8);

    TASK_CHECK(until_parked(job1) != PagedJobStatus::COMPLETE);
    TASK_CHECK(until_parked(job2) != PagedJobStatus::COMPLETE);
    TASK_CHECK(!cache.pending_requests().empty());

    job1.cancel();
    TASK_CHECK(job1.status() == PagedJobStatus::CANCELLED);
    TASK_CHECK(job1.step(4) == PagedJobStatus::CANCELLED);
    TASK_CHECK(job1.outstanding_requests().empty());

    // The shared requests keep serving job2; its completion is unaffected.
    TASK_CHECK(pump(cache, job2, checker_texel, 3) ==
               PagedJobStatus::COMPLETE);
    TASK_CHECK(job1.status() == PagedJobStatus::CANCELLED);
    return 0;
}

int lifetime_version_replacement_late_response() {
    PagedCache cache(2);
    const uint64_t v1 = cache.publish(TextureDescriptor{8, 8, 4, 31});
    const uint64_t v2 = cache.publish(TextureDescriptor{6, 6, 4, 31});
    TASK_CHECK(v1 != v2);

    Scene scene;
    scene.add(textured_quad_mesh());
    RenderOptions opts = default_options();
    PagedRenderJob job(cache, scene, default_camera(), opts,
                       {PagedTextureBinding{0, v1}}, 8);
    TASK_CHECK(until_parked(job) != PagedJobStatus::COMPLETE);
    const std::vector<PageRequest> v1_requests = job.outstanding_requests();
    TASK_CHECK(!v1_requests.empty());
    const uint64_t stale_ticket = v1_requests.front().ticket;

    // A new job on the replacement version must never receive v1 pages.
    PagedRenderJob job2(cache, scene, default_camera(), opts,
                        {PagedTextureBinding{0, v2}}, 8);
    TASK_CHECK(until_parked(job2) != PagedJobStatus::FAILED);
    const std::vector<PageRequest> v2_requests = job2.outstanding_requests();
    TASK_CHECK(!v2_requests.empty());

    std::set<uint64_t> v2_tickets;
    for (const PageRequest &req : v2_requests) v2_tickets.insert(req.ticket);
    TASK_CHECK(v2_tickets.find(stale_ticket) == v2_tickets.end());

    // Releasing v1 invalidates its in-flight tickets; the late response is
    // rejected and nothing leaks into v2.
    cache.release(v1);
    int w = 0, h = 0;
    cache.page_rect(v2, 0, 0, w, h);
    std::vector<uint8_t> bytes(static_cast<size_t>(w) * h * 4u, 200);
    TASK_CHECK(cache.supply(stale_ticket, w, h, bytes.data(),
                            bytes.size()) ==
               PageSupplyOutcome::REJECTED);
    TASK_CHECK(job2.status() != PagedJobStatus::FAILED);
    TASK_CHECK(pump(cache, job2, solid_texel, 3) ==
               PagedJobStatus::COMPLETE);
    return 0;
}

int lifetime_two_jobs_interleave_and_cancel() {
    PagedCache cache(1);
    const uint64_t version =
        cache.publish(TextureDescriptor{8, 8, 4, 25});
    RenderOptions opts = default_options();
    Scene scene;
    scene.add(textured_quad_mesh());

    PagedRenderJob a(cache, scene, default_camera(), opts,
                     {PagedTextureBinding{0, version}}, 4);
    PagedRenderJob b(cache, scene, default_camera(), opts,
                     {PagedTextureBinding{0, version}}, 4);

    // Interleave the two jobs only until each has parked at least one
    // fragment: that proves they share the capacity-1 cache without either
    // job finishing, so `a` can still be cancelled.
    bool a_parked = false;
    bool b_parked = false;
    PagedJobStatus sa = PagedJobStatus::READY;
    PagedJobStatus sb = PagedJobStatus::READY;
    for (int guard = 0; guard < 100000; ++guard) {
        sa = a.step(2);
        sb = b.step(2);
        if (a.pending_fragment_count() > 0) a_parked = true;
        if (b.pending_fragment_count() > 0) b_parked = true;
        if (sa == PagedJobStatus::WAITING || sb == PagedJobStatus::WAITING) {
            deliver_all(cache, a, checker_texel);
            deliver_all(cache, b, checker_texel);
        }
        if (a_parked && b_parked) break;
        if (sa == PagedJobStatus::FAILED || sb == PagedJobStatus::FAILED) {
            TASK_CHECK(false);
        }
    }
    TASK_CHECK(a_parked && b_parked);
    TASK_CHECK(a.status() != PagedJobStatus::COMPLETE);

    a.cancel();
    TASK_CHECK(a.status() == PagedJobStatus::CANCELLED);

    // b keeps running against the same shared cache and still completes.
    TASK_CHECK(pump(cache, b, checker_texel, 2) ==
               PagedJobStatus::COMPLETE);
    TASK_CHECK(b.status() == PagedJobStatus::COMPLETE);

    Scene dense = dense_scene_for(textured_quad_mesh(), 8, 8, checker_texel);
    TASK_CHECK(images_equal(
        b.image(),
        Renderer().render_scene(dense, default_camera(), opts)));
    return 0;
}

int lifetime_destructors_leave_no_dangling_waiters() {
    PagedCache outer(2);
    const uint64_t outer_version =
        outer.publish(TextureDescriptor{4, 4, 2, 26});
    {
        PagedCache inner(1);
        const uint64_t inner_version =
            inner.publish(TextureDescriptor{4, 4, 2, 27});
        Scene scene;
        scene.add(textured_quad_mesh());
        {
            PagedRenderJob job(inner, scene, default_camera(),
                               default_options(),
                               {PagedTextureBinding{0, inner_version}}, 4);
            // Park a fragment so the job registers at least one cache waiter.
            TASK_CHECK(until_parked(job) != PagedJobStatus::FAILED);
            TASK_CHECK(job.pending_fragment_count() > 0);
        }  // job destructor detaches its waiters
        TASK_CHECK(inner.pending_requests().empty());
    }  // inner cache destructor

    // The outer cache is still fully usable.
    TASK_CHECK(outer.descriptor(outer_version) != nullptr);
    Scene scene;
    scene.add(textured_quad_mesh());
    PagedRenderJob job(outer, scene, default_camera(), default_options(),
                       {PagedTextureBinding{0, outer_version}}, 8);
    TASK_CHECK(pump(outer, job, checker_texel, 4) ==
               PagedJobStatus::COMPLETE);
    return 0;
}

int lifetime_alpha_255_enforced() {
    PagedCache cache(2);
    const uint64_t version =
        cache.publish(TextureDescriptor{4, 4, 2, 28});
    Scene scene;
    scene.add(textured_quad_mesh());
    PagedRenderJob job(cache, scene, default_camera(), default_options(),
                       {PagedTextureBinding{0, version}}, 8);
    TASK_CHECK(until_parked(job) != PagedJobStatus::COMPLETE);
    TASK_CHECK(!job.outstanding_requests().empty());

    auto bad_texel = [](int, int, uint8_t &r, uint8_t &g, uint8_t &b,
                        uint8_t &a) {
        r = 10; g = 20; b = 30; a = 200;  // illegal: alpha != 255
    };
    for (const PageRequest &req : job.outstanding_requests()) {
        int w = 0, h = 0;
        cache.page_rect(req.version, req.page_x, req.page_y, w, h);
        std::vector<uint8_t> bytes =
            page_bytes(cache, req.version, req.page_x, req.page_y, bad_texel);
        cache.supply(req.ticket, w, h, bytes.data(), bytes.size());
    }
    TASK_CHECK(job.step(4) == PagedJobStatus::FAILED);
    // Failure detached every waiter and the job never publishes a result.
    TASK_CHECK(cache.pending_requests().empty());
    TASK_CHECK(job.pending_fragment_count() == 0);
    TASK_CHECK(job.step(4) == PagedJobStatus::FAILED);
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    // residency
    {"residency", "capacity1_four_page_filter",
     residency_capacity1_four_page_filter},
    {"residency", "request_coalescing", residency_request_coalescing},
    {"residency", "supply_validation", residency_supply_validation},
    {"residency", "zero_weight_taps_skipped",
     residency_zero_weight_taps_skipped},
    {"residency", "deliver_then_evict_keeps_tap",
     residency_deliver_then_evict_keeps_tap},

    // continuation
    {"continuation", "budget_zero_and_counters",
     continuation_budget_zero_and_counters},
    {"continuation", "small_budget_clipped_matches_dense",
     continuation_small_budget_clipped_matches_dense},
    {"continuation", "textured_clip_is_deterministic",
     continuation_textured_clip_is_deterministic},
    {"continuation", "k_limit_waits", continuation_k_limit_waits},
    {"continuation", "resume_submits_once",
     continuation_resume_submits_once},
    {"continuation", "equal_depth_order", continuation_equal_depth_order},
    {"continuation", "untextured_waits_behind_textured",
     continuation_untextured_fragment_waits_behind_textured},
    {"continuation", "orthographic_flat_shading",
     continuation_orthographic_flat_shading},

    // lifetime
    {"lifetime", "input_copies_are_independent",
     lifetime_input_copies_are_independent},
    {"lifetime", "completed_image_survives_eviction_and_release",
     lifetime_completed_image_survives_eviction_and_release},
    {"lifetime", "cancel_withdraws_waiters",
     lifetime_cancel_withdraws_waiters},
    {"lifetime", "version_replacement_late_response",
     lifetime_version_replacement_late_response},
    {"lifetime", "two_jobs_interleave_and_cancel",
     lifetime_two_jobs_interleave_and_cancel},
    {"lifetime", "destructors_leave_no_dangling_waiters",
     lifetime_destructors_leave_no_dangling_waiters},
    {"lifetime", "alpha_255_enforced", lifetime_alpha_255_enforced},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
