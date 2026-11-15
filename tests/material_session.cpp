// MaterialSession test suite. Registered with the task_support harness through
// task_cases / task_case_count; this TU deliberately defines no main().
//
// Groups: publication, lifetime, rendering. Every case returns 0 on success and
// uses TASK_CHECK for its real assertions.

#include "task_support/check.h"
#include "MaterialSession.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

using namespace materialsession;

namespace {

constexpr int kWidth = 16;
constexpr int kHeight = 12;
constexpr float kEps = 1e-5f;

SessionConfig MakeConfig()
{
    SessionConfig config;
    config.width = kWidth;
    config.height = kHeight;
    return config;
}

// Test helper table: the material parameters plus the reported emissive ids.
struct Table
{
    std::vector<MaterialParams> mats;
    std::vector<int> emissiveList;
    size_t emissiveCount = 0;
};

Table ReadDefaults(MaterialSession& session)
{
    Table table;
    table.mats.resize(session.SphereCount());
    std::vector<int> emissives(session.SphereCount());
    MaterialSnapshot snapshot;
    snapshot.materials = table.mats.data();
    snapshot.materialCapacity = table.mats.size();
    snapshot.emissives = emissives.data();
    snapshot.emissiveCapacity = emissives.size();
    session.GetDefaultMaterials(snapshot);
    table.emissiveCount = snapshot.emissiveCount;
    table.emissiveList.assign(emissives.begin(), emissives.begin() + snapshot.emissiveCount);
    return table;
}

Table ReadCurrent(MaterialSession& session, uint64_t& outGeneration)
{
    Table table;
    table.mats.resize(session.SphereCount());
    std::vector<int> emissives(session.SphereCount());
    MaterialSnapshot snapshot;
    snapshot.materials = table.mats.data();
    snapshot.materialCapacity = table.mats.size();
    snapshot.emissives = emissives.data();
    snapshot.emissiveCapacity = emissives.size();
    outGeneration = session.GetCurrentMaterials(snapshot);
    table.emissiveCount = snapshot.emissiveCount;
    table.emissiveList.assign(emissives.begin(), emissives.begin() + snapshot.emissiveCount);
    return table;
}

std::vector<MaterialEntry> BatchFrom(const std::vector<MaterialParams>& mats)
{
    std::vector<MaterialEntry> batch(mats.size());
    for (size_t i = 0; i < mats.size(); ++i)
    {
        batch[i].sphereIndex = (int)i;
        batch[i].params = mats[i];
    }
    return batch;
}

bool AllFiniteNonNegative(const std::vector<float>& pixels)
{
    for (float v : pixels)
    {
        if (!std::isfinite(v) || v < 0.0f)
            return false;
    }
    return true;
}

float PixelSum(const std::vector<float>& pixels)
{
    float sum = 0.0f;
    for (float v : pixels)
        sum += v;
    return sum;
}

std::vector<float> RenderOneFrame(MaterialSession& session)
{
    FrameTicket* ticket = session.BeginFrame();
    session.ComputeTicket(ticket);
    std::vector<float> pixels((size_t)session.Width() * session.Height() * 3);
    bool ok = ticket->ReadPixels(pixels.data(), pixels.size());
    if (!ok)
    {
        delete ticket;
        return {};
    }
    delete ticket;
    return pixels;
}

// ===========================================================================
// publication
// ===========================================================================

int publication_default_table()
{
    MaterialSession session(MakeConfig());
    TASK_CHECK(session.SphereCount() == 46);
    TASK_CHECK(session.CurrentGeneration() == 0);
    TASK_CHECK(session.CommittedFrameCount() == 0);

    Table table = ReadDefaults(session);
    TASK_CHECK(table.mats.size() == 46);
    TASK_CHECK(table.emissiveCount == 2);
    TASK_CHECK(table.emissiveList.size() == 2);
    TASK_CHECK(table.emissiveList[0] == 8);
    TASK_CHECK(table.emissiveList[1] == 45);
    TASK_CHECK(table.mats[8].emissive[0] == 30.0f);
    TASK_CHECK(table.mats[8].emissive[1] == 25.0f);
    TASK_CHECK(table.mats[8].emissive[2] == 15.0f);
    TASK_CHECK(table.mats[45].emissive[2] == 20.0f);
    TASK_CHECK(table.mats[7].type == kMaterialTypeDielectric);
    TASK_CHECK(table.mats[7].ri == 1.5f);
    TASK_CHECK(table.mats[3].type == kMaterialTypeMetal);
    return 0;
}

int publication_valid_batch_generations()
{
    MaterialSession session(MakeConfig());
    Table def = ReadDefaults(session);
    std::vector<MaterialEntry> batch = BatchFrom(def.mats);

    PublishOutput output;
    PublishStatus status = session.Publish(batch.data(), batch.size(), &output);
    TASK_CHECK(status == PublishStatus::Ok);
    TASK_CHECK(output.generation == 1);
    TASK_CHECK(session.CurrentGeneration() == 1);
    TASK_CHECK(output.snapshot.emissiveCount == 2);
    TASK_CHECK(session.CommittedFrameCount() == 0);

    uint64_t gen = 999;
    Table current = ReadCurrent(session, gen);
    TASK_CHECK(gen == 1);
    TASK_CHECK(current.mats.size() == def.mats.size());
    for (size_t i = 0; i < def.mats.size(); ++i)
    {
        TASK_CHECK(current.mats[i].type == def.mats[i].type);
        for (int c = 0; c < 3; ++c)
        {
            TASK_CHECK(current.mats[i].albedo[c] == def.mats[i].albedo[c]);
            TASK_CHECK(current.mats[i].emissive[c] == def.mats[i].emissive[c]);
        }
        TASK_CHECK(current.mats[i].roughness == def.mats[i].roughness);
        TASK_CHECK(current.mats[i].ri == def.mats[i].ri);
    }

    // An identical, still valid, batch creates a brand new generation.
    status = session.Publish(batch.data(), batch.size(), &output);
    TASK_CHECK(status == PublishStatus::Ok);
    TASK_CHECK(output.generation == 2);
    TASK_CHECK(session.CurrentGeneration() == 2);
    TASK_CHECK(session.CommittedFrameCount() == 0);
    return 0;
}

int publication_rejects_every_invalid_kind()
{
    MaterialSession session(MakeConfig());
    Table def = ReadDefaults(session);
    std::vector<MaterialEntry> good = BatchFrom(def.mats);
    const uint64_t genBefore = session.CurrentGeneration();

    // Empty batch is a no-op, not an error.
    TASK_CHECK(session.Publish(nullptr, 0) == PublishStatus::Empty);
    TASK_CHECK(session.Publish(good.data(), 0) == PublishStatus::Empty);
    TASK_CHECK(session.CurrentGeneration() == genBefore);

    auto expectInvalid = [&](std::vector<MaterialEntry> bad) -> bool {
        return session.Publish(bad.data(), bad.size()) == PublishStatus::InvalidArgument &&
               session.CurrentGeneration() == genBefore;
    };

    // Wrong size (missing one sphere).
    {
        std::vector<MaterialEntry> bad = good;
        bad.pop_back();
        TASK_CHECK(expectInvalid(bad));
    }
    // Duplicate sphere index, leaving one id uncovered.
    {
        std::vector<MaterialEntry> bad = good;
        bad[45].sphereIndex = 0;
        TASK_CHECK(expectInvalid(bad));
    }
    // Out of range index.
    {
        std::vector<MaterialEntry> bad = good;
        bad[10].sphereIndex = session.SphereCount();
        TASK_CHECK(expectInvalid(bad));
    }
    // Negative index.
    {
        std::vector<MaterialEntry> bad = good;
        bad[10].sphereIndex = -1;
        TASK_CHECK(expectInvalid(bad));
    }
    // Illegal type.
    {
        std::vector<MaterialEntry> bad = good;
        bad[2].params.type = 7;
        TASK_CHECK(expectInvalid(bad));
    }
    // Non-finite albedo.
    {
        std::vector<MaterialEntry> bad = good;
        bad[2].params.albedo[1] = INFINITY;
        TASK_CHECK(expectInvalid(bad));
    }
    // NaN emissive.
    {
        std::vector<MaterialEntry> bad = good;
        bad[8].params.emissive[0] = std::nanf("");
        TASK_CHECK(expectInvalid(bad));
    }
    // Negative colour.
    {
        std::vector<MaterialEntry> bad = good;
        bad[4].params.albedo[2] = -0.0001f;
        TASK_CHECK(expectInvalid(bad));
    }
    // Roughness out of range, both directions.
    {
        std::vector<MaterialEntry> bad = good;
        bad[5].params.roughness = 1.0001f;
        TASK_CHECK(expectInvalid(bad));
    }
    {
        std::vector<MaterialEntry> bad = good;
        bad[5].params.roughness = -0.5f;
        TASK_CHECK(expectInvalid(bad));
    }
    // Dielectric with non-positive ri.
    {
        std::vector<MaterialEntry> bad = good;
        bad[7].params.ri = 0.0f;
        TASK_CHECK(expectInvalid(bad));
    }
    {
        std::vector<MaterialEntry> bad = good;
        bad[7].params.ri = -1.0f;
        TASK_CHECK(expectInvalid(bad));
    }
    // Non-finite roughness.
    {
        std::vector<MaterialEntry> bad = good;
        bad[6].params.roughness = INFINITY;
        TASK_CHECK(expectInvalid(bad));
    }

    // The session table is still the untouched default, and a later valid
    // publication still succeeds.
    uint64_t gen = 0;
    Table current = ReadCurrent(session, gen);
    TASK_CHECK(gen == genBefore);
    TASK_CHECK(current.mats[8].emissive[0] == 30.0f);
    TASK_CHECK(session.Publish(good.data(), good.size()) == PublishStatus::Ok);
    TASK_CHECK(session.CurrentGeneration() == genBefore + 1);
    return 0;
}

int publication_copies_input_and_survives_mutation()
{
    MaterialSession session(MakeConfig());
    Table def = ReadDefaults(session);
    std::vector<MaterialEntry> batch = BatchFrom(def.mats);
    TASK_CHECK(session.Publish(batch.data(), batch.size()) == PublishStatus::Ok);

    // Destroy and overwrite the caller's storage; the session must be unaffected.
    batch.clear();
    batch.shrink_to_fit();

    uint64_t gen = 0;
    Table current = ReadCurrent(session, gen);
    TASK_CHECK(gen == 1);
    TASK_CHECK(current.mats[8].emissive[0] == 30.0f);
    TASK_CHECK(current.mats[45].emissive[2] == 20.0f);

    FrameTicket* ticket = session.BeginFrame();
    TASK_CHECK(ticket != nullptr);
    TASK_CHECK(session.ComputeTicket(ticket) == TicketStatus::Computed);
    TASK_CHECK(ticket->RayCount() > 0);
    delete ticket;
    return 0;
}

// ===========================================================================
// lifetime
// ===========================================================================

int lifetime_single_active_ticket()
{
    MaterialSession session(MakeConfig());
    FrameTicket* first = session.BeginFrame();
    TASK_CHECK(first != nullptr);
    TASK_CHECK(session.BeginFrame() == nullptr); // second open ticket refused

    // Ending the ticket frees the single active slot.
    session.ReleaseTicket(first);
    delete first;
    FrameTicket* second = session.BeginFrame();
    TASK_CHECK(second != nullptr);

    // A committed ticket also frees the slot.
    TASK_CHECK(session.ComputeTicket(second) == TicketStatus::Computed);
    TASK_CHECK(session.CommitTicket(second) == CommitResult::Ok);
    delete second;
    FrameTicket* third = session.BeginFrame();
    TASK_CHECK(third != nullptr);

    // A canceled ticket frees the slot too.
    TASK_CHECK(session.CancelTicket(third));
    delete third;
    FrameTicket* fourth = session.BeginFrame();
    TASK_CHECK(fourth != nullptr);
    delete fourth;
    return 0;
}

int lifetime_publish_between_begin_and_commit()
{
    MaterialSession session(MakeConfig());
    Table def = ReadDefaults(session);

    FrameTicket* ticket = session.BeginFrame();
    TASK_CHECK(ticket != nullptr);
    TASK_CHECK(ticket->Generation() == 0);
    TASK_CHECK(ticket->IsCurrent());

    // A new generation lands while the frame is in flight.
    std::vector<MaterialEntry> batch = BatchFrom(def.mats);
    batch[1].params.albedo[0] = 0.99f;
    TASK_CHECK(session.Publish(batch.data(), batch.size()) == PublishStatus::Ok);
    TASK_CHECK(session.CurrentGeneration() == 1);
    TASK_CHECK(!ticket->IsCurrent());

    // The ticket still renders its captured generation, but can no longer commit.
    TASK_CHECK(session.ComputeTicket(ticket) == TicketStatus::Computed);
    TASK_CHECK(ticket->Generation() == 0);
    TASK_CHECK(session.CommitTicket(ticket) == CommitResult::Stale);
    TASK_CHECK(session.CommittedFrameCount() == 0);

    // A stale ticket is still the one unterminated ticket, so a second frame is
    // refused until the caller explicitly ends it.
    TASK_CHECK(session.BeginFrame() == nullptr);
    session.ReleaseTicket(ticket);
    delete ticket;

    // The next frame belongs to the new generation and commits normally.
    FrameTicket* fresh = session.BeginFrame();
    TASK_CHECK(fresh != nullptr);
    TASK_CHECK(fresh->Generation() == 1);
    TASK_CHECK(session.ComputeTicket(fresh) == TicketStatus::Computed);
    TASK_CHECK(session.CommitTicket(fresh) == CommitResult::Ok);
    TASK_CHECK(session.CommittedFrameCount() == 1);
    delete fresh;
    return 0;
}

int lifetime_failed_publish_keeps_ticket_valid()
{
    MaterialSession session(MakeConfig());
    Table def = ReadDefaults(session);
    FrameTicket* ticket = session.BeginFrame();
    TASK_CHECK(ticket != nullptr);

    std::vector<MaterialEntry> bad = BatchFrom(def.mats);
    bad[0].params.ri = -2.0f; // bad, and table 0 is Lambert so make it dielectric too
    bad[0].params.type = kMaterialTypeDielectric;
    TASK_CHECK(session.Publish(bad.data(), bad.size()) == PublishStatus::InvalidArgument);
    TASK_CHECK(session.CurrentGeneration() == 0);
    TASK_CHECK(ticket->IsCurrent());

    TASK_CHECK(session.ComputeTicket(ticket) == TicketStatus::Computed);
    TASK_CHECK(session.CommitTicket(ticket) == CommitResult::Ok);
    TASK_CHECK(session.CommittedFrameCount() == 1);
    delete ticket;
    return 0;
}

int lifetime_cancel_and_repeat_commit()
{
    MaterialSession session(MakeConfig());

    FrameTicket* ticket = session.BeginFrame();
    TASK_CHECK(ticket != nullptr);
    TASK_CHECK(session.CancelTicket(ticket));
    TASK_CHECK(!session.CancelTicket(ticket));      // already terminal
    TASK_CHECK(ticket->Status() == TicketStatus::Canceled);
    TASK_CHECK(session.ComputeTicket(ticket) == TicketStatus::Canceled); // no recompute
    TASK_CHECK(session.CommitTicket(ticket) == CommitResult::InvalidTicket);
    TASK_CHECK(session.CommittedFrameCount() == 0);
    delete ticket;

    // A computed ticket commits exactly once.
    FrameTicket* other = session.BeginFrame();
    TASK_CHECK(other != nullptr);
    TASK_CHECK(session.ComputeTicket(other) == TicketStatus::Computed);
    TASK_CHECK(session.CommitTicket(other) == CommitResult::Ok);
    TASK_CHECK(session.CommittedFrameCount() == 1);
    TASK_CHECK(session.CommitTicket(other) == CommitResult::InvalidTicket);
    TASK_CHECK(session.CommittedFrameCount() == 1);
    TASK_CHECK(session.ComputeTicket(other) == TicketStatus::Committed); // idempotent
    delete other;

    // Null tickets are rejected rather than crashing.
    TASK_CHECK(!session.CancelTicket(nullptr));
    TASK_CHECK(session.CommitTicket(nullptr) == CommitResult::InvalidTicket);
    TASK_CHECK(session.ComputeTicket(nullptr) == TicketStatus::Invalid);
    return 0;
}

int lifetime_session_destroyed_ticket_survives()
{
    SessionConfig config = MakeConfig();
    FrameTicket* ticket = nullptr;
    {
        MaterialSession session(config);
        ticket = session.BeginFrame();
        TASK_CHECK(ticket != nullptr);
        // Session goes away with an unterminated, not-yet-computed ticket.
    }

    TASK_CHECK(ticket->Status() == TicketStatus::SessionDead);
    TASK_CHECK(!ticket->IsCurrent());
    // The ticket owns every render resource and can still finish its work.
    TicketStatus status = ticket->Compute();
    TASK_CHECK(status == TicketStatus::SessionDead);
    std::vector<float> pixels((size_t)config.width * config.height * 3);
    TASK_CHECK(ticket->ReadPixels(pixels.data(), pixels.size()));
    TASK_CHECK(AllFiniteNonNegative(pixels));
    TASK_CHECK(PixelSum(pixels) > 0.0f);
    TASK_CHECK(ticket->RayCount() > 0);
    TASK_CHECK(ticket->Pixels() != nullptr);
    delete ticket; // releases its resources safely
    return 0;
}

int lifetime_cancel_while_running()
{
    SessionConfig big = MakeConfig();
    big.width = 128;
    big.height = 96;
    MaterialSession session(big);

    // Repeat to make it likely the cancellation lands while rows are running.
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        FrameTicket* ticket = session.BeginFrame();
        TASK_CHECK(ticket != nullptr);

        TicketStatus computeStatus = TicketStatus::Invalid;
        std::thread worker([&] { computeStatus = session.ComputeTicket(ticket); });
        TASK_CHECK(session.CancelTicket(ticket)); // must not block on the render
        worker.join();

        // Whether the flag won or the render finished first, the outcome is
        // well defined and the image/frame count are never touched.
        TASK_CHECK(computeStatus == TicketStatus::Canceled ||
                   computeStatus == TicketStatus::Computed);
        TASK_CHECK(ticket->Status() == TicketStatus::Canceled ||
                   ticket->Status() == TicketStatus::Computed);
        TASK_CHECK(session.CommitTicket(ticket) == CommitResult::InvalidTicket);
        TASK_CHECK(session.CommittedFrameCount() == 0);

        // The slot is free: a following frame can begin immediately.
        FrameTicket* next = session.BeginFrame();
        TASK_CHECK(next != nullptr);
        TASK_CHECK(session.ComputeTicket(next) == TicketStatus::Computed);
        TASK_CHECK(session.CommitTicket(next) == CommitResult::Ok);
        TASK_CHECK(session.CommittedFrameCount() == 1);
        delete ticket;
        delete next;

        // Reset the accumulation for the next attempt via a same-table publish.
        Table def = ReadDefaults(session);
        std::vector<MaterialEntry> batch = BatchFrom(def.mats);
        TASK_CHECK(session.Publish(batch.data(), batch.size()) == PublishStatus::Ok);
    }
    return 0;
}

