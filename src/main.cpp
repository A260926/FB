#include "util/Utilities.h"

#include <string>
#include <fstream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <thread>
#include "Camera.h"
// #include "object/Object.h"
#include "object/ObjectList.h"
#include "object/Sphere.h"
#include "object/Cylinder.h"
#include "object/Triangle.h"
#include "light/Light.h"
#include "light/PointLight.h"
#include "light/RectangularLight.h"
#include "light/LightList.h"
#include "Material.h"
#include "Parser.h"


int main(int argc, char* argv[]) {
    // Check if the correct number of arguments is provided
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <jsonName> [outputDir] [outputName] [--threads N] [--benchmark]" << std::endl;
        return 1;
    }

    std::string jsonName = argv[1];

    // Parse positional args (outputDir, imageName), stopping at first flag
    std::string outputDir;
    std::string imageName = "rendered";
    int positional = 0;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]).substr(0, 2) == "--") break;
        ++positional;
        if (positional == 1) outputDir = argv[i];
        else if (positional == 2) imageName = argv[i];
    }

    size_t thread_count = 1;
    bool benchmark_mode = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--threads" && i + 1 < argc) {
            thread_count = std::stoul(argv[++i]);
            if (thread_count == 0) {
                thread_count = std::thread::hardware_concurrency();
                if (thread_count == 0) thread_count = 4;
            }
        } else if (arg == "--benchmark") {
            benchmark_mode = true;
        }
    }

    std::string extension = ".ppm";

    ObjectList world;
    LightList lights;
    Camera cam;

    Parser parser;

    try {
        parser.parse_scene(jsonName, jsonName, world, lights, cam);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    cam.filename = outputDir + imageName + extension;

    // // Build the BVH
    if (world.use_bvh == UseBVH::ENABLE) {
        world.build();
    }

    cam.print_specs();

    if (benchmark_mode) {
        // Benchmark: render 1T baseline, then NT, print comparison
        cam.num_threads = 1;
        cam.filename = outputDir + imageName + "_1T" + extension;

        auto start_1t = std::chrono::high_resolution_clock::now();
        cam.render(world, lights);
        auto end_1t = std::chrono::high_resolution_clock::now();
        double time_1t = std::chrono::duration_cast<std::chrono::milliseconds>(end_1t - start_1t).count() / 1000.0;

        cam.num_threads = thread_count;
        cam.filename = outputDir + imageName + "_NT" + extension;

        auto start_nt = std::chrono::high_resolution_clock::now();
        cam.render(world, lights);
        auto end_nt = std::chrono::high_resolution_clock::now();
        double time_nt = std::chrono::duration_cast<std::chrono::milliseconds>(end_nt - start_nt).count() / 1000.0;

        double speedup = time_1t / time_nt;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Speedup: " << speedup << "x (1T: " << time_1t << "s, NT: " << time_nt << "s, " << thread_count << " threads)" << std::endl;
    } else {
        cam.num_threads = thread_count;

        auto start = std::chrono::high_resolution_clock::now();
        cam.render(world, lights);
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Rendering completed in " << duration / 1000.0 << " seconds." << std::endl;
    }

    world.log_hit_counts();

    return 0;
}