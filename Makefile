rasterizer: src/rasterizer.cpp src/math.h src/math.cpp src/model.h src/model.cpp src/pipeline.h
	g++ -Wall -std=c++20 -O3 -o rasterizer src/rasterizer.cpp src/model.cpp src/math.cpp -lSDL2 -fopenmp -fconcepts

# Standalone surfel example: renders an opaque triangle backdrop with coloured
# surfels and writes a PPM.  No SDL / GPU / third-party dependencies.
surfel-example: examples/surfels.cpp src/surfels.cpp src/rasterizer.cpp src/math.cpp src/model.cpp
	g++ -Wall -std=c++20 -O2 -DRASTERIZER_HEADLESS -I. -o surfel-example \
		examples/surfels.cpp src/surfels.cpp src/rasterizer.cpp src/math.cpp src/model.cpp

clean:
	rm -f rasterizer surfel-example