int lifetime_cross_session_ticket_rejected()
{
    MaterialSession a(MakeConfig());
    MaterialSession b(MakeConfig());
    FrameTicket* ticketA = a.BeginFrame();
    TASK_CHECK(ticketA != nullptr);

    // The ticket is meaningless to the other session.
    TASK_CHECK(b.ComputeTicket(ticketA) == TicketStatus::Invalid);
    TASK_CHECK(b.CommitTicket(ticketA) == CommitResult::InvalidTicket);
    TASK_CHECK(!b.CancelTicket(ticketA));

    // The owning session can still use it normally.
    TASK_CHECK(a.ComputeTicket(ticketA) == TicketStatus::Computed);
    TASK_CHECK(a.CommitTicket(ticketA) == CommitResult::Ok);
    delete ticketA;
    return 0;
}

// ===========================================================================
// rendering
// ===========================================================================

int rendering_first_frame_writes_linear_rgb()
{
    MaterialSession session(MakeConfig());
    FrameTicket* ticket = session.BeginFrame();
    TASK_CHECK(ticket != nullptr);
    TASK_CHECK(ticket->Status() == TicketStatus::Pending);
    TASK_CHECK(session.ComputeTicket(ticket) == TicketStatus::Computed);
    TASK_CHECK(ticket->Status() == TicketStatus::Computed);
    TASK_CHECK(ticket->RayCount() > (int)(kWidth * kHeight));

    const size_t pixelCount = (size_t)kWidth * kHeight * 3;
    std::vector<float> privatePixels(pixelCount);
    TASK_CHECK(ticket->ReadPixels(privatePixels.data(), privatePixels.size()));
    TASK_CHECK(AllFiniteNonNegative(privatePixels));
    TASK_CHECK(PixelSum(privatePixels) > 0.0f);

    // Before commit the accumulation image is still empty.
    std::vector<float> accum(pixelCount, -1.0f);
    TASK_CHECK(session.ReadAccumulation(accum.data(), accum.size()) == 0);
    TASK_CHECK(PixelSum(accum) == 0.0f);

    TASK_CHECK(session.CommitTicket(ticket) == CommitResult::Ok);
    TASK_CHECK(session.ReadAccumulation(accum.data(), accum.size()) == 1);
    TASK_CHECK(AllFiniteNonNegative(accum));
    for (size_t i = 0; i < pixelCount; ++i)
        TASK_CHECK(std::fabs(accum[i] - privatePixels[i]) <= kEps);
    delete ticket;
    return 0;
}

