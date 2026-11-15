CXX = c++
AR = ar
CC = cc
CFLAGS = -O0 -g0 -MMD -MP
CPPFLAGS = -I./src -iquote .
CXXFLAGS = -std=c++20 -O0 -g0 -pthread -MMD -MP
SOURCES := $(sort $(wildcard src/GEL/CGLA/*.cpp src/GEL/Geometry/*.cpp src/GEL/HMesh/*.cpp src/GEL/Util/*.cpp))
CSOURCES := $(sort $(wildcard src/GEL/CGLA/*.c src/GEL/Geometry/*.c src/GEL/HMesh/*.c src/GEL/Util/*.c))
OBJECTS := $(patsubst %.cpp,.task-build/%.o,$(SOURCES)) $(patsubst %.c,.task-build/%.o,$(CSOURCES))
.task-build/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
.task-build/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
.task-build/libgel.a: $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)
.task-build/baseline: task_support/baseline.cpp .task-build/libgel.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< .task-build/libgel.a -o $@
.task-build/driver.o: task_support/driver.c task_support/check.h
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -x c++ -c $< -o $@
.PHONY: target example
example: .task-build/libgel.a
	@test -s examples/fair_hole.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) examples/fair_hole.cpp $(OBJECTS) -o .task-build/example
	.task-build/example
target: .task-build/libgel.a .task-build/driver.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/fair_hole_target.cpp .task-build/driver.o $(OBJECTS) -o .task-build/target
-include $(OBJECTS:.o=.d) .task-build/baseline.d .task-build/driver.d
