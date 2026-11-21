// tests/async_pose.cpp - task cases for the background IQM pose service.
//
// Cases are registered in three groups (numerics, publication, lifecycle)
// through task_cases/task_case_count; main() lives in task_support/driver.c.
// All concurrency orderings are produced with mutex/condition_variable
// handshakes (the Gate helper below), never with sleeps.

#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "task_support/check.h"

#include "demo/async_pose.h"
#include "tests/iqm_fixture.h"

using asyncpose::CharacterId;
using asyncpose::PoseSnapshot;
using asyncpose::RequestStatus;
using asyncpose::Ticket;

namespace
{

constexpr float kTol = 2e-5f;

// --------------------------------------------------------------------------
// Numeric comparison helpers.
// --------------------------------------------------------------------------

bool vec3_arrays_close(const std::vector<Vec3> &a, const std::vector<Vec3> &b, float tol)
{
    if(a.size() != b.size()) return false;
    for(size_t i = 0; i < a.size(); i++)
        for(int k = 0; k < 3; k++)
            if(std::fabs(a[i][k] - b[i][k]) > tol) return false;
    return true;
}

bool vec3_prefix_equal(const std::vector<Vec3> &a, const std::vector<Vec3> &b, float tol)
{
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for(size_t i = 0; i < n; i++)
        for(int k = 0; k < 3; k++)
            if(std::fabs(a[i][k] - b[i][k]) > tol) return false;
    return true;
}

bool joints_close(const std::vector<Matrix3x4> &a, const std::vector<Matrix3x4> &b, float tol)
{
    if(a.size() != b.size()) return false;
    for(size_t i = 0; i < a.size(); i++)
        for(int k = 0; k < 4; k++)
            if(std::fabs(a[i].a[k] - b[i].a[k]) > tol ||
               std::fabs(a[i].b[k] - b[i].b[k]) > tol ||
               std::fabs(a[i].c[k] - b[i].c[k]) > tol) return false;
    return true;
}

bool snapshot_matches(const PoseSnapshot &snap, const fixture::ReferenceOutput &ref, float tol = kTol)
{
    return joints_close(snap.joints, ref.joints, tol) &&
           vec3_arrays_close(snap.positions, ref.positions, tol) &&
           vec3_arrays_close(snap.normals, ref.normals, tol) &&
           vec3_arrays_close(snap.tangents, ref.tangents, tol) &&
           vec3_arrays_close(snap.bitangents, ref.bitangents, tol);
}

// --------------------------------------------------------------------------
// Serial path: the shared sampling + skinning core fed from the independent
// fixture data (its own joints and pre-concatenated frame matrices).
// --------------------------------------------------------------------------

struct SerialOutput
{
    bool ok = false;
    std::vector<Matrix3x4> joints;
    std::vector<float> positions, normals, tangents, bitangents;
};

SerialOutput run_serial(const fixture::ReferenceRig &ref, float frame)
{
    const fixture::Rig &rig = ref.built.rig;
    const int n = (int)rig.vertices.size();
    std::vector<float> pos(3*n), nrm(3*n), tan(4*n);
    std::vector<unsigned char> idx(4*n), wgt(4*n);
    for(int i = 0; i < n; i++)
    {
        const fixture::VertexSpec &v = rig.vertices[i];
        pos[3*i+0] = v.position.x; pos[3*i+1] = v.position.y; pos[3*i+2] = v.position.z;
        nrm[3*i+0] = v.normal.x;   nrm[3*i+1] = v.normal.y;   nrm[3*i+2] = v.normal.z;
        tan[4*i+0] = v.tangent.x;  tan[4*i+1] = v.tangent.y;
        tan[4*i+2] = v.tangent.z;  tan[4*i+3] = v.tangent.w;
        for(int k = 0; k < 4; k++) { idx[4*i+k] = v.index[k]; wgt[4*i+k] = v.weight[k]; }
    }

    asyncpose::SkinSource src;
    src.frames = ref.frames.data();
    src.numframes = (int)rig.frames.size();
    src.joints = ref.built.joints.data();
    src.numjoints = (int)rig.joints.size();
    src.inposition = pos.data();
    src.innormal = nrm.data();
    src.intangent = tan.data();
    src.inblendindex = idx.data();
    src.inblendweight = wgt.data();
    src.numverts = n;

    SerialOutput out;
    out.joints.resize(rig.joints.size());
    out.positions.resize(3*n);
    out.normals.resize(3*n);
    out.tangents.resize(3*n);
    out.bitangents.resize(3*n);
    asyncpose::SkinDest dst;
    dst.outframe = out.joints.data();
    dst.outposition = out.positions.data();
    dst.outnormal = out.normals.data();
    dst.outtangent = out.tangents.data();
    dst.outbitangent = out.bitangents.data();
    out.ok = asyncpose::sample_and_skin(src, frame, dst);
    return out;
}

// Compares an async snapshot against the serial path element-wise.
bool async_serial_match(asyncpose::PoseService &service, CharacterId id,
                        const fixture::ReferenceRig &ref, float frame, float tol = kTol)
{
    Ticket ticket = service.submit(id, frame);
    if(!ticket) return false;
    std::shared_ptr<const PoseSnapshot> snap;
    if(service.wait(id, ticket, &snap) != RequestStatus::published || !snap) return false;
    SerialOutput serial = run_serial(ref, frame);
    if(!serial.ok) return false;
    if(snap->joints.size() != serial.joints.size()) return false;
    for(size_t i = 0; i < snap->joints.size(); i++)
        for(int k = 0; k < 4; k++)
            if(std::fabs(snap->joints[i].a[k] - serial.joints[i].a[k]) > tol ||
               std::fabs(snap->joints[i].b[k] - serial.joints[i].b[k]) > tol ||
               std::fabs(snap->joints[i].c[k] - serial.joints[i].c[k]) > tol) return false;
    const std::vector<Vec3> *snaparrays[] =
        { &snap->positions, &snap->normals, &snap->tangents, &snap->bitangents };
    const std::vector<float> *serialarrays[] =
        { &serial.positions, &serial.normals, &serial.tangents, &serial.bitangents };
    for(int a = 0; a < 4; a++)
    {
        if(snaparrays[a]->size() * 3 != serialarrays[a]->size()) return false;
        for(size_t i = 0; i < snaparrays[a]->size(); i++)
            for(int k = 0; k < 3; k++)
                if(std::fabs((*snaparrays[a])[i][k] - (*serialarrays[a])[3*i+k]) > tol) return false;
    }
    return true;
}

// Submits one frame, waits for publication and compares the snapshot with
// the independent reference.
bool expect_published(asyncpose::PoseService &service, CharacterId id,
                      const fixture::ReferenceRig &ref, float frame, float tol = kTol)
{
    Ticket ticket = service.submit(id, frame);
    if(!ticket) return false;
    std::shared_ptr<const PoseSnapshot> snap;
    if(service.wait(id, ticket, &snap) != RequestStatus::published || !snap) return false;
    if(snap->ticket != ticket || snap->frame != frame) return false;
    return snapshot_matches(*snap, fixture::evaluate_reference(ref, frame), tol);
}

// --------------------------------------------------------------------------
// Temp files for the programmatically built IQM assets.
// --------------------------------------------------------------------------

std::string write_temp_rig(const char *name, const fixture::BuiltRig &built)
{
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if(ec) dir = ".";
    const std::string path = (dir / name).string();
    return fixture::write_file(path, built.filebytes) ? path : std::string();
}

// --------------------------------------------------------------------------
// Gate: a mutex/condition_variable handshake used to pause one evaluation
// inside the eval callback until the test releases it.  GateGuard releases
// every gate on scope exit (before the service is destroyed) so a failed
// check can never wedge the worker pool.
// --------------------------------------------------------------------------

struct Gate
{
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false;
    bool open = false;

