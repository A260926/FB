# Makefile for the periodic material example
#
# build_periodic_complex is a C++17 module that is compiled directly with
# the Voro++ sources, mirroring checks/build.sh. The example needs no
# windowing or online assets.

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wno-deprecated-declarations -I../../src

all: periodic_material

periodic_material: periodic_material.cc ../../src/periodic_complex.cc ../../src/voro++.cc
	$(CXX) $(CXXFLAGS) -o periodic_material \
		periodic_material.cc ../../src/periodic_complex.cc ../../src/voro++.cc

clean:
	rm -f periodic_material

.PHONY: all clean
