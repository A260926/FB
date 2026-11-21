#ifndef DEMO_ASYNC_POSE_H
#define DEMO_ASYNC_POSE_H

// async_pose.h - background-thread CPU pose evaluation for IQM models.
//
// This header provides three separated concerns:
//
//   1. asyncpose::PoseAsset   - a read-only, self-contained IQM asset.
//      Loading (asyncpose::load_pose_asset) happens before any request is
//      submitted; once loaded the asset owns copies of every array it uses
//      and never borrows the file buffer.  One asset may be shared by any
//      number of characters and services.
//
//   2. asyncpose::PoseService - thread scheduling.  A bounded work queue and
//      a pool of worker threads (at least two may evaluate concurrently).
//      Workers block on a condition variable when idle; they never spin.
//      The queue holds at most one not-yet-started request per character,
//      so it cannot grow without bound under continuous submission.
//
//   3. Per-character evaluation context (PoseService::CharacterId) - each
//      character owns its ticket sequence, its single pending-request slot
//      and its current published snapshot.  Characters never evict or
//      serialize each other.
//
// The classic serial entry animateiqm() in demo.cpp and this service share
// the exact same sampling + skinning implementation
// (asyncpose::sample_and_skin); neither keeps a private copy of the math.
//
// Typical usage (see demo/README.txt for the full guide):
//
//   auto asset = asyncpose::load_pose_asset("model.iqm");   // 1. load asset
//   asyncpose::PoseService service(2);                      // 2. start service
//   asyncpose::CharacterId id = service.create_character(asset); // 3. create
//   asyncpose::Ticket t = service.submit(id, 1.5f);         // 4. submit frame
//   std::shared_ptr<const asyncpose::PoseSnapshot> snap;
//   if(service.wait(id, t, &snap) == asyncpose::RequestStatus::published)
//       ...                                                 // 5. wait request
//   auto current = service.snapshot(id);                    // 6. get snapshot
//   service.unregister_character(id);                       // 7. unregister
//   service.shutdown();                                     // 8. shut down
//
// Request lifecycle rules:
//   - submit() returns a per-character, strictly increasing ticket (> 0),
//     or 0 when the arguments fail validation (bad id, inactive character,
//     asset without animation, non-finite frame, service shut down).  A
//     failed submit leaves the current snapshot and all accepted requests
//     untouched.
//   - Each character holds at most one not-yet-started request.  A new
//     submit replaces that slot and the replaced ticket settles as
//     superseded.
//   - A request already being evaluated runs to completion, but it only
//     updates the character's snapshot if, at publish time, it is still the
//     character's latest accepted ticket; otherwise its result is discarded
//     and it settles as superseded.  A late-finishing old request can never
//     overwrite a newer frame.
//   - Every accepted ticket eventually reaches exactly one terminal state:
//     published, superseded or failed.  wait() therefore never blocks
//     forever.  A ticket that once published keeps its published state even
//     after a newer request replaces its snapshot.
//   - unregister_character() settles all of the character's unpublished
//     requests as superseded and rejects further submissions for that id.
//     Ids are never reused, so a stale id can never address another
//     character.
//   - shutdown() rejects new submissions, settles queued requests as
//     superseded, wakes every waiter, lets in-flight evaluations finish
//     (they may still publish), then joins the worker threads.  Call it from
//     outside the worker threads (never from the evaluation callback).
//
// Snapshots are immutable and reference counted: readers hold
// std::shared_ptr<const PoseSnapshot> and may keep reading it after further
// submissions, after unregistration and after service shutdown.  All arrays
// in one snapshot belong to the same request and are never modified after
// publication.
//
// Evaluation callback (optional, unset by default):
//   service.set_eval_callback([](asyncpose::CharacterId id,
//                                asyncpose::Ticket ticket) { ... });
//   The callback runs on a worker thread after the four-attribute skinning
//   computation completes but before the publish-eligibility check, and it
//   is invoked without any internal lock held.  It exists so tests/tools can
//   deterministically pause stale results; normal operation leaves it unset.
//   If the callback throws, that request settles as failed and the worker
//   thread keeps running.  The callback may read the current snapshot, but
//   it must not shut down the service and must not wait on its own ticket.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "util.h"
#include "geom.h"
#include "iqm.h"

