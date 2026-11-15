#include "MaterialSession.h"

#include "Config.h"
#include "Maths.h"
#include "enkiTS/TaskScheduler.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace materialsession {

// Everything below except the public class member definitions is internal to
// this translation unit.

const float kMinT = 0.001f;
const float kMaxT = 1.0e7f;
const int kMaxDepth = 10;

// ---------------------------------------------------------------------------
// Fixed scene geometry. The session never animates or moves spheres, so the
// tables are built once and shared read-only. Each ticket still owns its own
// intersection (SoA) copy so its snapshot is self contained.
// ---------------------------------------------------------------------------

struct SceneGeometry
{
    int sphereCount = 0;
    std::vector<Sphere> spheres;
    std::vector<MaterialParams> defaultMaterials;
};

const SceneGeometry& Geometry()
{
    static const SceneGeometry geo = [] {
        SceneGeometry g;
        const Sphere sphereTable[] =
        {
            Sphere(float3(0,-100.5,-1), 100),
            Sphere(float3(2,0,-1), 0.5f),
            Sphere(float3(0,0,-1), 0.5f),
            Sphere(float3(-2,0,-1), 0.5f),
            Sphere(float3(2,0,1), 0.5f),
            Sphere(float3(0,0,1), 0.5f),
            Sphere(float3(-2,0,1), 0.5f),
            Sphere(float3(0.5f,1,0.5f), 0.5f),
            Sphere(float3(-1.5f,1.5f,0.f), 0.3f),
            Sphere(float3(4,0,-3), 0.5f), Sphere(float3(3,0,-3), 0.5f), Sphere(float3(2,0,-3), 0.5f), Sphere(float3(1,0,-3), 0.5f), Sphere(float3(0,0,-3), 0.5f), Sphere(float3(-1,0,-3), 0.5f), Sphere(float3(-2,0,-3), 0.5f), Sphere(float3(-3,0,-3), 0.5f), Sphere(float3(-4,0,-3), 0.5f),
            Sphere(float3(4,0,-4), 0.5f), Sphere(float3(3,0,-4), 0.5f), Sphere(float3(2,0,-4), 0.5f), Sphere(float3(1,0,-4), 0.5f), Sphere(float3(0,0,-4), 0.5f), Sphere(float3(-1,0,-4), 0.5f), Sphere(float3(-2,0,-4), 0.5f), Sphere(float3(-3,0,-4), 0.5f), Sphere(float3(-4,0,-4), 0.5f),
            Sphere(float3(4,0,-5), 0.5f), Sphere(float3(3,0,-5), 0.5f), Sphere(float3(2,0,-5), 0.5f), Sphere(float3(1,0,-5), 0.5f), Sphere(float3(0,0,-5), 0.5f), Sphere(float3(-1,0,-5), 0.5f), Sphere(float3(-2,0,-5), 0.5f), Sphere(float3(-3,0,-5), 0.5f), Sphere(float3(-4,0,-5), 0.5f),
            Sphere(float3(4,0,-6), 0.5f), Sphere(float3(3,0,-6), 0.5f), Sphere(float3(2,0,-6), 0.5f), Sphere(float3(1,0,-6), 0.5f), Sphere(float3(0,0,-6), 0.5f), Sphere(float3(-1,0,-6), 0.5f), Sphere(float3(-2,0,-6), 0.5f), Sphere(float3(-3,0,-6), 0.5f), Sphere(float3(-4,0,-6), 0.5f),
            Sphere(float3(1.5f,1.5f,-2), 0.3f),
        };
        g.spheres.assign(sphereTable, sphereTable + sizeof(sphereTable)/sizeof(sphereTable[0]));
        g.sphereCount = (int)g.spheres.size();

        MaterialParams m;
        auto lambert = [&](float r, float g_, float b, float er, float eg, float eb) {
            m = MaterialParams{};
            m.type = kMaterialTypeLambert;
            m.albedo[0]=r; m.albedo[1]=g_; m.albedo[2]=b;
            m.emissive[0]=er; m.emissive[1]=eg; m.emissive[2]=eb;
            g.defaultMaterials.push_back(m);
        };
        auto metal = [&](float r, float g_, float b, float rough) {
            m = MaterialParams{};
            m.type = kMaterialTypeMetal;
            m.albedo[0]=r; m.albedo[1]=g_; m.albedo[2]=b;
            m.roughness = rough;
            g.defaultMaterials.push_back(m);
        };
        auto dielectric = [&](float r, float g_, float b, float ri) {
            m = MaterialParams{};
            m.type = kMaterialTypeDielectric;
            m.albedo[0]=r; m.albedo[1]=g_; m.albedo[2]=b;
            m.ri = ri;
            g.defaultMaterials.push_back(m);
        };

        lambert(0.8f, 0.8f, 0.8f, 0, 0, 0);          // 0
        lambert(0.8f, 0.4f, 0.4f, 0, 0, 0);          // 1
        lambert(0.4f, 0.8f, 0.4f, 0, 0, 0);          // 2
        metal(0.4f, 0.4f, 0.8f, 0);                  // 3
        metal(0.4f, 0.8f, 0.4f, 0);                  // 4
        metal(0.4f, 0.8f, 0.4f, 0.2f);               // 5
        metal(0.4f, 0.8f, 0.4f, 0.6f);               // 6
        dielectric(0.4f, 0.4f, 0.4f, 1.5f);          // 7
        lambert(0.8f, 0.6f, 0.2f, 30, 25, 15);       // 8
        const float grays[9] = {0.1f,0.2f,0.3f,0.4f,0.5f,0.6f,0.7f,0.8f,0.9f};
        for (float v : grays) lambert(v, v, v, 0, 0, 0);       // 9..17
        for (float v : grays) metal(v, v, v, 0);               // 18..26
        const float cols[9][3] = {
            {0.8f,0.1f,0.1f},{0.8f,0.5f,0.1f},{0.8f,0.8f,0.1f},{0.4f,0.8f,0.1f},{0.1f,0.8f,0.1f},
            {0.1f,0.8f,0.5f},{0.1f,0.8f,0.8f},{0.1f,0.1f,0.8f},{0.5f,0.1f,0.8f},
        };
        for (auto& c : cols) metal(c[0], c[1], c[2], 0);       // 27..35
        for (auto& c : cols) lambert(c[0], c[1], c[2], 0, 0, 0); // 36..44
        lambert(0.1f, 0.2f, 0.5f, 3, 10, 20);                  // 45
        return g;
    }();
    return geo;
}

