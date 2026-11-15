// Implementation of the public C ABI (include/rednoise/rednoise.h): a thin,
// exception-free wrapper over the C++ engine that exposes no C++ or glm types.

#include "rednoise/rednoise.h"

#include "Camera.h"
#include "ObjLoader.h"
#include "Renderer.h"
#include <Canvas.h>
#include <cstdint>
#include <new>
#include <vector>

#include "../third_party/stb/stb_image_write.h" // declarations; implementation in Canvas.cpp

struct rn_scene {
	std::vector<ModelTriangle> model;
};

extern "C" {

rn_scene *rn_scene_load_obj(const char *path, float scale) {
	if (!path)
		return nullptr;
	rn_scene *scene = new (std::nothrow) rn_scene();
	if (!scene)
		return nullptr;
	scene->model = loadOBJ(path, scale);
	if (scene->model.empty()) {
		delete scene;
		return nullptr;
	}
	return scene;
}

void rn_scene_free(rn_scene *scene) {
	delete scene;
}

int rn_scene_triangle_count(const rn_scene *scene) {
	return scene ? static_cast<int>(scene->model.size()) : 0;
}

int rn_render(const rn_scene *scene, rn_render_mode mode, int width, int height, float cam_z, int samples,
              unsigned char *rgba) {
	if (!scene || !rgba || width <= 0 || height <= 0)
		return 0;

	Camera camera(width, height, 2.0f, glm::vec3(0.0f, 0.0f, cam_z));
	camera.lookAt(glm::vec3(0.0f));
	Canvas canvas(width, height);
	switch (mode) {
	case RN_WIREFRAME:
		renderWireframe(scene->model, camera, canvas);
		break;
	case RN_RASTERISED:
		renderRasterised(scene->model, camera, canvas);
		break;
	case RN_RAYTRACED:
		renderRaytraced(scene->model, camera, canvas);
		break;
	case RN_PATHTRACED:
		renderPathTraced(scene->model, camera, canvas, samples);
		break;
	default:
		return 0;
	}

	// Canvas stores 0xAARRGGBB per pixel; unpack to RGBA8.
	int count = width * height;
	for (int i = 0; i < count; i++) {
		uint32_t p = canvas.pixels[static_cast<size_t>(i)];
		rgba[i * 4 + 0] = static_cast<unsigned char>((p >> 16) & 0xFF);
		rgba[i * 4 + 1] = static_cast<unsigned char>((p >> 8) & 0xFF);
		rgba[i * 4 + 2] = static_cast<unsigned char>(p & 0xFF);
		rgba[i * 4 + 3] = static_cast<unsigned char>((p >> 24) & 0xFF);
	}
	return 1;
}

int rn_save_png(const char *path, int width, int height, const unsigned char *rgba) {
	if (!path || !rgba || width <= 0 || height <= 0)
		return 0;
	return stbi_write_png(path, width, height, 4, rgba, width * 4) ? 1 : 0;
}

const char *rn_version(void) {
	return "0.1.2";
}

} // extern "C"
