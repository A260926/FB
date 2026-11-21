CXX = c++
CPPFLAGS = -DNDEBUG -DPBRT_PROBES_NONE -Isrc -Isrc/core -I.
CXXFLAGS = -std=c++11 -O1 -ffunction-sections -fdata-sections -MMD -MP
SOURCES := $(shell find src/core src/shapes src/accelerators src/materials src/integrators src/lights -name '*.cpp' ! -name 'api.cpp' ! -name 'pbrtlex.cpp' ! -name 'pbrtparse.cpp' ! -name 'parallel.cpp' ! -name 'mipmap.cpp' ! -name 'imageio.cpp' ! -name 'shapeset.cpp') task_support/runtime.cpp
OBJS := $(patsubst %.cpp,.task-build/obj/%.o,$(SOURCES))
all: $(OBJS)
.task-build/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
-include $(OBJS:.o=.d)
.PHONY: all
archive: all
	ar rcs .task-build/production.tmp.a $(OBJS)
	mv .task-build/production.tmp.a .task-build/libproduction.a