    // Called on a worker thread from the eval callback.
    void pass()
    {
        std::unique_lock<std::mutex> lock(mu);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [this] { return open; });
    }
    void wait_entered()
    {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait(lock, [this] { return entered; });
    }
    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mu);
            open = true;
        }
        cv.notify_all();
    }
};

struct GateGuard
{
    std::vector<Gate *> gates;
    ~GateGuard() { for(Gate *g : gates) g->release(); }
    void add(Gate &g) { gates.push_back(&g); }
};

// A thread that joins on destruction, so a failed check cannot leave a
// joinable thread behind (which would call std::terminate).
struct joining_thread
{
    std::thread t;
    joining_thread() {}
    template<class F> explicit joining_thread(F &&f) : t(std::forward<F>(f)) {}
    ~joining_thread() { if(t.joinable()) t.join(); }
    void join() { if(t.joinable()) t.join(); }
};

// --------------------------------------------------------------------------
// numerics
// --------------------------------------------------------------------------

int case_asset_reference_match()
{
    fixture::ReferenceRig ref = fixture::prepare_rig(fixture::make_rig_alpha());
    TASK_CHECK(ref.built.joints.size() == 3 && ref.built.rig.frames.size() == 4);
    const std::string path = write_temp_rig("async_pose_test_alpha.iqm", ref.built);
    TASK_CHECK(!path.empty());
    auto asset = asyncpose::load_pose_asset(path);
    TASK_CHECK(asset != nullptr);
    if(!asset) return 1;
    TASK_CHECK(asset->numjoints() == 3 && asset->numframes() == 4 && asset->numverts() == 5);

    // The asset owns its data: corrupt and delete the file it was loaded
    // from, then keep evaluating against the reference.
    TASK_CHECK(fixture::write_file(path, std::vector<unsigned char>(ref.built.filebytes.size(), 0xAB)));
    std::remove(path.c_str());

    asyncpose::PoseService service(2);
    CharacterId id = service.create_character(asset);
    TASK_CHECK(id.valid());

    const float frames[] = { 0.0f, 0.5f, 1.25f, 2.9f, 3.99f };
    for(float f : frames) TASK_CHECK(expect_published(service, id, ref, f));
    for(float f : frames) TASK_CHECK(async_serial_match(service, id, ref, f));

    service.shutdown();
    return 0;
}