int rendering_two_frame_average_matches_snapshot_frames()
{
    MaterialSession session(MakeConfig());
    const size_t pixelCount = (size_t)kWidth * kHeight * 3;

    FrameTicket* t0 = session.BeginFrame();
    TASK_CHECK(session.ComputeTicket(t0) == TicketStatus::Computed);
    std::vector<float> frame0(pixelCount);
    TASK_CHECK(t0->ReadPixels(frame0.data(), frame0.size()));
    TASK_CHECK(session.CommitTicket(t0) == CommitResult::Ok);
    delete t0;

    FrameTicket* t1 = session.BeginFrame();
    TASK_CHECK(session.ComputeTicket(t1) == TicketStatus::Computed);
    std::vector<float> frame1(pixelCount);
    TASK_CHECK(t1->ReadPixels(frame1.data(), frame1.size()));
    TASK_CHECK(session.CommitTicket(t1) == CommitResult::Ok);
    delete t1;

    TASK_CHECK(session.CommittedFrameCount() == 2);
    std::vector<float> accum(pixelCount);
    TASK_CHECK(session.ReadAccumulation(accum.data(), accum.size()) == 2);

    // The two frames use distinct per-frame random streams.
    int differing = 0;
    for (size_t i = 0; i < pixelCount; ++i)
        if (std::fabs(frame0[i] - frame1[i]) > kEps)
            ++differing;
    TASK_CHECK(differing > 0);

    // Running the same fixed snapshot sequentially must equal the per-frame
    // mean produced by the session's accumulation rule.
    for (size_t i = 0; i < pixelCount; ++i)
    {
        float expected = (frame0[i] + frame1[i]) * 0.5f;
        TASK_CHECK(std::fabs(accum[i] - expected) <= kEps);
    }
    return 0;
}

