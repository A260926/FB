#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <sys/wait.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers (mirrors test_differential.cpp pattern)
// ---------------------------------------------------------------------------

static const std::string OUTPUT_DIR = "/tmp/bvh-test/";

static int run_renderer(const std::string& scene,
                        const std::string& output_name,
                        int timeout_seconds = 30) {
    fs::create_directories(OUTPUT_DIR);

    auto out_path = OUTPUT_DIR + output_name + ".ppm";
    fs::remove(out_path);

    auto log_path = OUTPUT_DIR + output_name + ".log";
    std::string cmd =
        "timeout -k 5s " + std::to_string(timeout_seconds) + "s " +
        "./ray_tracer " + scene + " " + OUTPUT_DIR + " " + output_name +
        " >" + log_path + " 2>&1";

    int ret = std::system(cmd.c_str());
    if (WIFEXITED(ret))
        return WEXITSTATUS(ret);
    return -1;
}

static std::string read_log(const std::string& output_name) {
    auto log_path = OUTPUT_DIR + output_name + ".log";
    std::ifstream f(log_path);
    if (!f.is_open()) return "(no log)";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool parse_ppm(const std::string& path,
                      int& width, int& height,
                      std::vector<int>& pixels) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string magic;
    f >> magic;
    if (magic != "P3") return false;

    std::string line;
    while (f.peek() == '\n' || f.peek() == '#') {
        if (f.peek() == '#') {
            std::getline(f, line);
        } else {
            f.get();
        }
    }

    f >> width >> height;
    int maxval;
    f >> maxval;

    pixels.clear();
    int v;
    while (f >> v) {
        pixels.push_back(v);
    }
    return !pixels.empty();
}

static void require_valid_ppm(const std::string& output_name, int expected_w, int expected_h) {
    auto path = OUTPUT_DIR + output_name + ".ppm";
    REQUIRE(fs::exists(path));
    REQUIRE(fs::file_size(path) > 0);

    int w, h;
    std::vector<int> pixels;
    REQUIRE(parse_ppm(path, w, h, pixels));

    REQUIRE(w == expected_w);
    REQUIRE(h == expected_h);
    REQUIRE(static_cast<int>(pixels.size()) == 3 * w * h);

    for (int v : pixels) {
        REQUIRE(v >= 0);
        REQUIRE(v <= 255);
    }
}

static bool render_and_compare(const std::string& scene_on,
                               const std::string& scene_off,
                               const std::string& name_on,
                               const std::string& name_off) {
    int rc_on  = run_renderer(scene_on,  name_on);
    int rc_off = run_renderer(scene_off, name_off);

    if (rc_on != 0)  FAIL_CHECK("BVH on failed:\n" + read_log(name_on));
    if (rc_off != 0) FAIL_CHECK("BVH off failed:\n" + read_log(name_off));
    REQUIRE(rc_on  == 0);
    REQUIRE(rc_off == 0);

    auto path_on  = OUTPUT_DIR + name_on + ".ppm";
    auto path_off = OUTPUT_DIR + name_off + ".ppm";

    REQUIRE(fs::exists(path_on));
    REQUIRE(fs::exists(path_off));

    int w_on, h_on, w_off, h_off;
    std::vector<int> pix_on, pix_off;
    REQUIRE(parse_ppm(path_on,  w_on,  h_on,  pix_on));
    REQUIRE(parse_ppm(path_off, w_off, h_off, pix_off));

    REQUIRE(w_on == w_off);
    REQUIRE(h_on == h_off);
    REQUIRE(pix_on == pix_off);

    return true;
}

// ---------------------------------------------------------------------------
// BVH correctness tests
// ---------------------------------------------------------------------------

TEST_CASE("BVH on/off produces identical output (binary)", "[bvh]") {
    render_and_compare(
        "../tests/fixtures/bvh_test_3spheres.json",
        "../tests/fixtures/bvh_test_3spheres_off.json",
        "bvh_3sph_on",
        "bvh_3sph_off"
    );
}

