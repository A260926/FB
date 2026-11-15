CMAKE ?= $(shell command -v cmake 2>/dev/null || echo .venv/bin/cmake)
JOBS  ?= 8

.PHONY: build test asan bench render clean

build:
	$(CMAKE) -S . -B build -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build build -j $(JOBS)

test: build
	./build/softgpu_tests

asan:
	$(CMAKE) -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSOFTGPU_ASAN=ON
	$(CMAKE) --build build-asan -j $(JOBS)
	./build-asan/softgpu_tests

bench: build
	./build/softgpu_bench

render: build
	mkdir -p out
	./build/softgpu_render --scene cube --out out/cube.ppm --stats
	./build/softgpu_render --scene torus --out out/torus.ppm --stats
	./build/softgpu_render --scene sphere --out out/sphere.ppm --stats
	./build/softgpu_render --scene depth --out out/depth.ppm --stats --dump-stage depth

clean:
	rm -rf build build-asan out