int rendering_publish_resets_image_and_frame_number()
{
    MaterialSession session(MakeConfig());
    Table def = ReadDefaults(session);
    const size_t pixelCount = (size_t)kWidth * kHeight * 3;

    FrameTicket* t0 = session.BeginFrame();
    TASK_CHECK(session.ComputeTicket(t0) == TicketStatus::Computed);
    TASK_CHECK(session.CommitTicket(t0) == CommitResult::Ok);
    FrameTicket* t1 = session.BeginFrame();
    TASK_CHECK(session.ComputeTicket(t1) == TicketStatus::Computed);
    TASK_CHECK(session.CommitTicket(t1) == CommitResult::Ok);
    delete t0;
    delete t1;
    TASK_CHECK(session.CommittedFrameCount() == 2);

    std::vector<MaterialEntry> batch = BatchFrom(def.mats);
    TASK_CHECK(session.Publish(batch.data(), batch.size()) == PublishStatus::Ok);
    TASK_CHECK(session.CommittedFrameCount() == 0);

    std::vector<float> cleared(pixelCount, -1.0f);
    TASK_CHECK(session.ReadAccumulation(cleared.data(), cleared.size()) == 0);
    TASK_CHECK(PixelSum(cleared) == 0.0f);

    // The new generation's first frame uses frame number zero (lerp factor 0),
    // so the accumulation equals that single frame exactly.
    FrameTicket* first = session.BeginFrame();
    TASK_CHECK(first->Generation() == 1);
    TASK_CHECK(session.ComputeTicket(first) == TicketStatus::Computed);
    std::vector<float> frame(pixelCount);
    TASK_CHECK(first->ReadPixels(frame.data(), frame.size()));
    TASK_CHECK(session.CommitTicket(first) == CommitResult::Ok);
    TASK_CHECK(session.CommittedFrameCount() == 1);
    std::vector<float> accum(pixelCount);
    TASK_CHECK(session.ReadAccumulation(accum.data(), accum.size()) == 1);
    for (size_t i = 0; i < pixelCount; ++i)
        TASK_CHECK(std::fabs(accum[i] - frame[i]) <= kEps);
    delete first;
    return 0;
}

