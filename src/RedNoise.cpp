#include "Camera.h"
#include "ObjLoader.h"
#include "Renderer.h"
#include <Canvas.h>
#include <DrawingWindow.h>
#include <ModelTriangle.h>
#include <glm/glm.hpp>
#include <iostream>
#include <vector>

#define WIDTH 320
#define HEIGHT 240

enum class RenderMode { Wireframe, Rasterised, Raytraced, PathTraced };

static RenderMode renderMode = RenderMode::Rasterised;
static ShadingModel shadingModel = ShadingModel::Phong;
static Camera camera(WIDTH, HEIGHT, 2.0f, glm::vec3(0.0f, 0.0f, 4.0f));
static Canvas canvas(WIDTH, HEIGHT);
static bool orbiting = false;
static bool backfaceCull = false;

static const std::vector<ModelTriangle> &scene() {
	static std::vector<ModelTriangle> triangles = loadOBJ("assets/cornell-box.obj", 0.35f);
	static bool logged = false;
	if (!logged) {
		std::cout << "Loaded " << triangles.size() << " triangles from cornell-box.obj" << std::endl;
		logged = true;
	}
	return triangles;
}

void draw() {
	const std::vector<ModelTriangle> &model = scene();
	if (orbiting)
		camera.orbitY(0.02f, glm::vec3(0.0f, 0.0f, 0.0f));
	switch (renderMode) {
	case RenderMode::Wireframe:
		renderWireframe(model, camera, canvas);
		break;
	case RenderMode::Rasterised:
		renderRasterised(model, camera, canvas, backfaceCull);
		break;
	case RenderMode::Raytraced:
		renderRaytraced(model, camera, canvas, shadingModel);
		break;
	case RenderMode::PathTraced:
		renderPathTraced(model, camera, canvas, 8); // few samples for interactivity
		break;
	}
}

void handleEvent(SDL_Event event) {
	if (event.type == SDL_EVENT_KEY_DOWN) {
		float step = 0.1f;
		switch (event.key.key) {
		// Camera translation.
		case SDLK_A:
			camera.translate(glm::vec3(-step, 0.0f, 0.0f));
			break;
		case SDLK_D:
			camera.translate(glm::vec3(step, 0.0f, 0.0f));
			break;
		case SDLK_W:
			camera.translate(glm::vec3(0.0f, step, 0.0f));
			break;
		case SDLK_S:
			camera.translate(glm::vec3(0.0f, -step, 0.0f));
			break;
		case SDLK_Q:
			camera.translate(glm::vec3(0.0f, 0.0f, -step));
			break;
		case SDLK_E:
			camera.translate(glm::vec3(0.0f, 0.0f, step));
			break;
		// Orientation (arrow keys pan/tilt).
		case SDLK_LEFT:
			camera.rotateY(-0.05f);
			break;
		case SDLK_RIGHT:
			camera.rotateY(0.05f);
			break;
		case SDLK_UP:
			camera.rotateX(-0.05f);
			break;
		case SDLK_DOWN:
			camera.rotateX(0.05f);
			break;
		case SDLK_Z:
			camera.rotateZ(-0.05f); // roll left
			break;
		case SDLK_X:
			camera.rotateZ(0.05f); // roll right
			break;
		// Camera actions.
		case SDLK_L:
			camera.lookAt(glm::vec3(0.0f, 0.0f, 0.0f));
			break;
		case SDLK_O:
			orbiting = !orbiting;
			break;
		case SDLK_C:
			backfaceCull = !backfaceCull;
			break;
		case SDLK_R:
			camera = Camera(WIDTH, HEIGHT, 2.0f, glm::vec3(0.0f, 0.0f, 4.0f));
			camera.lookAt(glm::vec3(0.0f, 0.0f, 0.0f));
			orbiting = false;
			break;
		// Render modes.
		case SDLK_1:
			renderMode = RenderMode::Wireframe;
			std::cout << "wireframe" << std::endl;
			break;
		case SDLK_2:
			renderMode = RenderMode::Rasterised;
			std::cout << "rasterised" << std::endl;
			break;
		case SDLK_3:
			renderMode = RenderMode::Raytraced;
			std::cout << "raytraced" << std::endl;
			break;
		case SDLK_G:
			renderMode = RenderMode::PathTraced;
			std::cout << "path traced" << std::endl;
			break;
		// Shading model (ray tracer).
		case SDLK_4:
			shadingModel = ShadingModel::Flat;
			std::cout << "flat" << std::endl;
			break;
		case SDLK_5:
			shadingModel = ShadingModel::Gouraud;
			std::cout << "gouraud" << std::endl;
			break;
		case SDLK_6:
			shadingModel = ShadingModel::Phong;
			std::cout << "phong" << std::endl;
			break;
		// Save a screenshot.
		case SDLK_P:
			canvas.savePPM("output.ppm");
			std::cout << "saved output.ppm" << std::endl;
			break;
		default:
			break;
		}
	} else if (event.type == SDL_EVENT_MOUSE_MOTION) {
		// Drag with the left mouse button held to look around.
		if (event.motion.state & SDL_BUTTON_LMASK) {
			camera.rotateY(event.motion.xrel * 0.005f);
			camera.rotateX(event.motion.yrel * 0.005f);
		}
	}
}

int main(int argc, char *argv[]) {
	DrawingWindow window = DrawingWindow(WIDTH, HEIGHT, false);
	camera.lookAt(glm::vec3(0.0f, 0.0f, 0.0f));
	SDL_Event event;
	while (true) {
		// We MUST poll for events - otherwise the window will freeze !
		if (window.pollForInputEvents(event))
			handleEvent(event);
		draw();
		// Need to render the frame at the end, or nothing actually gets shown on the screen!
		window.renderFrame(canvas);
	}
	return 0;
}