// ---------------------------------------------------------------------------
// Process-wide enkiTS scheduler. Sessions and tickets share it; there is no
// global render lock, only enkiTS' own work-stealing synchronisation.
// ---------------------------------------------------------------------------

constexpr uint32_t kExternalThreadSlots = 16;

struct SchedulerHolder
{
    enki::TaskScheduler scheduler;
    explicit SchedulerHolder(int threads)
    {
        // Reserve external-thread slots so host threads (e.g. one std::thread
        // per session) can drive WaitforTask concurrently with worker threads.
        enki::TaskSchedulerConfig config;
        if (threads > 0)
            config.numTaskThreadsToCreate = std::max(1u, (uint32_t)threads);
        config.numExternalTaskThreads = kExternalThreadSlots;
        scheduler.Initialize(config);
    }
};

// enkiTS only allows scheduler calls from registered threads. The primary
// thread is registered implicitly; any other host thread registers for the
// duration of the call.
struct ScopedExternalThread
{
    enki::TaskScheduler* scheduler = nullptr;
    bool registered = false;
    explicit ScopedExternalThread(enki::TaskScheduler& s)
    {
        if (s.GetThreadNum() == enki::NO_THREAD_NUM)
        {
            registered = s.RegisterExternalTaskThread();
            scheduler = &s;
        }
    }
    ~ScopedExternalThread()
    {
        if (registered)
            scheduler->DeRegisterExternalTaskThread();
    }
};

std::shared_ptr<SchedulerHolder> SharedScheduler(int desiredThreads)
{
    static std::weak_ptr<SchedulerHolder> weak;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock(mtx);
    std::shared_ptr<SchedulerHolder> held = weak.lock();
    if (!held)
    {
        held = std::make_shared<SchedulerHolder>(desiredThreads);
        weak = held;
    }
    return held;
}