int rendering_emissive_change_changes_direct_light()
{
    // Two independent sessions: the only difference is the warm light's
    // strength. The emissive sphere stays in the light list, so the RNG stream
    // and shadow rays are identical and any difference is real direct light.
    MaterialSession defaultsSession(MakeConfig());
    Table def = ReadDefaults(defaultsSession);

    MaterialSession baseline(MakeConfig());
    std::vector<float> basePixels = RenderOneFrame(baseline);
    TASK_CHECK(!basePixels.empty());
    TASK_CHECK(AllFiniteNonNegative(basePixels));

    std::vector<MaterialParams> brighter = def.mats;
    TASK_CHECK(brighter[8].emissive[0] > 0.0f);
    for (int c = 0; c < 3; ++c)
        brighter[8].emissive[c] *= 10.0f;
    std::vector<MaterialEntry> batch = BatchFrom(brighter);

    MaterialSession lit(MakeConfig());
    PublishOutput output;
    TASK_CHECK(lit.Publish(batch.data(), batch.size(), &output) == PublishStatus::Ok);
    TASK_CHECK(output.snapshot.emissiveCount == 2); // still two emissive spheres
    std::vector<float> brightPixels = RenderOneFrame(lit);
    TASK_CHECK(!brightPixels.empty());
    TASK_CHECK(AllFiniteNonNegative(brightPixels));

    float baseSum = PixelSum(basePixels);
    float brightSum = PixelSum(brightPixels);
    TASK_CHECK(baseSum > 0.0f);
    TASK_CHECK(brightSum > baseSum * 1.5f);

    // At least one pixel actually receives more energy; this is a per-pixel
    // lighting assertion, not just a version or non-black check.
    int increased = 0;
    for (size_t i = 0; i < basePixels.size(); ++i)
        if (brightPixels[i] > basePixels[i] + kEps)
            ++increased;
    TASK_CHECK(increased > 0);

    // Turning the light off (separate independent session) darkens the frame.
    std::vector<MaterialParams> darker = def.mats;
    for (int c = 0; c < 3; ++c)
        darker[8].emissive[c] = 0.0f;
    std::vector<MaterialEntry> darkBatch = BatchFrom(darker);
    MaterialSession dim(MakeConfig());
    TASK_CHECK(dim.Publish(darkBatch.data(), darkBatch.size()) == PublishStatus::Ok);
    std::vector<float> dimPixels = RenderOneFrame(dim);
    TASK_CHECK(!dimPixels.empty());
    TASK_CHECK(PixelSum(dimPixels) < baseSum);
    return 0;
}

