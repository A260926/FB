#pragma once

#include <chrono>
#include <deque>
#include <atomic>
#include <optional>
#include <cstdint>

#include <glm/vec3.hpp>
#include <glm/vec2.hpp>

#include <nlohmann/json.hpp>

#include "image.hpp"
#include "film.hpp"

#include "../scene/scene.hpp"
#include "../common/work-queue.hpp"
#include "../common/option.hpp"
#include "../ray/ray.hpp"
#include "../sampling/sampler.hpp"

class Integrator;

class Camera
{
public:
    Camera(const nlohmann::json &j, const Option &option);

    void capture();
    void sampleImage();

    void saveImage() const
    {
        image.save(savename);
    }

    void setPosition(const glm::dvec3& p)
    {
        eye = p;
    }

    void lookAt(const glm::dvec3& p);

    size_t sqrtspp;

    glm::dvec3 eye;
    glm::dvec3 forward, left, up;

    double focal_length, sensor_width, aperture_radius, focus_distance;
    Image image;
    Film film;
    bool thin_lens;

    std::string savename;

private:
    struct Bucket
    {
        Bucket() : min(0), max(0) { }
        Bucket(const glm::ivec2& min, const glm::ivec2& max) : min(min), max(max) { }

        glm::ivec2 min;
        glm::ivec2 max;
    };

    void samplePixel(size_t x, size_t y);
    void sampleImageThread(WorkQueue<Bucket>& buckets);

    // One camera sample: produces the ray, pixel position and the sampler state as
    // it is after camera/lens sampling and before the path's first shuffle.
    struct CameraSample
    {
        Ray ray;
        glm::dvec2 pixel;
        Sampler::State sampler_state;
    };

    CameraSample generateCameraSample(size_t x, size_t y, int sample_index);
    void sampleBucketWavefront(const Bucket& bucket);

    void printInfoThread(WorkQueue<Bucket>& buckets);

    const size_t bucket_size = 32;

    std::shared_ptr<Integrator> integrator;

    // Optional wavefront transport. wavefront_capacity > 0 routes the camera's
    // production sampling through a WavefrontBatch; otherwise scalar mode is used.
    size_t wavefront_capacity = 0;
    std::optional<uint32_t> render_seed;

    std::atomic_size_t num_sampled_pixels = 0;
    size_t last_num_sampled_pixels = 0;
    std::chrono::time_point<std::chrono::steady_clock> last_update = std::chrono::steady_clock::now();
    const size_t num_times = 32;
    std::deque<double> times;
};