// ---------------------------------------------------------------------------
// A material generation: an immutable full table plus its emissive index list.
// ---------------------------------------------------------------------------

struct Generation
{
    std::vector<MaterialParams> materials;
    std::vector<int> emissives;

    explicit Generation(int sphereCount) : materials(sphereCount) {}
};

std::vector<int> FindEmissives(const std::vector<MaterialParams>& mats)
{
    std::vector<int> out;
    for (size_t i = 0; i < mats.size(); ++i)
    {
        const MaterialParams& m = mats[i];
        if (m.emissive[0] > 0.0f || m.emissive[1] > 0.0f || m.emissive[2] > 0.0f)
            out.push_back((int)i);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Render core. Owned by a FrameTicket; keeps everything a frame needs alive
// independently of the session and of the caller's publication arrays.
// ---------------------------------------------------------------------------

// Definition of the opaque frame-ticket state declared in the header.
enum ComputeState
{
    kComputePending = 0,
    kComputeRunning,
    kComputeComputed,
    kComputeCanceled,
    kComputeCommitted,
};

struct FrameTicketCore
{
    std::shared_ptr<SchedulerHolder> schedulerHolder;

    // Owning frame snapshot.
    std::shared_ptr<const Generation> generation;
    uint64_t generationId = 0;
    std::unique_ptr<SpheresSoA> spheres;
    Camera camera;
    int width = 0;
    int height = 0;
    int frameNumber = 0; // frame count at BeginFrame; drives the RNG seed

    // Private linear-RGB frame buffer.
    std::vector<float> pixels;
    std::atomic<int> rayCount{0};
    std::atomic<bool> canceled{false};
    std::atomic<int> computeState{kComputePending};
    std::mutex computeMutex;

    // Session linkage. Null once the owning session is gone; the ticket then
    // remains fully computable on its own but reports TicketStatus::SessionDead.
    // Atomic because the session destructor clears it while other threads may
    // be querying the ticket.
    std::atomic<MaterialSession::Impl*> session{nullptr};
};

// Read-only view handed to the trace routines.
struct SceneView
{
    const SpheresSoA* spheres = nullptr;
    const MaterialParams* materials = nullptr;
    const int* emissives = nullptr;
    int emissiveCount = 0;
};

inline float3 AsFloat3(const float v[3]) { return float3(v[0], v[1], v[2]); }

bool HitWorld(const SceneView& scene, const Ray& r, float tMin, float tMax, Hit& outHit, int& outID)
{
    outID = HitSpheres(r, *scene.spheres, tMin, tMax, outHit);
    return outID != -1;
}

// Same scatter model as the production Test.cpp path, parameterised by the
// ticket's snapshot instead of the file-global material table.
bool Scatter(const SceneView& scene, const MaterialParams& mat, int hitId,
             const Ray& r_in, const Hit& rec, float3& attenuation, Ray& scattered,
             float3& outLightE, int& inoutRayCount, uint32_t& state)
{
    outLightE = float3(0, 0, 0);
    if (mat.type == kMaterialTypeLambert)
    {
        float3 target = rec.pos + rec.normal + RandomUnitVector(state);
        scattered = Ray(rec.pos, normalize(target - rec.pos));
        float3 matAlbedo = AsFloat3(mat.albedo);
        attenuation = matAlbedo;

#if DO_LIGHT_SAMPLING
        for (int j = 0; j < scene.emissiveCount; ++j)
        {
            int i = scene.emissives[j];
            if (i == hitId)
                continue; // skip self
            const MaterialParams& smat = scene.materials[i];
            const Sphere& s = Geometry().spheres[i];

            float3 sc = s.center.toFloat3();
            float3 sw = normalize(sc - rec.pos);
            float3 su = normalize(cross(fabs(sw.getX()) > 0.01f ? float3(0, 1, 0) : float3(1, 0, 0), sw));
            float3 sv = cross(sw, su);
            float cosAMax = sqrtf(1.0f - s.radius * s.radius / sqLength(rec.pos - sc));
            float eps1 = RandomFloat01(state), eps2 = RandomFloat01(state);
            float cosA = 1.0f - eps1 + eps1 * cosAMax;
            float sinA = sqrtf(1.0f - cosA * cosA);
            float phi = 2 * kPI * eps2;
            float3 l = su * (cosf(phi) * sinA) + sv * (sinf(phi) * sinA) + sw * cosA;

            Hit lightHit;
            int hitID;
            ++inoutRayCount;
            if (HitWorld(scene, Ray(rec.pos, l), kMinT, kMaxT, lightHit, hitID) && hitID == i)
            {
                float omega = 2 * kPI * (1 - cosAMax);
                float3 rdir = r_in.dir;
                AssertUnit(rdir);
                float3 nl = dot(rec.normal, rdir) < 0 ? rec.normal : -rec.normal;
                float3 smatEmissive = AsFloat3(smat.emissive);
                outLightE += (matAlbedo * smatEmissive) * (std::max(0.0f, dot(l, nl)) * omega / kPI);
            }
        }
#endif
        return true;
    }
    else if (mat.type == kMaterialTypeMetal)
    {
        AssertUnit(r_in.dir); AssertUnit(rec.normal);
        float3 refl = reflect(r_in.dir, rec.normal);
        float roughness = mat.roughness;
        scattered = Ray(rec.pos, normalize(refl + roughness * RandomInUnitSphere(state)));
        attenuation = AsFloat3(mat.albedo);
        return dot(scattered.dir, rec.normal) > 0;
    }
    else // kMaterialTypeDielectric
    {
        AssertUnit(r_in.dir); AssertUnit(rec.normal);
        float3 outwardN;
        float3 rdir = r_in.dir;
        float3 refl = reflect(rdir, rec.normal);
        float nint;
        attenuation = float3(1, 1, 1);
        float3 refr;
        float reflProb;
        float cosine;
        if (dot(rdir, rec.normal) > 0)
        {
            outwardN = -rec.normal;
            nint = mat.ri;
            cosine = mat.ri * dot(rdir, rec.normal);
        }
        else
        {
            outwardN = rec.normal;
            nint = 1.0f / mat.ri;
            cosine = -dot(rdir, rec.normal);
        }
        if (refract(rdir, outwardN, nint, refr))
            reflProb = schlick(cosine, mat.ri);
        else
            reflProb = 1;
        if (RandomFloat01(state) < reflProb)
            scattered = Ray(rec.pos, normalize(refl));
        else
            scattered = Ray(rec.pos, normalize(refr));
    }
    return true;
}

float3 Trace(const SceneView& scene, const Ray& r, int depth, int& inoutRayCount,
             uint32_t& state, bool doMaterialE = true)
{
    Hit rec;
    int id = 0;
    ++inoutRayCount;
    if (HitWorld(scene, r, kMinT, kMaxT, rec, id))
    {
        Ray scattered;
        float3 attenuation;
        float3 lightE;
        const MaterialParams& mat = scene.materials[id];
        float3 matE = AsFloat3(mat.emissive);
        if (depth < kMaxDepth && Scatter(scene, mat, id, r, rec, attenuation, scattered, lightE, inoutRayCount, state))
        {
#if DO_LIGHT_SAMPLING
            if (!doMaterialE) matE = float3(0, 0, 0);
            doMaterialE = (mat.type != kMaterialTypeLambert);
#endif
            return matE + lightE + attenuation * Trace(scene, scattered, depth + 1, inoutRayCount, state, doMaterialE);
        }
        return matE;
    }

    // Sky, identical to the production path.
    float3 unitDir = r.dir;
    float t = 0.5f * (unitDir.getY() + 1.0f);
    return ((1.0f - t) * float3(1.0f, 1.0f, 1.0f) + t * float3(0.5f, 0.7f, 1.0f)) * 0.3f;
}

// Row task: same per-pixel loop and RNG seeding rule as TraceRowJob in
// Test.cpp, so sequential snapshots reproduce the production frame sequence.
struct RowJobData
{
    FrameTicketCore* core;
    SceneView scene;
};

void RunRows(RowJobData& data, uint32_t start, uint32_t end)
{
    FrameTicketCore& core = *data.core;
    const int width = core.width;
    const int height = core.height;
    float invWidth = 1.0f / width;
    float invHeight = 1.0f / height;
    int rayCount = 0;
    for (uint32_t y = start; y < end; ++y)
    {
        if (core.canceled.load(std::memory_order_relaxed))
            return;
        uint32_t state = (y * 9781 + core.frameNumber * 6271) | 1;
        float* out = core.pixels.data() + ((size_t)y * width) * 3;
        for (int x = 0; x < width; ++x)
        {
            float3 col(0, 0, 0);
            for (int s = 0; s < DO_SAMPLES_PER_PIXEL; ++s)
            {
                float u = float(x + RandomFloat01(state)) * invWidth;
                float v = float(y + RandomFloat01(state)) * invHeight;
                Ray ray = core.camera.GetRay(u, v, state);
                col += Trace(data.scene, ray, 0, rayCount, state);
            }
            col *= 1.0f / float(DO_SAMPLES_PER_PIXEL);
            col.store(out);
            out += 3;
        }
    }
    core.rayCount += rayCount;
}

class RowTaskSet : public enki::ITaskSet
{
public:
    RowJobData* job;
    RowTaskSet(RowJobData* job_, uint32_t setSize, uint32_t minRange)
        : enki::ITaskSet(setSize, minRange), job(job_) {}
    void ExecuteRange(enki::TaskSetPartition range, uint32_t /*threadnum*/) override
    {
        RunRows(*job, range.start, range.end);
    }
};

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool IsFiniteColor(const float c[3])
{
    return std::isfinite(c[0]) && std::isfinite(c[1]) && std::isfinite(c[2]);
}

bool ValidateBatch(const MaterialEntry* entries, size_t count, int sphereCount,
                   std::vector<MaterialParams>& outNext)
{
    if (count != (size_t)sphereCount)
        return false;

    std::vector<unsigned char> seen(sphereCount, 0);
    for (size_t k = 0; k < count; ++k)
    {
        const MaterialEntry& e = entries[k];
        int idx = e.sphereIndex;
        if (idx < 0 || idx >= sphereCount || seen[idx])
            return false;
        seen[idx] = 1;

        const MaterialParams& p = e.params;
        if (p.type != kMaterialTypeLambert &&
            p.type != kMaterialTypeMetal &&
            p.type != kMaterialTypeDielectric)
            return false;
        if (!IsFiniteColor(p.albedo) || !IsFiniteColor(p.emissive) ||
            !std::isfinite(p.roughness) || !std::isfinite(p.ri))
            return false;
        for (int c = 0; c < 3; ++c)
        {
            if (p.albedo[c] < 0.0f || p.emissive[c] < 0.0f)
                return false;
        }
        if (p.roughness < 0.0f || p.roughness > 1.0f)
            return false;
        if (p.type == kMaterialTypeDielectric && p.ri <= 0.0f)
            return false;
    }
    for (int i = 0; i < sphereCount; ++i)
        if (!seen[i])
            return false;

    outNext.resize(sphereCount);
    for (size_t k = 0; k < count; ++k)
        outNext[entries[k].sphereIndex] = entries[k].params;
    return true;
}

void FillSnapshot(const Generation& gen, MaterialSnapshot& dst)
{
    if (dst.materials && dst.materialCapacity > 0)
    {
        size_t n = std::min(dst.materialCapacity, gen.materials.size());
        std::memcpy(dst.materials, gen.materials.data(), n * sizeof(MaterialParams));
    }
    dst.emissiveCount = gen.emissives.size();
    if (dst.emissives && dst.emissiveCapacity > 0)
    {
        size_t n = std::min(dst.emissiveCapacity, gen.emissives.size());
        std::memcpy(dst.emissives, gen.emissives.data(), n * sizeof(int));
    }
}

// ---------------------------------------------------------------------------
// Session implementation
// ---------------------------------------------------------------------------

class MaterialSession::Impl
{
public:
    int width;
    int height;
    std::shared_ptr<SchedulerHolder> schedulerHolder;

    mutable std::mutex mutex;
    std::shared_ptr<Generation> current;
    uint64_t currentId = 0;
    std::vector<float> accumulation; // width*height*3 linear RGB
    int frameCount = 0;
    FrameTicketCore* activeCore = nullptr;

    Impl(const SessionConfig& config)
        : width(std::max(1, config.width))
        , height(std::max(1, config.height))
        , schedulerHolder(SharedScheduler(config.workerThreads))
    {
        const SceneGeometry& geo = Geometry();
        current = std::make_shared<Generation>(geo.sphereCount);
        current->materials = geo.defaultMaterials;
        current->emissives = FindEmissives(current->materials);
        accumulation.assign((size_t)width * height * 3, 0.0f);
    }

    int SphereCount() const { return (int)current->materials.size(); }

    std::unique_ptr<SpheresSoA> BuildSoA() const
    {
        const SceneGeometry& geo = Geometry();
        auto soa = std::make_unique<SpheresSoA>(geo.sphereCount);
        for (int i = 0; i < geo.sphereCount; ++i)
        {
            const Sphere& s = geo.spheres[i];
            soa->centerX[i] = s.center.x;
            soa->centerY[i] = s.center.y;
            soa->centerZ[i] = s.center.z;
            soa->sqRadius[i] = s.radius * s.radius;
            soa->invRadius[i] = 1.0f / s.radius;
        }
        return soa;
    }

    Camera BuildCamera() const
    {
        // Same fixed camera as UpdateTest (big scene aperture scaling).
        float3 lookfrom(0, 2, 3);
        float3 lookat(0, 0, 0);
        float distToFocus = 3;
        float aperture = 0.1f * 0.2f;
        return Camera(lookfrom, lookat, float3(0, 1, 0), 60,
                      float(width) / float(height), aperture, distToFocus);
    }
};

// ---------------------------------------------------------------------------
// FrameTicket
// ---------------------------------------------------------------------------

FrameTicket::FrameTicket(FrameTicketCore* core) : core_(core) {}

FrameTicket::~FrameTicket()
{
    if (!core_)
        return;
    // Detach from a still-live session if we are its active ticket.
    MaterialSession::Impl* session = core_->session.load(std::memory_order_acquire);
    if (session)
    {
        std::lock_guard<std::mutex> sessionLock(session->mutex);
        if (session->activeCore == core_)
            session->activeCore = nullptr;
    }
    delete core_;
}

TicketStatus FrameTicket::Status() const
{
    const bool alive = core_->session.load(std::memory_order_acquire) != nullptr;
    switch (core_->computeState.load(std::memory_order_acquire))
    {
    case kComputeRunning:   return TicketStatus::Running;
    case kComputeComputed:  return alive ? TicketStatus::Computed : TicketStatus::SessionDead;
    case kComputeCommitted: return TicketStatus::Committed;
    case kComputeCanceled:  return alive ? TicketStatus::Canceled : TicketStatus::SessionDead;
    case kComputePending:
    default:                return alive ? TicketStatus::Pending : TicketStatus::SessionDead;
    }
}

int FrameTicket::Width() const { return core_->width; }
int FrameTicket::Height() const { return core_->height; }
uint64_t FrameTicket::Generation() const { return core_->generationId; }

bool FrameTicket::IsCurrent() const
{
    MaterialSession::Impl* session = core_->session.load(std::memory_order_acquire);
    if (!session)
        return false;
    std::lock_guard<std::mutex> lock(session->mutex);
    return core_->session.load(std::memory_order_acquire) == session &&
           session->currentId == core_->generationId;
}

int FrameTicket::RayCount() const
{
    return core_->rayCount.load(std::memory_order_acquire);
}

TicketStatus FrameTicket::Compute()
{
    {
        std::lock_guard<std::mutex> lock(core_->computeMutex);
        if (core_->computeState.load(std::memory_order_acquire) != kComputePending)
            return Status(); // Running / Computed / Canceled / Committed are terminal
        core_->canceled.store(false, std::memory_order_relaxed);
        core_->rayCount.store(0, std::memory_order_relaxed);
        core_->computeState.store(kComputeRunning, std::memory_order_release);
    }

    RowJobData job;
    job.core = core_;
    job.scene.spheres = core_->spheres.get();
    job.scene.materials = core_->generation->materials.data();
    job.scene.emissives = core_->generation->emissives.data();
    job.scene.emissiveCount = (int)core_->generation->emissives.size();

    // The core lock is released for the whole render so CancelTicket can set
    // the cancellation flag and the row tasks observe it at row boundaries.
    RowTaskSet task(&job, (uint32_t)core_->height, 4);
    enki::TaskScheduler& scheduler = core_->schedulerHolder->scheduler;
    ScopedExternalThread external(scheduler);
    scheduler.AddTaskSetToPipe(&task);
    scheduler.WaitforTask(&task);

    std::lock_guard<std::mutex> lock(core_->computeMutex);
    if (core_->canceled.load(std::memory_order_relaxed))
        core_->computeState.store(kComputeCanceled, std::memory_order_release);
    else
        core_->computeState.store(kComputeComputed, std::memory_order_release);
    return Status();
}

bool FrameTicket::ReadPixels(float* outPixels, size_t count) const
{
    if (core_->computeState.load(std::memory_order_acquire) != kComputeComputed)
        return false;
    size_t needed = (size_t)core_->width * core_->height * 3;
    if (!outPixels || count < needed)
        return false;
    std::memcpy(outPixels, core_->pixels.data(), needed * sizeof(float));
    return true;
}

const float* FrameTicket::Pixels() const
{
    if (core_->computeState.load(std::memory_order_acquire) != kComputeComputed)
        return nullptr;
    return core_->pixels.data();
}

// ---------------------------------------------------------------------------
// MaterialSession
// ---------------------------------------------------------------------------

MaterialSession::MaterialSession(const SessionConfig& config) : impl_(new Impl(config)) {}

MaterialSession::~MaterialSession()
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->activeCore)
        {
            FrameTicketCore* core = impl_->activeCore;
            int state = core->computeState.load(std::memory_order_acquire);
            if (state == kComputePending || state == kComputeRunning)
                core->canceled.store(true, std::memory_order_release);
            // Detach: the ticket owns every render resource and can still be
            // computed and released; only commits are now impossible.
            core->session.store(nullptr, std::memory_order_release);
        }
    }
    delete impl_;
}