int rendering_two_sessions_run_in_parallel()
{
    constexpr int frames = 3;
    auto renderSession = [](std::vector<float>& outPixels, int& outFrameCount,
                            std::atomic<int>& outRays) {
        MaterialSession session(MakeConfig());
        for (int f = 0; f < frames; ++f)
        {
            FrameTicket* ticket = session.BeginFrame();
            session.ComputeTicket(ticket);
            if (session.CommitTicket(ticket) != CommitResult::Ok)
            {
                delete ticket;
                return;
            }
            outRays += ticket->RayCount();
            delete ticket;
        }
        outPixels.resize((size_t)session.Width() * session.Height() * 3);
        outFrameCount = session.ReadAccumulation(outPixels.data(), outPixels.size());
    };

    std::vector<float> pixelsA, pixelsB;
    int framesA = 0, framesB = 0;
    std::atomic<int> raysA(0), raysB(0);
    std::thread threadA(renderSession, std::ref(pixelsA), std::ref(framesA), std::ref(raysA));
    std::thread threadB(renderSession, std::ref(pixelsB), std::ref(framesB), std::ref(raysB));
    threadA.join();
    threadB.join();

    TASK_CHECK(framesA == frames);
    TASK_CHECK(framesB == frames);
    TASK_CHECK(raysA.load() > 0);
    TASK_CHECK(raysB.load() > 0);
    TASK_CHECK(pixelsA.size() == pixelsB.size());
    TASK_CHECK(AllFiniteNonNegative(pixelsA));
    TASK_CHECK(AllFiniteNonNegative(pixelsB));
    // Deterministic, independent sessions must agree pixel for pixel.
    for (size_t i = 0; i < pixelsA.size(); ++i)
        TASK_CHECK(std::fabs(pixelsA[i] - pixelsB[i]) <= kEps);
    return 0;
}

