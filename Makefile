rasterizer: src/rasterizer.cpp src/math.h src/math.cpp src/model.h src/model.cpp
	g++ -Wall -std=c++20 -O3 -o rasterizer src/rasterizer.cpp src/model.cpp src/math.cpp -lSDL2 -fopenmp -fconcepts

clean:
	rm -f rasterizer