namespace asyncpose
{

// ---------------------------------------------------------------------------
// Shared sampling + skinning core.
//
// This is the single implementation of the classic animateiqm() math:
// linear interpolation between the two adjacent pose matrices, concatenation
// along the parent chain, inverse-bind pre-concatenation (already baked into
// the frames array at load time) and the adjoint-transpose direction
// transform for normals/tangents.  Frame numbers are finite floats and loop
// with a positive modulo over the whole animation frame array, so negative
// frames are supported.
// ---------------------------------------------------------------------------

struct SkinSource
{
    const Matrix3x4 *frames = nullptr;   // numframes * numjoints, pose * inverse-bind
    int numframes = 0;
    const iqmjoint *joints = nullptr;    // numjoints, only ->parent is read
    int numjoints = 0;
    const float *inposition = nullptr;   // 3 * numverts
    const float *innormal = nullptr;     // 3 * numverts
    const float *intangent = nullptr;    // 4 * numverts (w = bitangent sign)
    const uchar *inblendindex = nullptr; // 4 * numverts
    const uchar *inblendweight = nullptr;// 4 * numverts
    int numverts = 0;
};

struct SkinDest
{
    Matrix3x4 *outframe = nullptr;       // numjoints bone matrices
    float *outposition = nullptr;        // 3 * numverts
    float *outnormal = nullptr;          // 3 * numverts
    float *outtangent = nullptr;         // 3 * numverts
    float *outbitangent = nullptr;       // 3 * numverts
};

// Samples the pose at absolute frame number curframe and skins all four
// vertex attributes.  Returns false for non-finite frames or empty input;
// on false the destination is left untouched.
bool sample_and_skin(const SkinSource &src, float curframe, const SkinDest &dst);

// ---------------------------------------------------------------------------
// PoseAsset - read-only IQM asset that owns its data.
// ---------------------------------------------------------------------------

class PoseAsset
{
public:
    int numjoints() const { return joints_; }
    int numframes() const { return frames_; }
    int numverts()  const { return verts_; }

    // Read-only view of the owned arrays, for use with sample_and_skin.
    // The returned pointers stay valid as long as the asset is alive.
    SkinSource skin_source() const;

private:
    friend std::shared_ptr<const PoseAsset> load_pose_asset(const std::string &filename);

    std::vector<iqmjoint> joints;      // bind pose description (parents)
    std::vector<Matrix3x4> frames;     // frames_ * joints_ pose matrices
    std::vector<float> position;       // 3 * verts_
    std::vector<float> normal;         // 3 * verts_
    std::vector<float> tangent;        // 4 * verts_
    std::vector<uchar> blendindex;     // 4 * verts_
    std::vector<uchar> blendweight;    // 4 * verts_
    int joints_ = 0, frames_ = 0, verts_ = 0;
};

// Loads an IQM file (mesh + animation) into a self-contained read-only
// asset.  After a successful load nothing references the file buffer.
// Returns an empty shared_ptr when the file cannot be read or lacks the
// data needed for four-attribute skinning (position, normal, tangent,
// blend indices/weights, joints and at least one animation frame).
std::shared_ptr<const PoseAsset> load_pose_asset(const std::string &filename);

// ---------------------------------------------------------------------------
// Requests, snapshots and characters.
// ---------------------------------------------------------------------------

// Per-character request ticket.  Tickets are strictly increasing per
// character and start at 1; ticket 0 means "no request" (rejected submit).
typedef std::uint64_t Ticket;

// Opaque character handle.  Ids are never reused within a service, so a
// stale id can never resolve to a different character.
struct CharacterId
{
    std::uint32_t slot = 0xFFFFFFFFu;
    std::uint32_t generation = 0;

