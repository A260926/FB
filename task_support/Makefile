CXX = c++
AR = ar
CPPFLAGS = -I./src -iquote .
CXXFLAGS = -std=c++20 -O0 -g0 -w -pthread -MMD -MP
SOURCES := $(sort $(wildcard src/GEL/CGLA/*.cpp src/GEL/Geometry/*.cpp src/GEL/HMesh/*.cpp src/GEL/Util/*.cpp))
OBJECTS := $(patsubst %.cpp,.task-build/%.o,$(SOURCES))
.task-build/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
.task-build/libgel.a: $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)
.task-build/baseline: task_support/baseline.cpp .task-build/libgel.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< .task-build/libgel.a -o $@
.task-build/driver.o: task_support/driver.c task_support/check.h
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -x c++ -c $< -o $@
.PHONY: target
 target: .task-build/libgel.a .task-build/driver.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -include GEL/HMesh/SurfaceFit.h task_tests/surface_fit.cpp .task-build/driver.o src/GEL/HMesh/SurfaceFit.cpp .task-build/libgel.a -o .task-build/target
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -include GEL/HMesh/SurfaceFit.h examples/surface_fit.cpp src/GEL/HMesh/SurfaceFit.cpp .task-build/libgel.a -o .task-build/example
-include $(OBJECTS:.o=.d) .task-build/baseline.d .task-build/driver.d