int case_negative_and_wrapped_frames()
{
    fixture::ReferenceRig ref = fixture::prepare_rig(fixture::make_rig_beta());
    const std::string path = write_temp_rig("async_pose_test_beta.iqm", ref.built);
    TASK_CHECK(!path.empty());
    auto asset = asyncpose::load_pose_asset(path);
    TASK_CHECK(asset != nullptr);
    if(!asset) return 1;
    std::remove(path.c_str());

    asyncpose::PoseService service(2);
    CharacterId id = service.create_character(asset);
    TASK_CHECK(id.valid());

    // Negative and out-of-range frames wrap with a positive modulo over the
    // whole animation frame array.
    const float frames[] = { -0.5f, -1.25f, -2.99f, -3.0f, -4.5f, -8.0f, 3.5f, 7.5f, 100.25f, -100.75f };
    for(float f : frames) TASK_CHECK(expect_published(service, id, ref, f));
    for(float f : frames) TASK_CHECK(async_serial_match(service, id, ref, f));

    // Rejected submissions leave the snapshot and accepted requests alone.
    auto current = service.snapshot(id);
    TASK_CHECK(current != nullptr);
    TASK_CHECK(service.submit(id, NAN) == 0);
    TASK_CHECK(service.submit(id, INFINITY) == 0);
    TASK_CHECK(service.submit(id, -INFINITY) == 0);
    TASK_CHECK(service.submit(CharacterId(), 1.0f) == 0);
    TASK_CHECK(service.snapshot(id).get() == current.get());

    service.shutdown();
    return 0;
}

int case_two_distinct_assets()
{
    fixture::ReferenceRig refa = fixture::prepare_rig(fixture::make_rig_alpha());
    fixture::ReferenceRig refb = fixture::prepare_rig(fixture::make_rig_beta());
    const std::string patha = write_temp_rig("async_pose_test_alpha2.iqm", refa.built);
    const std::string pathb = write_temp_rig("async_pose_test_beta2.iqm", refb.built);
    TASK_CHECK(!patha.empty() && !pathb.empty());
    auto asseta = asyncpose::load_pose_asset(patha);
    auto assetb = asyncpose::load_pose_asset(pathb);
    TASK_CHECK(asseta != nullptr && assetb != nullptr);
    if(!asseta || !assetb) return 1;
    std::remove(patha.c_str());
    std::remove(pathb.c_str());
    TASK_CHECK(asseta->numjoints() != assetb->numjoints());
    TASK_CHECK(asseta->numverts() != assetb->numverts());
    TASK_CHECK(asseta->numframes() != assetb->numframes());

    asyncpose::PoseService service(4);
    CharacterId a = service.create_character(asseta);
    CharacterId b = service.create_character(assetb);
    TASK_CHECK(a.valid() && b.valid());
    TASK_CHECK(a != b);

    // Same absolute frame submitted to both characters of two different
    // assets; both get their own ticket 1 and their own correct pose.
    Ticket ta = service.submit(a, 1.5f);
    Ticket tb = service.submit(b, 1.5f);
    TASK_CHECK(ta == 1 && tb == 1);
    std::shared_ptr<const PoseSnapshot> snapa, snapb;
    TASK_CHECK(service.wait(a, ta, &snapa) == RequestStatus::published);
    TASK_CHECK(service.wait(b, tb, &snapb) == RequestStatus::published);
    TASK_CHECK(snapa && snapb);
    if(!snapa || !snapb) return 1;
    TASK_CHECK(snapshot_matches(*snapa, fixture::evaluate_reference(refa, 1.5f)));
    TASK_CHECK(snapshot_matches(*snapb, fixture::evaluate_reference(refb, 1.5f)));
    // The two assets produce genuinely different poses.
    TASK_CHECK(!vec3_prefix_equal(snapa->positions, snapb->positions, kTol));
    TASK_CHECK(!vec3_prefix_equal(snapa->normals, snapb->normals, kTol));

    service.shutdown();
    return 0;
}

// --------------------------------------------------------------------------
// publication
// --------------------------------------------------------------------------

