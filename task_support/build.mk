CXX = c++
CPPFLAGS = -Isrc -I.
CXXFLAGS = -std=c++17 -O1 -MMD -MP
SOURCES := $(filter-out src/MiniLight.cpp,$(wildcard src/*.cpp))
OBJECTS := $(patsubst src/%.cpp,.task-build/obj/%.o,$(SOURCES))
.PHONY: archive
archive: $(OBJECTS)
	ar rcs .task-build/production.tmp.a $(OBJECTS)
	mv .task-build/production.tmp.a .task-build/libproduction.a
.task-build/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
-include $(OBJECTS:.o=.d)