int MaterialSession::SphereCount() const { return impl_->SphereCount(); }
int MaterialSession::Width() const { return impl_->width; }
int MaterialSession::Height() const { return impl_->height; }

uint64_t MaterialSession::CurrentGeneration() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->currentId;
}

int MaterialSession::CommittedFrameCount() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->frameCount;
}

void MaterialSession::GetDefaultMaterials(MaterialSnapshot& outSnapshot) const
{
    const SceneGeometry& geo = Geometry();
    Generation gen(geo.sphereCount);
    gen.materials = geo.defaultMaterials;
    gen.emissives = FindEmissives(gen.materials);
    FillSnapshot(gen, outSnapshot);
}

uint64_t MaterialSession::GetCurrentMaterials(MaterialSnapshot& outSnapshot) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    FillSnapshot(*impl_->current, outSnapshot);
    return impl_->currentId;
}

PublishStatus MaterialSession::Publish(const MaterialEntry* entries, size_t count,
                                       PublishOutput* outOutput)
{
    if (count == 0)
        return PublishStatus::Empty;

    const int sphereCount = Geometry().sphereCount;
    std::vector<MaterialParams> next;
    if (!ValidateBatch(entries, count, sphereCount, next))
        return PublishStatus::InvalidArgument;

    auto gen = std::make_shared<Generation>(sphereCount);
    gen->materials = std::move(next);
    gen->emissives = FindEmissives(gen->materials);

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->current = std::move(gen);
    ++impl_->currentId;
    std::fill(impl_->accumulation.begin(), impl_->accumulation.end(), 0.0f);
    impl_->frameCount = 0;
    // A live ticket keeps its old snapshot and is now stale: it stays the one
    // active (but no longer committable) ticket until the caller ends it
    // (commit is rejected as Stale; cancel/release/destroy free the slot).

    if (outOutput)
    {
        outOutput->generation = impl_->currentId;
        FillSnapshot(*impl_->current, outOutput->snapshot);
    }
    return PublishStatus::Ok;
}