int case_overlap_two_workers()
{
    fixture::ReferenceRig refa = fixture::prepare_rig(fixture::make_rig_alpha());
    fixture::ReferenceRig refb = fixture::prepare_rig(fixture::make_rig_beta());
    const std::string patha = write_temp_rig("async_pose_test_oa.iqm", refa.built);
    const std::string pathb = write_temp_rig("async_pose_test_ob.iqm", refb.built);
    TASK_CHECK(!patha.empty() && !pathb.empty());
    auto asseta = asyncpose::load_pose_asset(patha);
    auto assetb = asyncpose::load_pose_asset(pathb);
    TASK_CHECK(asseta != nullptr && assetb != nullptr);
    if(!asseta || !assetb) return 1;
    std::remove(patha.c_str());
    std::remove(pathb.c_str());

    // Gates are declared before the service so they are released (by the
    // guard) and destroyed only after the service has joined its workers.
    Gate gate_a;
    asyncpose::PoseService service(2);
    GateGuard guard;
    guard.add(gate_a);
    CharacterId a = service.create_character(asseta);
    CharacterId b = service.create_character(assetb);
    TASK_CHECK(a.valid() && b.valid());

    // Worker evaluating character A pauses inside the eval callback.
    service.set_eval_callback([a, &gate_a](CharacterId id, Ticket ticket) {
        if(id == a && ticket == 1) gate_a.pass();
    });

    Ticket ta = service.submit(a, 0.5f);
    TASK_CHECK(ta == 1);
    gate_a.wait_entered(); // A is now inside production evaluation on worker 1

    // Two characters may hold the same ticket number independently.
    Ticket tb = service.submit(b, 2.5f);
    TASK_CHECK(tb == 1);
    TASK_CHECK(ta == tb);

    // B evaluates and publishes on worker 2 while A is still paused: the two
    // tasks overlapped.
    std::shared_ptr<const PoseSnapshot> snapb;
    TASK_CHECK(service.wait(b, tb, &snapb) == RequestStatus::published);
    TASK_CHECK(snapb && snapshot_matches(*snapb, fixture::evaluate_reference(refb, 2.5f)));
    TASK_CHECK(service.poll(a, ta) == RequestStatus::pending);

    gate_a.release();
    std::shared_ptr<const PoseSnapshot> snapa;
    TASK_CHECK(service.wait(a, ta, &snapa) == RequestStatus::published);
    TASK_CHECK(snapa && snapshot_matches(*snapa, fixture::evaluate_reference(refa, 0.5f)));

    // Tickets are strictly increasing per character.
    TASK_CHECK(service.submit(a, 1.0f) == 2);
    TASK_CHECK(service.submit(b, 1.0f) == 2);

    service.shutdown();
    return 0;
}

int case_old_request_finishes_last()
{
    fixture::ReferenceRig ref = fixture::prepare_rig(fixture::make_rig_alpha());
    const std::string path = write_temp_rig("async_pose_test_or.iqm", ref.built);
    TASK_CHECK(!path.empty());
    auto asset = asyncpose::load_pose_asset(path);
    TASK_CHECK(asset != nullptr);
    if(!asset) return 1;
    std::remove(path.c_str());

    Gate gate_old;
    asyncpose::PoseService service(2);
    GateGuard guard;
    guard.add(gate_old);
    CharacterId a = service.create_character(asset);
    TASK_CHECK(a.valid());
    service.set_eval_callback([a, &gate_old](CharacterId id, Ticket ticket) {
        if(id == a && ticket == 1) gate_old.pass();
    });

    Ticket told = service.submit(a, 0.5f);
    TASK_CHECK(told == 1);
    gate_old.wait_entered(); // old request computed, paused before publishing

    // A newer request is accepted while the old one is still in flight.
    Ticket tnew = service.submit(a, 2.5f);
    TASK_CHECK(tnew == 2);
    std::shared_ptr<const PoseSnapshot> snapnew;
    TASK_CHECK(service.wait(a, tnew, &snapnew) == RequestStatus::published);
    TASK_CHECK(snapnew && snapshot_matches(*snapnew, fixture::evaluate_reference(ref, 2.5f)));
    TASK_CHECK(service.poll(a, told) == RequestStatus::pending);

    // The old request finishes last: it must not overwrite the newer frame.
    gate_old.release();
    TASK_CHECK(service.wait(a, told) == RequestStatus::superseded);
    auto current = service.snapshot(a);
    TASK_CHECK(current && current->ticket == tnew && current->frame == 2.5f);
    TASK_CHECK(current && snapshot_matches(*current, fixture::evaluate_reference(ref, 2.5f)));

    service.shutdown();
    return 0;
}

int case_pending_superseded_chain()
{
    fixture::ReferenceRig ref = fixture::prepare_rig(fixture::make_rig_alpha());
    const std::string path = write_temp_rig("async_pose_test_pc.iqm", ref.built);
    TASK_CHECK(!path.empty());
    auto asset = asyncpose::load_pose_asset(path);
    TASK_CHECK(asset != nullptr);
    if(!asset) return 1;
    std::remove(path.c_str());

    Gate gate_a, gate_b;
    asyncpose::PoseService service(2);
    GateGuard guard;
    guard.add(gate_a);
    guard.add(gate_b);
    CharacterId a = service.create_character(asset);
    CharacterId b = service.create_character(asset);
    CharacterId c = service.create_character(asset);
    TASK_CHECK(a.valid() && b.valid() && c.valid());
    service.set_eval_callback([a, b, &gate_a, &gate_b](CharacterId id, Ticket ticket) {
        if(ticket != 1) return;
        if(id == a) gate_a.pass();
        else if(id == b) gate_b.pass();
    });

    TASK_CHECK(service.submit(a, 0.0f) == 1);
    TASK_CHECK(service.submit(b, 0.0f) == 1);
    gate_a.wait_entered();
    gate_b.wait_entered(); // both workers busy; C's requests stay queued

    // Three rapid submissions: each new one replaces the single pending
    // slot and settles the replaced ticket as superseded.
    Ticket t1 = service.submit(c, 0.0f);
    Ticket t2 = service.submit(c, 0.5f);
    Ticket t3 = service.submit(c, 1.5f);
    TASK_CHECK(t1 == 1 && t2 == 2 && t3 == 3);
    TASK_CHECK(service.poll(c, t1) == RequestStatus::superseded);
    TASK_CHECK(service.poll(c, t2) == RequestStatus::superseded);
    TASK_CHECK(service.poll(c, t3) == RequestStatus::pending);
    TASK_CHECK(service.wait(c, t1) == RequestStatus::superseded);
    TASK_CHECK(service.wait(c, t2) == RequestStatus::superseded);
    TASK_CHECK(service.snapshot(c) == nullptr);

    gate_a.release();
    gate_b.release();
    std::shared_ptr<const PoseSnapshot> snapc;
    TASK_CHECK(service.wait(c, t3, &snapc) == RequestStatus::published);
    TASK_CHECK(snapc && snapshot_matches(*snapc, fixture::evaluate_reference(ref, 1.5f)));
    TASK_CHECK(service.wait(a, 1) == RequestStatus::published);
    TASK_CHECK(service.wait(b, 1) == RequestStatus::published);

    service.shutdown();
    return 0;
}

