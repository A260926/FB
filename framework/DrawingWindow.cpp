#include <array>
#include "DrawingWindow.h"
// On some platforms you may need to include <cstring> (if you compiler can't find memset !)

DrawingWindow::DrawingWindow() {}

DrawingWindow::DrawingWindow(int w, int h, bool fullscreen) : width(w), height(h), pixelBuffer(w * h) {
	if (!SDL_Init(SDL_INIT_VIDEO))
		printMessageAndQuit("Could not initialise SDL: ", SDL_GetError());
	SDL_WindowFlags flags = SDL_WINDOW_OPENGL;
	if (fullscreen)
		flags |= SDL_WINDOW_FULLSCREEN;
	window = SDL_CreateWindow("COMS30020", width, height, flags);
	if (!window)
		printMessageAndQuit("Could not set video mode: ", SDL_GetError());
	// Use the software renderer (hardware acceleration doesn't work on all platforms).
	renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
	if (!renderer)
		printMessageAndQuit("Could not create renderer: ", SDL_GetError());
	SDL_SetRenderLogicalPresentation(renderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, width, height);
	if (!texture)
		printMessageAndQuit("Could not allocate texture: ", SDL_GetError());
	SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
}

void DrawingWindow::renderFrame() {
	SDL_UpdateTexture(texture, nullptr, pixelBuffer.data(), width * sizeof(uint32_t));
	SDL_RenderClear(renderer);
	SDL_RenderTexture(renderer, texture, nullptr, nullptr);
	SDL_RenderPresent(renderer);
}

void DrawingWindow::renderFrame(const Canvas &canvas) {
	SDL_UpdateTexture(texture, nullptr, canvas.pixels.data(), width * sizeof(uint32_t));
	SDL_RenderClear(renderer);
	SDL_RenderTexture(renderer, texture, nullptr, nullptr);
	SDL_RenderPresent(renderer);
}

void DrawingWindow::saveBMP(const std::string &filename) const {
	SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ARGB8888, (void *)pixelBuffer.data(),
	                                             width * sizeof(uint32_t));
	SDL_SaveBMP(surface, filename.c_str());
	SDL_DestroySurface(surface);
}

void DrawingWindow::savePPM(const std::string &filename) const {
	std::ofstream outputStream(filename, std::ofstream::out);
	outputStream << "P6\n";
	outputStream << width << " " << height << "\n";
	outputStream << "255\n";

	for (size_t i = 0; i < width * height; i++) {
		std::array<char, 3> rgb{{static_cast<char>((pixelBuffer[i] >> 16) & 0xFF),
		                         static_cast<char>((pixelBuffer[i] >> 8) & 0xFF),
		                         static_cast<char>((pixelBuffer[i] >> 0) & 0xFF)}};
		outputStream.write(rgb.data(), 3);
	}
	outputStream.close();
}

bool DrawingWindow::pollForInputEvents(SDL_Event &event) {
	if (SDL_PollEvent(&event)) {
		if ((event.type == SDL_EVENT_QUIT) || ((event.type == SDL_EVENT_KEY_DOWN) && (event.key.key == SDLK_ESCAPE))) {
			SDL_DestroyTexture(texture);
			SDL_DestroyRenderer(renderer);
			SDL_DestroyWindow(window);
			SDL_Quit();
			printMessageAndQuit("Exiting", nullptr);
		}
		SDL_Event dummy;
		// Clear the event queue by getting all available events
		// This seems like bad practice (because it will skip some events) however preventing backlog is paramount !
		while (SDL_PollEvent(&dummy))
			;
		return true;
	}
	return false;
}

void DrawingWindow::setPixelColour(size_t x, size_t y, uint32_t colour) {
	if ((x >= width) || (y >= height)) {
		std::cout << x << "," << y << " not on visible screen area" << std::endl;
	} else
		pixelBuffer[(y * width) + x] = colour;
}

uint32_t DrawingWindow::getPixelColour(size_t x, size_t y) {
	if ((x >= width) || (y >= height)) {
		std::cout << x << "," << y << " not on visible screen area" << std::endl;
		return 0;
	} else
		return pixelBuffer[(y * width) + x];
}

void DrawingWindow::clearPixels() {
	std::fill(pixelBuffer.begin(), pixelBuffer.end(), 0);
}

void printMessageAndQuit(const std::string &message, const char *error) {
	if (error == nullptr) {
		std::cout << message << std::endl;
		exit(0);
	} else {
		std::cout << message << " " << error << std::endl;
		exit(1);
	}
}