FrameTicket* MaterialSession::BeginFrame()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->activeCore)
    {
        int cs = impl_->activeCore->computeState.load(std::memory_order_acquire);
        if (cs == kComputePending || cs == kComputeRunning || cs == kComputeComputed)
            return nullptr; // one unterminated ticket at a time
        impl_->activeCore = nullptr;
    }

    auto core = std::make_unique<FrameTicketCore>();
    core->schedulerHolder = impl_->schedulerHolder;
    core->generation = impl_->current;
    core->generationId = impl_->currentId;
    core->width = impl_->width;
    core->height = impl_->height;
    core->frameNumber = impl_->frameCount;
    core->spheres = impl_->BuildSoA();
    core->camera = impl_->BuildCamera();
    core->pixels.assign((size_t)core->width * core->height * 3, 0.0f);
    core->session.store(impl_, std::memory_order_release);
    core->computeState.store(kComputePending, std::memory_order_release);

    FrameTicketCore* raw = core.release();
    impl_->activeCore = raw;
    return new FrameTicket(raw);
}

TicketStatus MaterialSession::ComputeTicket(FrameTicket* ticket)
{
    if (!ticket)
        return TicketStatus::Invalid;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (ticket->core_->session.load(std::memory_order_acquire) != impl_)
            return ticket->core_->session.load(std::memory_order_acquire) ? TicketStatus::Invalid : TicketStatus::SessionDead;
    }
    // The session lock is released: publications can proceed while this frame
    // renders on the worker threads.
    return ticket->Compute();
}