int case_out_of_order_completion()
{
    fixture::ReferenceRig ref = fixture::prepare_rig(fixture::make_rig_alpha());
    const std::string path = write_temp_rig("async_pose_test_oo.iqm", ref.built);
    TASK_CHECK(!path.empty());
    auto asset = asyncpose::load_pose_asset(path);
    TASK_CHECK(asset != nullptr);
    if(!asset) return 1;
    std::remove(path.c_str());

    Gate gate_a, gate_b;
    asyncpose::PoseService service(3);
    GateGuard guard;
    guard.add(gate_a);
    guard.add(gate_b);
    CharacterId a = service.create_character(asset);
    CharacterId b = service.create_character(asset);
    CharacterId c = service.create_character(asset);
    service.set_eval_callback([a, b, &gate_a, &gate_b](CharacterId id, Ticket ticket) {
        if(ticket != 1) return;
        if(id == a) gate_a.pass();
        else if(id == b) gate_b.pass();
    });

    // Submitted in order A, B, C; completed in order C, B, A.
    TASK_CHECK(service.submit(a, 0.25f) == 1);
    TASK_CHECK(service.submit(b, 1.25f) == 1);
    TASK_CHECK(service.submit(c, 2.25f) == 1);
    gate_a.wait_entered();
    gate_b.wait_entered();

    std::shared_ptr<const PoseSnapshot> snapc;
    TASK_CHECK(service.wait(c, 1, &snapc) == RequestStatus::published);
    TASK_CHECK(snapc && snapshot_matches(*snapc, fixture::evaluate_reference(ref, 2.25f)));
    TASK_CHECK(service.poll(a, 1) == RequestStatus::pending);
    TASK_CHECK(service.poll(b, 1) == RequestStatus::pending);

    gate_b.release();
    std::shared_ptr<const PoseSnapshot> snapb;
    TASK_CHECK(service.wait(b, 1, &snapb) == RequestStatus::published);
    TASK_CHECK(snapb && snapshot_matches(*snapb, fixture::evaluate_reference(ref, 1.25f)));
    TASK_CHECK(service.poll(a, 1) == RequestStatus::pending);

    gate_a.release();
    std::shared_ptr<const PoseSnapshot> snapa;
    TASK_CHECK(service.wait(a, 1, &snapa) == RequestStatus::published);
    TASK_CHECK(snapa && snapshot_matches(*snapa, fixture::evaluate_reference(ref, 0.25f)));

    service.shutdown();
    return 0;
}

int case_snapshot_lifetime()
{
    fixture::ReferenceRig ref = fixture::prepare_rig(fixture::make_rig_alpha());
    const std::string path = write_temp_rig("async_pose_test_sl.iqm", ref.built);
    TASK_CHECK(!path.empty());
    auto asset = asyncpose::load_pose_asset(path);
    TASK_CHECK(asset != nullptr);
    if(!asset) return 1;
    std::remove(path.c_str());

    std::shared_ptr<const PoseSnapshot> s1, s2;
    {
        asyncpose::PoseService service(2);
        CharacterId a = service.create_character(asset);
        Ticket t1 = service.submit(a, 0.5f);
        Ticket t2 = 0;
        TASK_CHECK(t1 == 1);
        TASK_CHECK(service.wait(a, t1) == RequestStatus::published);
        s1 = service.snapshot(a);
        TASK_CHECK(s1 != nullptr);

        t2 = service.submit(a, 2.5f);
        TASK_CHECK(t2 == 2);
        TASK_CHECK(service.wait(a, t2) == RequestStatus::published);
        s2 = service.snapshot(a);
        TASK_CHECK(s2 != nullptr);
        TASK_CHECK(s1 != s2);
        TASK_CHECK(s1->ticket == t1 && s1->frame == 0.5f);
        TASK_CHECK(s2->ticket == t2 && s2->frame == 2.5f);

        // A published ticket keeps its successful terminal state and its own
        // snapshot even after a newer request replaces it.
        std::shared_ptr<const PoseSnapshot> out1, out2;
        TASK_CHECK(service.wait(a, t1, &out1) == RequestStatus::published);
        TASK_CHECK(service.wait(a, t2, &out2) == RequestStatus::published);
        TASK_CHECK(out1.get() == s1.get());
        TASK_CHECK(out2.get() == s2.get());

        // Long-held snapshots stay numerically intact after newer publishes,
        // unregistration and shutdown.
        service.unregister_character(a);
        TASK_CHECK(snapshot_matches(*s1, fixture::evaluate_reference(ref, 0.5f)));
        TASK_CHECK(snapshot_matches(*s2, fixture::evaluate_reference(ref, 2.5f)));
        service.shutdown();
        TASK_CHECK(snapshot_matches(*s1, fixture::evaluate_reference(ref, 0.5f)));
    }
    // The service is gone; the snapshots are still fully readable.
    TASK_CHECK(snapshot_matches(*s1, fixture::evaluate_reference(ref, 0.5f)));
    TASK_CHECK(snapshot_matches(*s2, fixture::evaluate_reference(ref, 2.5f)));
    return 0;
}

