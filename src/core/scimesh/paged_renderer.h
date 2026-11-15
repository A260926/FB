/// @file paged_renderer.h
/// @brief PagedRenderJob — incremental, page-driven triangle rendering.
///
/// A PagedRenderJob is a self-contained render of one Scene from one Camera
/// with one RenderOptions set, where the texture images of selected meshes
/// live in a PagedCache instead of being resident as complete Images.
///
/// The job copies every geometric input and every setting it needs at
/// construction time: the caller may mutate or destroy the Scene, Camera and
/// RenderOptions afterwards without affecting the job, and the job pins the
/// texture versions named by its bindings.  Transforms, near-plane and user
/// clipping, the Rasterizer coverage/fill rules, material lighting and depth
/// submission are the same code paths and arithmetic as
/// Renderer::render_scene(); a completed job is byte-identical to the dense
/// renderer on the same inputs.
///
/// Work is driven by step(): each call consumes at most a raster-step budget
/// and returns a PagedJobStatus.  A fragment whose bilinear taps are not all
/// resident is parked: it keeps at most its four texels and weights, asks the
/// cache for the missing pages, and writes neither color, depth nor normals
/// until every tap has been delivered.  Resuming it later submits exactly
/// once, in the original submission order.
///
/// First-version restrictions, rejected up front (std::invalid_argument):
/// opaque triangle meshes only; single sample; single threaded; texture alpha
/// must be 255; line layers, text layers, alpha blending, SSAO and any
/// anti-aliasing sample count other than 1 are refused before work starts.
///
/// @see PagedCache, paged_texture.h, Renderer

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <scimesh/camera.h>
#include <scimesh/image.h>
#include <scimesh/paged_texture.h>
#include <scimesh/rasterizer.h>
#include <scimesh/render_options.h>
#include <scimesh/scene.h>

namespace scimesh {

/// @brief Lifecycle state of a PagedRenderJob.
enum class PagedJobStatus {
    /// @brief The job made progress and still has runnable work; call step()
    ///        again (or supply missing pages first).
    READY,

    /// @brief The job is blocked on page deliveries (a missing page, or the
    ///        pending-fragment limit reached with nothing resumable).
    WAITING,

    /// @brief Every fragment has been processed; image() holds the result.
    COMPLETE,

    /// @brief The job failed (currently: a delivered texture texel had alpha
    ///        other than 255).  No further results are published.
    FAILED,

    /// @brief cancel() was called; waiters were withdrawn and no result is
    ///        published.
    CANCELLED
};

/// @brief Binds one scene mesh (by index) to one paged texture version.
struct PagedTextureBinding {
    /// @brief Index into the Scene's mesh list.
    size_t mesh_index = 0;

    /// @brief Version handle returned by PagedCache::publish().
    uint64_t version = 0;
};

/// @brief One incremental, resumable paged render.
///
/// Owns its private copy of the scene geometry, camera and options, the
/// output image, the depth/normal buffers and the parked fragments.  The
/// referenced PagedCache is not owned and must outlive the job.
class PagedRenderJob : public PagedPageWaiter {
public:
    /// @brief Build a job from fully self-contained inputs.
    ///
    /// @param cache        Page cache shared by any number of jobs; must
    ///                     outlive the job.
    /// @param scene        Scene to render; meshes and transforms are copied.
    /// @param camera       Camera; copied.
    /// @param options      Render settings; copied.  Must be single-sample,
    ///                     single-threaded, SSAO/blending/wire disabled only
    ///                     where the first version refuses them.
    /// @param bindings     Paged texture versions keyed by mesh index.
    /// @param max_pending_fragments K: maximum number of fragments parked
    ///                     waiting on pages at any time (must be >= 1).
    /// @throws std::invalid_argument for any configuration this version
    ///         refuses, unknown mesh indices/versions or invalid meshes.
    PagedRenderJob(PagedCache &cache,
                   const Scene &scene,
                   const Camera &camera,
                   const RenderOptions &options,
                   const std::vector<PagedTextureBinding> &bindings,
                   size_t max_pending_fragments);

    ~PagedRenderJob() override;

    PagedRenderJob(const PagedRenderJob &) = delete;
    PagedRenderJob &operator=(const PagedRenderJob &) = delete;

    /// @brief Advance the render by at most `budget` raster steps.
    ///
    /// One raster step is either examining one pixel of a triangle's
    /// bounding box or resuming one parked fragment.  A budget of 0 performs
    /// no work and returns the current status.
    PagedJobStatus step(size_t budget);

    /// @brief Cancel the job: withdraw every cache waiter immediately.
    ///
    /// Shared in-flight requests keep serving other jobs; a late response can
    /// neither revive this job nor fill another version.  No image is
    /// published afterwards.
    void cancel();

    /// @brief Current lifecycle status.
    PagedJobStatus status() const;

    /// @brief Finished image; valid only when status() == COMPLETE.
    ///
    /// The returned reference is valid while the job lives; take_image()
    /// transfers ownership of the pixels.
    const Image &image() const;

    /// @brief Move the finished image out; valid only when COMPLETE.
    Image take_image();

    /// @brief Requests the job's parked fragments are currently waiting on.
    ///
    /// Deduplicated by ticket; several fragments (or jobs, at cache level)
    /// share one request per version/page.
    std::vector<PageRequest> outstanding_requests() const;

    /// @brief Number of parked fragments still missing at least one tap.
    size_t pending_fragment_count() const;

    /// @brief Number of fragments that have passed the depth test and written
    ///        color, depth and normals (a resumed fragment counts at most
    ///        once).
    uint64_t submission_count() const;

    /// @brief Total raster steps consumed since construction.
    uint64_t total_steps() const;

    /// @brief Configured pending-fragment limit K.
    size_t max_pending_fragments() const;

    /// @internal PagedCache delivery callback; not for direct use.
    void paged_page_ready(uint64_t ticket, uint64_t version,
                          const ResidentPage &page) override;

private:
    struct PendingFragment;
    struct WorkItem;

    void validate_inputs(const std::vector<PagedTextureBinding> &bindings) const;
    void build_work_items();
    bool scan_one_pixel();
    bool resume_one_pending();
    PagedJobStatus finish_status() const;
    bool has_earlier_pending(int x, int y, int64_t sequence) const;
    void submit_fragment(int x, int y, float depth, const Color &base_color,
                         const Vec3 &normal);
    void detach_and_fail();

    PagedCache &cache_;
    Scene scene_;
    Camera camera_;
    RenderOptions options_;
    std::map<size_t, uint64_t> bindings_;
    size_t max_pending_;

    Image output_;
    Rasterizer rasterizer_;
    std::vector<float> &z_buffer_;
    std::vector<Vec3> &normal_buffer_;
    Vec3 light_direction_;

    std::vector<WorkItem> items_;
    size_t item_index_ = 0;
    int cursor_x_ = 0;
    int cursor_y_ = 0;
    int64_t next_sequence_ = 0;

    std::list<PendingFragment> pending_;
    uint64_t submissions_ = 0;
    uint64_t steps_ = 0;
    PagedJobStatus status_ = PagedJobStatus::READY;
};

} // namespace scimesh