TEST_CASE("BVH on/off produces identical output (phong)", "[bvh]") {
    render_and_compare(
        "../tests/fixtures/bvh_test_phong_on.json",
        "../tests/fixtures/bvh_test_phong_off.json",
        "bvh_phong_on",
        "bvh_phong_off"
    );
}

TEST_CASE("BVH returns correct hits for known rays", "[bvh]") {
    int rc = run_renderer("../tests/fixtures/bvh_test_3spheres.json", "bvh_hits");
    REQUIRE(rc == 0);
    require_valid_ppm("bvh_hits", 20, 20);

    auto path = OUTPUT_DIR + "bvh_hits.ppm";
    int w, h;
    std::vector<int> pixels;
    REQUIRE(parse_ppm(path, w, h, pixels));

    // Binary mode: hit stores R=255, miss stores R=0
    int center_idx = (10 * 20 + 10) * 3;
    REQUIRE(pixels[center_idx] == 255);

    // Corners should miss all spheres
    REQUIRE(pixels[0] == 0);

    int tr_idx = (0 * 20 + 19) * 3;
    REQUIRE(pixels[tr_idx] == 0);

    int bl_idx = (19 * 20 + 0) * 3;
    REQUIRE(pixels[bl_idx] == 0);

    int hits = 0, misses = 0;
    for (size_t i = 0; i + 2 < pixels.size(); i += 3) {
        if (pixels[i] == 255) hits++;
        else if (pixels[i] == 0) misses++;
    }
    REQUIRE(hits > 0);
    REQUIRE(misses > 0);
}

TEST_CASE("BVH with single object", "[bvh]") {
    render_and_compare(
        "../tests/fixtures/bvh_test_single.json",
        "../tests/fixtures/bvh_test_single_off.json",
        "bvh_single_on",
        "bvh_single_off"
    );
}

TEST_CASE("BVH with empty scene", "[bvh]") {
    int rc = run_renderer("../tests/fixtures/bvh_test_empty.json", "bvh_empty");
    REQUIRE(rc == 0);
    require_valid_ppm("bvh_empty", 20, 20);

    auto path = OUTPUT_DIR + "bvh_empty.ppm";
    int w, h;
    std::vector<int> pixels;
    REQUIRE(parse_ppm(path, w, h, pixels));

    // All pixels should be background color (0,0,0)
    for (int v : pixels) {
        REQUIRE(v == 0);
    }
}

TEST_CASE("BVH with overlapping objects", "[bvh]") {
    render_and_compare(
        "../tests/fixtures/bvh_test_overlapping.json",
        "../tests/fixtures/bvh_test_overlapping_off.json",
        "bvh_overlap_on",
        "bvh_overlap_off"
    );
}

TEST_CASE("BVH output is valid PPM", "[bvh]") {
    const std::vector<std::pair<std::string, std::string>> scenes = {
        {"../tests/fixtures/bvh_test_3spheres.json",   "bvh_valid_3sph"},
        {"../tests/fixtures/bvh_test_single.json",     "bvh_valid_single"},
        {"../tests/fixtures/bvh_test_empty.json",      "bvh_valid_empty"},
        {"../tests/fixtures/bvh_test_overlapping.json","bvh_valid_overlap"},
        {"../tests/fixtures/bvh_test_phong_on.json",   "bvh_valid_phong"},
    };

    for (const auto& [scene, name] : scenes) {
        int rc = run_renderer(scene, name);
        if (rc != 0) FAIL_CHECK("Scene " + name + " failed:\n" + read_log(name));
        REQUIRE(rc == 0);

        auto path = OUTPUT_DIR + name + ".ppm";
        REQUIRE(fs::exists(path));
        REQUIRE(fs::file_size(path) > 0);

        int w, h;
        std::vector<int> pixels;
        REQUIRE(parse_ppm(path, w, h, pixels));

        REQUIRE(w == 20);
        REQUIRE(h == 20);
        REQUIRE(static_cast<int>(pixels.size()) == 3 * 20 * 20);

        for (int v : pixels) {
            REQUIRE(v >= 0);
            REQUIRE(v <= 255);
        }
    }
}