int case_failed_then_continue()
{
    fixture::ReferenceRig ref = fixture::prepare_rig(fixture::make_rig_alpha());
    const std::string path = write_temp_rig("async_pose_test_ft.iqm", ref.built);
    TASK_CHECK(!path.empty());
    auto asset = asyncpose::load_pose_asset(path);
    TASK_CHECK(asset != nullptr);
    if(!asset) return 1;
    std::remove(path.c_str());

    asyncpose::PoseService service(2);
    CharacterId a = service.create_character(asset);
    CharacterId b = service.create_character(asset);
    TASK_CHECK(a.valid() && b.valid());
    service.set_eval_callback([a](CharacterId id, Ticket ticket) {
        if(id == a && ticket == 2) throw std::runtime_error("eval callback boom");
    });

    Ticket t1 = service.submit(a, 0.5f);
    TASK_CHECK(t1 == 1);
    TASK_CHECK(service.wait(a, t1) == RequestStatus::published);

    // The callback throws for ticket 2: the request settles as failed and
    // the snapshot keeps the previous frame.
    Ticket t2 = service.submit(a, 1.5f);
    TASK_CHECK(t2 == 2);
    TASK_CHECK(service.wait(a, t2) == RequestStatus::failed);
    auto current = service.snapshot(a);
    TASK_CHECK(current && current->ticket == t1 && current->frame == 0.5f);
    TASK_CHECK(current && snapshot_matches(*current, fixture::evaluate_reference(ref, 0.5f)));

    // The service keeps working: the worker survived the exception.
    Ticket t3 = service.submit(a, 2.5f);
    TASK_CHECK(t3 == 3);
    std::shared_ptr<const PoseSnapshot> snap3;
    TASK_CHECK(service.wait(a, t3, &snap3) == RequestStatus::published);
    TASK_CHECK(snap3 && snapshot_matches(*snap3, fixture::evaluate_reference(ref, 2.5f)));
    TASK_CHECK(expect_published(service, b, ref, 1.0f));

    service.set_eval_callback(nullptr);
    service.shutdown();
    return 0;
}

// --------------------------------------------------------------------------
// lifecycle
// --------------------------------------------------------------------------

int case_unregister_inflight()
{
    fixture::ReferenceRig refa = fixture::prepare_rig(fixture::make_rig_alpha());
    fixture::ReferenceRig refb = fixture::prepare_rig(fixture::make_rig_beta());
    const std::string patha = write_temp_rig("async_pose_test_ua.iqm", refa.built);
    const std::string pathb = write_temp_rig("async_pose_test_ub.iqm", refb.built);
    TASK_CHECK(!patha.empty() && !pathb.empty());
    auto asseta = asyncpose::load_pose_asset(patha);
    auto assetb = asyncpose::load_pose_asset(pathb);
    TASK_CHECK(asseta != nullptr && assetb != nullptr);
    if(!asseta || !assetb) return 1;
    std::remove(patha.c_str());
    std::remove(pathb.c_str());

    Gate gate_a, gate_b;
    asyncpose::PoseService service(2);
    GateGuard guard;
    guard.add(gate_a);
    guard.add(gate_b);
    CharacterId a = service.create_character(asseta);
    CharacterId b = service.create_character(assetb);
    TASK_CHECK(a.valid() && b.valid());
    service.set_eval_callback([a, b, &gate_a, &gate_b](CharacterId id, Ticket ticket) {
        if(ticket != 1) return;
        if(id == a) gate_a.pass();
        else if(id == b) gate_b.pass();
    });

    TASK_CHECK(service.submit(a, 0.5f) == 1);
    TASK_CHECK(service.submit(b, 1.5f) == 1);
    gate_a.wait_entered(); // A's ticket 1 is in flight
    gate_b.wait_entered(); // both workers busy
    Ticket ta2 = service.submit(a, 2.5f); // stays pending
    TASK_CHECK(ta2 == 2);

    // Unregister while one request is in flight and one is queued: both
    // settle as superseded and the id stops accepting submissions.
    service.unregister_character(a);
    TASK_CHECK(service.poll(a, 1) == RequestStatus::superseded);
    TASK_CHECK(service.poll(a, ta2) == RequestStatus::superseded);
    TASK_CHECK(service.submit(a, 0.0f) == 0);

    gate_a.release();
    gate_b.release();
    TASK_CHECK(service.wait(a, 1) == RequestStatus::superseded);
    TASK_CHECK(service.wait(a, ta2) == RequestStatus::superseded);
    TASK_CHECK(service.snapshot(a) == nullptr);

    // The other character is unaffected and the service keeps working.
    std::shared_ptr<const PoseSnapshot> snapb;
    TASK_CHECK(service.wait(b, 1, &snapb) == RequestStatus::published);
    TASK_CHECK(snapb && snapshot_matches(*snapb, fixture::evaluate_reference(refb, 1.5f)));
    CharacterId c = service.create_character(asseta);
    TASK_CHECK(c.valid() && c != a);
    TASK_CHECK(expect_published(service, c, refa, 0.25f));

    service.shutdown();
    return 0;
}