int rendering_publish_concurrent_with_compute()
{
    SessionConfig big = MakeConfig();
    big.width = 128;
    big.height = 96;
    MaterialSession session(big);
    Table def = ReadDefaults(session);
    std::vector<MaterialEntry> good = BatchFrom(def.mats);
    std::vector<MaterialEntry> bad = BatchFrom(def.mats);
    bad[3].params.type = 42; // illegal type, must always be rejected

    FrameTicket* ticket = session.BeginFrame();
    TASK_CHECK(ticket != nullptr);
    TASK_CHECK(ticket->Generation() == 0);

    TicketStatus computeStatus = TicketStatus::Invalid;
    std::thread worker([&] { computeStatus = session.ComputeTicket(ticket); });

    // Publications (valid and invalid) run while the worker threads render.
    uint64_t lastGen = 0;
    for (int i = 0; i < 8; ++i)
    {
        TASK_CHECK(session.Publish(bad.data(), bad.size()) == PublishStatus::InvalidArgument);
        std::vector<MaterialEntry> next = good;
        next[1].params.albedo[0] = 0.1f + 0.05f * i;
        TASK_CHECK(session.Publish(next.data(), next.size()) == PublishStatus::Ok);
        lastGen = session.CurrentGeneration();
    }
    TASK_CHECK(lastGen == 8);

    worker.join();
    TASK_CHECK(computeStatus == TicketStatus::Computed); // captured gen 0 still rendered
    TASK_CHECK(ticket->Generation() == 0);
    TASK_CHECK(!ticket->IsCurrent());

    // The stale ticket must not touch the image or frame count, and it keeps
    // holding the single active-ticket slot until explicitly ended.
    TASK_CHECK(session.BeginFrame() == nullptr);
    TASK_CHECK(session.CommitTicket(ticket) == CommitResult::Stale);
    TASK_CHECK(session.CommittedFrameCount() == 0);
    session.ReleaseTicket(ticket);
    delete ticket;

    // A fresh frame on the new generation renders and commits normally.
    FrameTicket* fresh = session.BeginFrame();
    TASK_CHECK(fresh != nullptr);
    TASK_CHECK(fresh->Generation() == 8);
    TASK_CHECK(session.ComputeTicket(fresh) == TicketStatus::Computed);
    TASK_CHECK(session.CommitTicket(fresh) == CommitResult::Ok);
    TASK_CHECK(session.CommittedFrameCount() == 1);
    std::vector<float> accum((size_t)big.width * big.height * 3);
    TASK_CHECK(session.ReadAccumulation(accum.data(), accum.size()) == 1);
    TASK_CHECK(AllFiniteNonNegative(accum));
    TASK_CHECK(PixelSum(accum) > 0.0f);
    delete fresh;
    return 0;
}

} // namespace

