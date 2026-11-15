#pragma once
//
// MaterialSession.h
//
// Independent material editing session for the Toy Path Tracer CPU backend.
//
// A MaterialSession keeps the default sphere geometry, camera and image size
// fixed (there is no animation). A caller publishes whole batches of material
// parameters; every publication is atomic (all or nothing) and, when valid,
// creates a new material generation and clears the accumulated image.
//
// Rendering is ticket based:
//
//   1. BeginFrame() captures an owning snapshot of the current generation.
//      Later publications never affect a frame that has already begun.
//   2. FrameTicket::Compute() / MaterialSession::ComputeTicket() renders one
//      frame into the ticket's private linear-RGB buffer. The buffer is not
//      the accumulation image.
//   3. MaterialSession::CommitTicket() folds exactly one frame into the
//      accumulation image, but only if the ticket belongs to the session,
//      computed successfully and still references the current generation.
//
// Intersection, scattering and tracing reuse the same production CPU path
// logic as Test.cpp (HitSpheres, the Lambert/Metal/Dielectric scatter model,
// explicit emissive sphere sampling, four samples per pixel, xorshift RNG).
//
// C++17 only. Threading uses the enkiTS version shipped under
// Cpp/Source/enkiTS; no window system or GPU is required.
//

#include <stddef.h>
#include <stdint.h>

namespace materialsession {

// Material type ids. These match the production Material::Type enum values.
inline constexpr int kMaterialTypeLambert    = 0;
inline constexpr int kMaterialTypeMetal      = 1;
inline constexpr int kMaterialTypeDielectric = 2;

// Full material description for one sphere.
struct MaterialParams
{
    int type = kMaterialTypeLambert;
    float albedo[3]    = {0.0f, 0.0f, 0.0f}; // linear RGB, finite and >= 0
    float emissive[3]  = {0.0f, 0.0f, 0.0f}; // linear RGB, finite and >= 0
    float roughness    = 0.0f;               // finite, in [0,1]
    float ri           = 0.0f;               // dielectric: finite and > 0
};

// One entry of a publication batch. A valid batch addresses every sphere
// exactly once: sphereIndex values must be unique and cover [0, SphereCount()).
struct MaterialEntry
{
    int sphereIndex = 0;
    MaterialParams params;
};

enum class PublishStatus
{
    Ok = 0,              // a new generation was installed
    Empty = 1,           // the batch was empty; nothing happened
    InvalidArgument = 2, // validation failed; the session is unchanged
};

enum class TicketStatus
{
    Invalid = 0,     // not a ticket recognised in the current context
    Pending = 1,     // created by BeginFrame, not computed yet
    Running = 2,     // compute in progress
    Computed = 3,    // compute finished successfully
    Committed = 4,   // compute result was folded into the accumulation image
    Canceled = 5,    // ended by CancelTicket / ReleaseTicket
    SessionDead = 6, // owning session was destroyed while the ticket was live
};

enum class CommitResult
{
    Ok = 0,
    InvalidTicket = 1, // null, foreign, canceled, committed or dead ticket
    NotComputed = 2,   // live ticket that has not finished computing
    Stale = 3,         // computed, but its generation is no longer current
};

struct SessionConfig
{
    int width = 32;         // clamped to >= 1
    int height = 24;        // clamped to >= 1
    int workerThreads = 0;  // 0 -> enkiTS default (hardware thread count)
};

// Sink for a full material table plus the emissive sphere index list.
// Pointers may be null when a particular output is not wanted.
struct MaterialSnapshot
{
    MaterialParams* materials = nullptr; // capacity >= materialCapacity entries
    size_t materialCapacity = 0;
    int* emissives = nullptr;             // capacity >= emissiveCapacity entries
    size_t emissiveCapacity = 0;
    size_t emissiveCount = 0;             // filled in by the call
};

// Output of a successful publication.
struct PublishOutput
{
    uint64_t generation = 0; // id of the newly installed generation
    MaterialSnapshot snapshot;
};

class MaterialSession;
struct FrameTicketCore;

// A FrameTicket is owned by the caller (delete it when done). A ticket keeps
// its frame snapshot and render resources alive on its own, so it stays safe
// to compute and release after the owning session has been destroyed or after
// the input arrays handed to a publication have been deleted/modified.
class FrameTicket
{
public:
    FrameTicket(const FrameTicket&) = delete;
    FrameTicket& operator=(const FrameTicket&) = delete;
    ~FrameTicket();