int case_shutdown_drains_inflight()
{
    fixture::ReferenceRig ref = fixture::prepare_rig(fixture::make_rig_alpha());
    const std::string path = write_temp_rig("async_pose_test_sd.iqm", ref.built);
    TASK_CHECK(!path.empty());
    auto asset = asyncpose::load_pose_asset(path);
    TASK_CHECK(asset != nullptr);
    if(!asset) return 1;
    std::remove(path.c_str());

    Gate gate_a, gate_b;
    asyncpose::PoseService service(2);
    GateGuard guard;
    guard.add(gate_a);
    guard.add(gate_b);
    CharacterId a = service.create_character(asset);
    CharacterId b = service.create_character(asset);
    CharacterId c = service.create_character(asset);
    service.set_eval_callback([a, b, &gate_a, &gate_b](CharacterId id, Ticket ticket) {
        if(ticket != 1) return;
        if(id == a) gate_a.pass();
        else if(id == b) gate_b.pass();
    });

    TASK_CHECK(service.submit(a, 0.5f) == 1);
    TASK_CHECK(service.submit(b, 1.5f) == 1);
    gate_a.wait_entered();
    gate_b.wait_entered(); // both workers in flight
    Ticket tc = service.submit(c, 2.5f); // queued behind the busy workers
    TASK_CHECK(tc == 1);

    // Shutdown starts while work is in flight: queued requests settle as
    // superseded and new submissions are rejected immediately.  The late
    // guard is declared after the shutdown thread so that, on any exit, the
    // gates are released before the thread is joined.
    joining_thread closer([&service] { service.shutdown(); });
    GateGuard late_guard;
    late_guard.add(gate_a);
    late_guard.add(gate_b);
    TASK_CHECK(service.wait(c, tc) == RequestStatus::superseded);
    TASK_CHECK(service.submit(c, 0.0f) == 0);

    // In-flight evaluations are drained and may still publish.
    gate_a.release();
    gate_b.release();
    closer.join();
    std::shared_ptr<const PoseSnapshot> snapa;
    TASK_CHECK(service.wait(a, 1, &snapa) == RequestStatus::published);
    TASK_CHECK(snapa && snapshot_matches(*snapa, fixture::evaluate_reference(ref, 0.5f)));
    TASK_CHECK(service.wait(b, 1) == RequestStatus::published);

    // Snapshots stay readable after shutdown; shutdown is idempotent.
    auto current = service.snapshot(a);
    TASK_CHECK(current && snapshot_matches(*current, fixture::evaluate_reference(ref, 0.5f)));
    service.shutdown();
    return 0;
}

int case_shutdown_wakes_waiters()
{
    fixture::ReferenceRig ref = fixture::prepare_rig(fixture::make_rig_alpha());
    const std::string path = write_temp_rig("async_pose_test_sw.iqm", ref.built);
    TASK_CHECK(!path.empty());
    auto asset = asyncpose::load_pose_asset(path);
    TASK_CHECK(asset != nullptr);
    if(!asset) return 1;
    std::remove(path.c_str());

    Gate gate_a, gate_b;
    asyncpose::PoseService service(2);
    GateGuard guard;
    guard.add(gate_a);
    guard.add(gate_b);
    CharacterId a = service.create_character(asset);
    CharacterId b = service.create_character(asset);
    CharacterId c = service.create_character(asset);
    service.set_eval_callback([a, b, &gate_a, &gate_b](CharacterId id, Ticket ticket) {
        if(ticket != 1) return;
        if(id == a) gate_a.pass();
        else if(id == b) gate_b.pass();
    });

    TASK_CHECK(service.submit(a, 0.5f) == 1);
    TASK_CHECK(service.submit(b, 1.5f) == 1);
    gate_a.wait_entered();
    gate_b.wait_entered();
    Ticket tc = service.submit(c, 2.5f);
    TASK_CHECK(tc == 1);

    RequestStatus st_a = RequestStatus::pending, st_c = RequestStatus::pending;
    std::shared_ptr<const PoseSnapshot> snapa;
    joining_thread waiter_a([&] { st_a = service.wait(a, 1, &snapa); });
    joining_thread waiter_c([&] { st_c = service.wait(c, tc); });
    joining_thread closer([&service] { service.shutdown(); });
    // Declared after the threads so the gates open before they are joined.
    GateGuard late_guard;
    late_guard.add(gate_a);
    late_guard.add(gate_b);

    // The waiter on the queued ticket wakes with superseded at shutdown.
    waiter_c.join();
    TASK_CHECK(st_c == RequestStatus::superseded);

    gate_a.release();
    gate_b.release();
    closer.join();
    waiter_a.join();
    TASK_CHECK(st_a == RequestStatus::published);
    TASK_CHECK(snapa && snapshot_matches(*snapa, fixture::evaluate_reference(ref, 0.5f)));
    return 0; // the destructor runs after a completed shutdown without hanging
}