bool MaterialSession::CancelTicket(FrameTicket* ticket)
{
    if (!ticket)
        return false;
    FrameTicketCore* core = ticket->core_;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (core->session.load(std::memory_order_acquire) != impl_)
            return false;
    }

    int state = core->computeState.load(std::memory_order_acquire);
    if (state == kComputeCanceled || state == kComputeCommitted)
        return false;
    core->canceled.store(true, std::memory_order_release);
    if (state != kComputeRunning)
        core->computeState.store(kComputeCanceled, std::memory_order_release);
    // A running compute observes the flag at its row checks and ends Canceled.

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->activeCore == core)
        impl_->activeCore = nullptr;
    return true;
}

void MaterialSession::ReleaseTicket(FrameTicket* ticket)
{
    if (!ticket)
        return;
    FrameTicketCore* core = ticket->core_;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (core->session.load(std::memory_order_acquire) != impl_)
            return;
    }

    int state = core->computeState.load(std::memory_order_acquire);
    if (state != kComputeCommitted)
    {
        core->canceled.store(true, std::memory_order_release);
        if (state != kComputeRunning)
            core->computeState.store(kComputeCanceled, std::memory_order_release);
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->activeCore == core)
        impl_->activeCore = nullptr;
}

CommitResult MaterialSession::CommitTicket(FrameTicket* ticket)
{
    if (!ticket)
        return CommitResult::InvalidTicket;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    FrameTicketCore* core = ticket->core_;
    if (core->session.load(std::memory_order_acquire) != impl_)
        return CommitResult::InvalidTicket;

    int state = core->computeState.load(std::memory_order_acquire);
    if (state == kComputeCanceled || state == kComputeCommitted)
        return CommitResult::InvalidTicket;
    if (state != kComputeComputed)
        return CommitResult::NotComputed;
    if (core->generationId != impl_->currentId || core->generation.get() != impl_->current.get())
        return CommitResult::Stale;

    const float lerpFac = float(impl_->frameCount) / float(impl_->frameCount + 1);
    const size_t n = impl_->accumulation.size();
    for (size_t i = 0; i < n; ++i)
        impl_->accumulation[i] = impl_->accumulation[i] * lerpFac + core->pixels[i] * (1.0f - lerpFac);
    ++impl_->frameCount;

    core->computeState.store(kComputeCommitted, std::memory_order_release);
    impl_->activeCore = nullptr;
    return CommitResult::Ok;
}

int MaterialSession::ReadAccumulation(float* outPixels, size_t count) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    size_t n = impl_->accumulation.size();
    if (outPixels && count >= n)
        std::memcpy(outPixels, impl_->accumulation.data(), n * sizeof(float));
    return impl_->frameCount;
}

} // namespace materialsession
