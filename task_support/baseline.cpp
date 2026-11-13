#include "Camera.hpp"
#include "Image.hpp"
#include "Random.hpp"
#include "RayTracer.hpp"
#include "Scene.hpp"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
using namespace hxa7241_minilight;

static void demand(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static bool near(const Vector3f& a, const Vector3f& b, double eps) {
    for (int c=0; c<3; ++c) if (std::abs(a[c]-b[c])>eps) return false;
    return true;
}
static void sceneStream(std::istringstream& in) {
    in.exceptions(std::ios::badbit | std::ios::eofbit | std::ios::failbit);
}
static void renderLegacy(const char* path) {
    std::ifstream file(path);
    demand(bool(file), "missing upstream scene");
    std::string format;
    int iterations, width, height;
    file >> format >> iterations >> width >> height;
    demand(format == "#MiniLight" && iterations>0 && width>0 && height>0, "invalid upstream scene");
    std::ostringstream body;
    body << file.rdbuf();
    std::istringstream cameraInput(body.str());
    sceneStream(cameraInput);
    const Camera camera(cameraInput);
    const Scene scene(cameraInput, camera.getEyePoint());
    std::istringstream aSize("8 6"), bSize("8 6");
    Image a(aSize), b(bSize);
    Random aRandom, bRandom;
    for (int pass=0; pass<2; ++pass) {
        camera.getFrame(scene, aRandom, a);
        camera.getFrame(scene, bRandom, b);
    }
    std::ostringstream aPpm, bPpm;
    a.getFormatted(aPpm, 2); b.getFormatted(bPpm, 2);
    demand(aPpm.str() == bPpm.str(), "old fixed-seed render changed between instances");
    demand(aRandom.getInt32u() == bRandom.getInt32u(), "random streams diverged");
    const auto ppm = aPpm.str();
    const auto data = ppm.find("255\n");
    demand(ppm.rfind("P6\n",0) == 0 && data != std::string::npos, "not a PPM");
    demand(ppm.size() - (data+4) == 8*6*3, "incomplete PPM raster");
    bool nonzero=false;
    for (std::size_t i=data+4; i<ppm.size(); ++i) nonzero |= ppm[i] != 0;
    demand(nonzero, "all-black old scene");
    std::cout << "LEGACY_SCENE_OK " << path << " passes=2 pixels=48 bytes=" << ppm.size() << '\n';
    // Preserve a small upstream-derived model for the original executable.
    if (std::string(path)=="scenes/cornellbox.ml.txt") {
        std::ofstream out(".task-build/legacy.ml");
        out << "#MiniLight\n2\n8 6\n" << body.str();
        demand(bool(out), "cannot write CLI input");
    }
}

int main() try {
    const Vector3f eye(0,1,0), down(0,-1,0);
    std::istringstream skyInput("(1 2 4) (1 1 1)\n");
    sceneStream(skyInput);
    const Scene sky(skyInput, eye);
    Random skyRandom;
    demand(near(RayTracer(sky).getRadiance(eye, Vector3f(0,1,0), skyRandom), Vector3f(1,2,4), 1e-12), "sky ray mismatch");

    const std::string floor = "(-10 0 -10) (-10 0 10) (10 0 -10) (.4 .5 .6) (0 0 0)\n";
    std::istringstream diffuseInput("(1 2 4) (1 1 1)\n" + floor);
    sceneStream(diffuseInput);
    const Scene diffuse(diffuseInput, eye);
    const RayTracer tracer(diffuse);
    Random random;
    Vector3f sum;
    for (int i=0; i<20000; ++i) sum = sum + tracer.getRadiance(Vector3f(-1,1,-1),down,random);
    const auto mean = sum/20000;
    demand(near(mean, Vector3f(.4,1,2.4), .035), "diffuse/RR expectation mismatch");
    std::cout << std::setprecision(12) << "DIFFUSE_RR_OK samples=20000 mean=" << mean[0] << ',' << mean[1] << ',' << mean[2] << '\n';

    std::istringstream emissionInput("(0 0 0) (0 0 0)\n(-10 0 -10) (-10 0 10) (10 0 -10) (0 0 0) (2 3 5)\n");
    sceneStream(emissionInput);
    const Scene emission(emissionInput, eye);
    const RayTracer emitTracer(emission);
    int predecessor = 0;
    demand(near(emitTracer.getRadiance(Vector3f(-1,1,-1),down,random),Vector3f(2,3,5),0), "primary emission mismatch");
    demand(emitTracer.getRadiance(Vector3f(-1,1,-1),down,random,&predecessor).isZero(), "old continuation emission is not suppressed");
    std::cout << "LEGACY_EMISSION_POLICY_OK primary=2,3,5 continuation=0,0,0\n";
    renderLegacy("scenes/cornellbox.ml.txt");
    renderLegacy("scenes/smits-emission.ml.txt");
    std::cout << "BASELINE_OK\n";
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