int case_identity_not_reused()
{
    fixture::ReferenceRig ref = fixture::prepare_rig(fixture::make_rig_alpha());
    const std::string path = write_temp_rig("async_pose_test_id.iqm", ref.built);
    TASK_CHECK(!path.empty());
    auto asset = asyncpose::load_pose_asset(path);
    TASK_CHECK(asset != nullptr);
    if(!asset) return 1;
    std::remove(path.c_str());

    asyncpose::PoseService service(2);
    CharacterId a = service.create_character(asset);
    TASK_CHECK(a.valid());
    Ticket ta1 = service.submit(a, 1.0f);
    TASK_CHECK(ta1 == 1);
    TASK_CHECK(service.wait(a, ta1) == RequestStatus::published);
    auto snapa = service.snapshot(a);
    TASK_CHECK(snapa && snapshot_matches(*snapa, fixture::evaluate_reference(ref, 1.0f)));

    service.unregister_character(a);
    CharacterId b = service.create_character(asset);
    TASK_CHECK(b.valid());
    TASK_CHECK(b != a); // the slot is never reused for another character

    // The stale id cannot submit and cannot address the new character, but
    // its published ticket keeps its terminal state and snapshot.
    TASK_CHECK(service.submit(a, 0.5f) == 0);
    std::shared_ptr<const PoseSnapshot> out;
    TASK_CHECK(service.wait(a, ta1, &out) == RequestStatus::published);
    TASK_CHECK(out.get() == snapa.get());
    TASK_CHECK(service.snapshot(a).get() == snapa.get());
    TASK_CHECK(snapshot_matches(*out, fixture::evaluate_reference(ref, 1.0f)));

    // The new character has its own ticket sequence starting at 1.
    Ticket tb1 = service.submit(b, 2.0f);
    TASK_CHECK(tb1 == ta1);
    std::shared_ptr<const PoseSnapshot> snapb;
    TASK_CHECK(service.wait(b, tb1, &snapb) == RequestStatus::published);
    TASK_CHECK(snapb && snapshot_matches(*snapb, fixture::evaluate_reference(ref, 2.0f)));

    service.unregister_character(a); // repeated unregister is a no-op
    service.unregister_character(b);
    TASK_CHECK(service.submit(b, 0.5f) == 0);
    service.shutdown();
    return 0;
}

int case_destructor_shutdown()
{
    fixture::ReferenceRig ref = fixture::prepare_rig(fixture::make_rig_alpha());
    const std::string path = write_temp_rig("async_pose_test_ds.iqm", ref.built);
    TASK_CHECK(!path.empty());
    auto asset = asyncpose::load_pose_asset(path);
    TASK_CHECK(asset != nullptr);
    if(!asset) return 1;
    std::remove(path.c_str());

    std::shared_ptr<const PoseSnapshot> snap;
    {
        asyncpose::PoseService service(2);
        CharacterId a = service.create_character(asset);
        Ticket t = service.submit(a, 0.5f);
        TASK_CHECK(t == 1);
        TASK_CHECK(service.wait(a, t, &snap) == RequestStatus::published);
        // No explicit shutdown: the destructor drains and joins the workers.
    }
    TASK_CHECK(snap && snapshot_matches(*snap, fixture::evaluate_reference(ref, 0.5f)));
    return 0;
}

} // namespace

const task_case task_cases[] = {
    { "numerics", "asset_reference_match", &case_asset_reference_match },
    { "numerics", "negative_and_wrapped_frames", &case_negative_and_wrapped_frames },
    { "numerics", "two_distinct_assets", &case_two_distinct_assets },
    { "publication", "overlap_two_workers", &case_overlap_two_workers },
    { "publication", "old_request_finishes_last", &case_old_request_finishes_last },
    { "publication", "pending_superseded_chain", &case_pending_superseded_chain },
    { "publication", "out_of_order_completion", &case_out_of_order_completion },
    { "publication", "snapshot_lifetime", &case_snapshot_lifetime },
    { "publication", "failed_then_continue", &case_failed_then_continue },
    { "lifecycle", "unregister_inflight", &case_unregister_inflight },
    { "lifecycle", "shutdown_drains_inflight", &case_shutdown_drains_inflight },
    { "lifecycle", "shutdown_wakes_waiters", &case_shutdown_wakes_waiters },
    { "lifecycle", "identity_not_reused", &case_identity_not_reused },
    { "lifecycle", "destructor_shutdown", &case_destructor_shutdown },
};
const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