// ===========================================================================
// Harness registration
// ===========================================================================

extern const struct task_case task_cases[] = {
    {"publication", "default_table", publication_default_table},
    {"publication", "valid_batch_generations", publication_valid_batch_generations},
    {"publication", "rejects_every_invalid_kind", publication_rejects_every_invalid_kind},
    {"publication", "copies_input_and_survives_mutation", publication_copies_input_and_survives_mutation},

    {"lifetime", "single_active_ticket", lifetime_single_active_ticket},
    {"lifetime", "publish_between_begin_and_commit", lifetime_publish_between_begin_and_commit},
    {"lifetime", "failed_publish_keeps_ticket_valid", lifetime_failed_publish_keeps_ticket_valid},
    {"lifetime", "cancel_and_repeat_commit", lifetime_cancel_and_repeat_commit},
    {"lifetime", "session_destroyed_ticket_survives", lifetime_session_destroyed_ticket_survives},
    {"lifetime", "cancel_while_running", lifetime_cancel_while_running},
    {"lifetime", "cross_session_ticket_rejected", lifetime_cross_session_ticket_rejected},

    {"rendering", "first_frame_writes_linear_rgb", rendering_first_frame_writes_linear_rgb},
    {"rendering", "two_frame_average_matches_snapshot_frames", rendering_two_frame_average_matches_snapshot_frames},
    {"rendering", "publish_resets_image_and_frame_number", rendering_publish_resets_image_and_frame_number},
    {"rendering", "emissive_change_changes_direct_light", rendering_emissive_change_changes_direct_light},
    {"rendering", "two_sessions_run_in_parallel", rendering_two_sessions_run_in_parallel},
    {"rendering", "publish_concurrent_with_compute", rendering_publish_concurrent_with_compute},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