    bool valid() const { return slot != 0xFFFFFFFFu; }
};

inline bool operator==(const CharacterId &a, const CharacterId &b)
{
    return a.slot == b.slot && a.generation == b.generation;
}
inline bool operator!=(const CharacterId &a, const CharacterId &b) { return !(a == b); }

// Terminal states of an accepted ticket are published, superseded and
// failed.  pending is transient; invalid means the (character, ticket)
// pair is unknown (e.g. ticket 0, a stale id, or a ticket never accepted).
enum class RequestStatus
{
    pending,
    published,
    superseded,
    failed,
    invalid
};

// One published animation frame.  Immutable after publication; every array
// belongs to the same request.  Readers keep it alive with shared_ptr.
struct PoseSnapshot
{
    Ticket ticket = 0;                 // request that produced this snapshot
    float frame = 0;                   // absolute frame number that was sampled
    std::vector<Matrix3x4> joints;     // evaluated bone matrices
    std::vector<Vec3> positions;       // skinned vertex positions
    std::vector<Vec3> normals;         // skinned normals
    std::vector<Vec3> tangents;        // skinned tangents
    std::vector<Vec3> bitangents;      // skinned bitangents
};

// ---------------------------------------------------------------------------
// PoseService - worker pool and request scheduling.
// ---------------------------------------------------------------------------

class PoseService
{
public:
    // worker_count == 0 picks a default of at least 2 workers.  At least two
    // workers can be inside the skinning computation at the same time.
    explicit PoseService(unsigned worker_count = 0);
    ~PoseService();                    // implies shutdown()

    PoseService(const PoseService &) = delete;
    PoseService &operator=(const PoseService &) = delete;

    // Creates a character (its own evaluation context) bound to an asset.
    // Returns an invalid id when the asset is empty or not animatable.
    CharacterId create_character(const std::shared_ptr<const PoseAsset> &asset);

    // Submits an absolute float frame number.  Returns the character's
    // strictly increasing ticket, or 0 when the request is rejected
    // (invalid id, unregistered character, invalid frame, shut-down
    // service).  On rejection the snapshot and accepted requests are
    // unchanged.  A successful submit replaces the character's single
    // not-yet-started request, settling the replaced ticket as superseded.
    Ticket submit(CharacterId id, float frame);

    // Blocks until the ticket reaches a terminal state and returns it.
    // published means the result was installed as the character's snapshot;
    // when result is non-null it receives that exact snapshot (kept valid
    // even if a newer request later replaces it).  Never blocks forever:
    // every accepted ticket is guaranteed to settle.
    RequestStatus wait(CharacterId id, Ticket ticket,
                       std::shared_ptr<const PoseSnapshot> *result = nullptr) const;

    // Non-blocking status query; may still return RequestStatus::pending.
    RequestStatus poll(CharacterId id, Ticket ticket,
                       std::shared_ptr<const PoseSnapshot> *result = nullptr) const;

    // Returns the character's latest published snapshot, or nullptr when
    // nothing has been published yet.  Safe to call from the evaluation
    // callback.
    std::shared_ptr<const PoseSnapshot> snapshot(CharacterId id) const;

    // Settles the character's unpublished requests as superseded and rejects
    // further submissions for this id.  Published snapshots stay readable.
    void unregister_character(CharacterId id);

    // Rejects new submissions, settles queued requests as superseded, wakes
    // all waiters, lets in-flight evaluations finish, then joins the worker
    // threads.  Idempotent.  Must be called from outside the worker threads
    // (in particular not from the evaluation callback).
    void shutdown();

    // Installs the optional evaluation-complete callback (unset by default).
    // Called on a worker thread with (character, ticket) after the
    // four-attribute computation completes and before the publish check,
    // without any internal lock held.  A throwing callback settles the
    // request as failed; the worker thread survives.  The callback may read
    // the current snapshot but must not call shutdown() or wait on its own
    // ticket.  Pass nullptr to clear.
    void set_eval_callback(std::function<void(CharacterId, Ticket)> callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace asyncpose

#endif