    TicketStatus Status() const;
    int Width() const;
    int Height() const;
    uint64_t Generation() const; // generation captured by BeginFrame
    bool IsCurrent() const;      // owning session alive and still on this generation
    int RayCount() const;

    // Runs the frame render into the ticket's private linear-RGB buffer.
    // Blocking; safe to call without a live session (uses resources owned by
    // the ticket). Idempotent: a ticket that is already computed, canceled or
    // dead simply reports its status again without re-rendering.
    TicketStatus Compute();

    // Copies width*height*3 linear RGB floats. Returns false unless the ticket
    // has computed successfully (in which case the buffer never changes again).
    bool ReadPixels(float* outPixels, size_t count) const;

    // Direct read-only view of the private buffer; null until computed.
    // Valid for the lifetime of the ticket.
    const float* Pixels() const;

private:
    friend class MaterialSession;
    explicit FrameTicket(FrameTicketCore* core);
    FrameTicketCore* core_;
};

class MaterialSession
{
public:
    explicit MaterialSession(const SessionConfig& config = SessionConfig{});
    ~MaterialSession();
    MaterialSession(const MaterialSession&) = delete;
    MaterialSession& operator=(const MaterialSession&) = delete;

    // Fixed scene/session properties.
    int SphereCount() const;
    int Width() const;
    int Height() const;

    // Generation 0 is the default material table. Successful publications
    // increment it; failed/empty publications leave it untouched.
    uint64_t CurrentGeneration() const;

    // Number of frames folded into the accumulation image for the current
    // generation. Reset to zero by every successful publication.
    int CommittedFrameCount() const;

    // Reads the built-in default material table (46 spheres, two emissive).
    void GetDefaultMaterials(MaterialSnapshot& outSnapshot) const;

    // Reads the material table and emissive indices of the current generation.
    // Returns the current generation id.
    uint64_t GetCurrentMaterials(MaterialSnapshot& outSnapshot) const;

    // Atomically validates and publishes a whole material batch.
    //
    // A batch is rejected (InvalidArgument, session untouched) when it is not
    // a full unique cover of [0, SphereCount()), a type is unknown, any float
    // is non-finite, a colour component is negative, roughness is outside
    // [0,1], or a dielectric has ri <= 0. An empty batch returns Empty and
    // does nothing. Any other valid batch - even one identical to the current
    // table - creates a new generation, resets the accumulation image and
    // frame count, and reports the full new table plus emissive indices.
    PublishStatus Publish(const MaterialEntry* entries, size_t count,
                          PublishOutput* outOutput = nullptr);

    // Captures the current generation in an owning snapshot. At most one
    // unterminated ticket may exist per session; BeginFrame returns null while
    // the previous ticket is still active. A ticket is terminated only by a
    // successful commit, CancelTicket, ReleaseTicket or destruction - so a
    // ticket that became stale because of a publication keeps occupying the
    // slot until the caller ends it (its commit is then rejected as Stale).
    FrameTicket* BeginFrame();

    // Blocks while the ticket renders into its private buffer. The ticket
    // must belong to this session; otherwise its current status is returned
    // (SessionDead for a ticket whose session is gone).
    TicketStatus ComputeTicket(FrameTicket* ticket);

    // Cancels a live ticket. Work already running winds down at row
    // boundaries; the active ticket slot is freed immediately so a new frame
    // can begin. Returns false for null/foreign/already-terminal tickets.
    bool CancelTicket(FrameTicket* ticket);

    // Ends a ticket without changing the image or frame count and frees the
    // active ticket slot. Safe on null and on tickets that already ended.
    void ReleaseTicket(FrameTicket* ticket);

    // Folds one frame into the accumulation image. See CommitResult for the
    // rejection rules; only CommitResult::Ok changes the image and adds one to
    // CommittedFrameCount().
    CommitResult CommitTicket(FrameTicket* ticket);

    // Copies width*height*3 accumulated linear RGB floats and returns the
    // committed frame count (zero means the buffer is cleared).
    int ReadAccumulation(float* outPixels, size_t count) const;

private:
    friend class FrameTicket;
    friend struct FrameTicketCore;
    class Impl;
    Impl* impl_;
};

} // namespace materialsession
