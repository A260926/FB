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
// Helpers
// ---------------------------------------------------------------------------

static const std::string OUTPUT_DIR = "/tmp/diff-test/";

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

static void require_valid_ppm_any(const std::string& output_name) {
    auto path = OUTPUT_DIR + output_name + ".ppm";
    REQUIRE(fs::exists(path));
    REQUIRE(fs::file_size(path) > 0);

    int w, h;
    std::vector<int> pixels;
    REQUIRE(parse_ppm(path, w, h, pixels));

    REQUIRE(w > 0);
    REQUIRE(h > 0);
    REQUIRE(static_cast<int>(pixels.size()) == 3 * w * h);

    for (int v : pixels) {
        REQUIRE(v >= 0);
        REQUIRE(v <= 255);
    }
}

// ---------------------------------------------------------------------------
// Structural tests (fixtures)
// ---------------------------------------------------------------------------

TEST_CASE("differential BVH on/off produces identical output", "[differential]") {
    const std::string scene_on  = "../tests/fixtures/minimal_bvh_on.json";
    const std::string scene_off = "../tests/fixtures/minimal_bvh_off.json";

    int rc_on  = run_renderer(scene_on,  "bvh_on");
    int rc_off = run_renderer(scene_off, "bvh_off");

    if (rc_on != 0)  FAIL_CHECK("BVH on failed:\n" + read_log("bvh_on"));
    if (rc_off != 0) FAIL_CHECK("BVH off failed:\n" + read_log("bvh_off"));
    REQUIRE(rc_on  == 0);
    REQUIRE(rc_off == 0);

    auto path_on  = OUTPUT_DIR + "bvh_on.ppm";
    auto path_off = OUTPUT_DIR + "bvh_off.ppm";

    REQUIRE(fs::exists(path_on));
    REQUIRE(fs::exists(path_off));
    REQUIRE(fs::file_size(path_on)  > 0);
    REQUIRE(fs::file_size(path_off) > 0);

    int w_on, h_on, w_off, h_off;
    std::vector<int> pix_on, pix_off;
    REQUIRE(parse_ppm(path_on,  w_on,  h_on,  pix_on));
    REQUIRE(parse_ppm(path_off, w_off, h_off, pix_off));

    REQUIRE(w_on == w_off);
    REQUIRE(h_on == h_off);
    REQUIRE(pix_on.size() == pix_off.size());

    int non_zero_on = 0, non_zero_off = 0;
    for (int v : pix_on) if (v != 0) non_zero_on++;
    for (int v : pix_off) if (v != 0) non_zero_off++;
    REQUIRE(non_zero_on == non_zero_off);

    double sum_on = 0, sum_off = 0;
    for (int v : pix_on) sum_on += v;
    for (int v : pix_off) sum_off += v;
    REQUIRE(std::abs(sum_on - sum_off) < 1.0);
}

TEST_CASE("differential binary mode produces only black/white pixels", "[differential]") {
    int rc = run_renderer("../tests/fixtures/minimal_binary.json", "binary_test");
    if (rc != 0) FAIL_CHECK("Binary renderer failed:\n" + read_log("binary_test"));
    REQUIRE(rc == 0);
    require_valid_ppm("binary_test", 10, 10);

    auto path = OUTPUT_DIR + "binary_test.ppm";
    int w, h;
    std::vector<int> pixels;
    REQUIRE(parse_ppm(path, w, h, pixels));

    bool all_valid = true;
    for (int v : pixels) {
        if (v != 0 && v != 255) {
            all_valid = false;
            break;
        }
    }
    REQUIRE(all_valid);

    bool has_hit  = false;
    bool has_miss = false;
    for (size_t i = 0; i + 2 < pixels.size(); i += 3) {
        if (pixels[i] == 255) has_hit  = true;
        if (pixels[i] == 0)   has_miss = true;
    }
    REQUIRE(has_hit);
    REQUIRE(has_miss);
}

// ---------------------------------------------------------------------------
// Code-path coverage via tiny fixtures
// ---------------------------------------------------------------------------

TEST_CASE("path tracer renders without errors", "[differential][path]") {
    int rc = run_renderer("../tests/fixtures/minimal_path.json", "path_fixture", 30);
    if (rc != 0) FAIL_CHECK("Path tracer failed:\n" + read_log("path_fixture"));
    REQUIRE(rc == 0);
    require_valid_ppm("path_fixture", 16, 16);
}

TEST_CASE("path tracer with BVH and excluded object", "[differential][path][bvh]") {
    int rc = run_renderer("../tests/fixtures/minimal_path_bvh.json", "path_bvh_fixture", 30);
    if (rc != 0) FAIL_CHECK("Path BVH fixture failed:\n" + read_log("path_bvh_fixture"));
    REQUIRE(rc == 0);
    require_valid_ppm("path_bvh_fixture", 16, 16);
}

TEST_CASE("textured scene renders without errors", "[differential][texture]") {
    int rc = run_renderer("../tests/fixtures/minimal_texture.json", "texture_fixture", 30);
    if (rc != 0) FAIL_CHECK("Texture scene failed:\n" + read_log("texture_fixture"));
    REQUIRE(rc == 0);
    require_valid_ppm("texture_fixture", 16, 16);
}

// ---------------------------------------------------------------------------
// Sanitizer smoke: lightweight TestSuite scenes (individual test cases)
// ---------------------------------------------------------------------------

#define SCENE_TEST(name, timeout_secs) \
    TEST_CASE("smoke: " name, "[differential][sanitizer]") { \
        std::string scene = "../TestSuite/" name ".json"; \
        int rc = run_renderer(scene, "smoke_" name, timeout_secs); \
        if (rc != 0) FAIL_CHECK("Scene " name " failed:\n" + read_log("smoke_" name)); \
        REQUIRE(rc == 0); \
        require_valid_ppm_any("smoke_" name); \
    }

SCENE_TEST("binary_primitives", 30)
SCENE_TEST("binary_scene", 30)
SCENE_TEST("simple_phong", 30)
SCENE_TEST("mirror_image", 30)
SCENE_TEST("phong_scene", 30)
SCENE_TEST("bvh", 30)
SCENE_TEST("refract", 60)
