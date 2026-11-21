CXX := c++
CPPFLAGS := -Iinclude -Itask_support/vendor/eigen -I.
CXXFLAGS := -std=c++23 -O1 -MMD -MP
SOURCES := $(shell find src -name '*.cpp' -type f)
OBJECTS := $(patsubst src/%.cpp,.task-build/obj/%.o,$(SOURCES))
HEADERS := include/libnurbs/Surface/OffsetFit.hpp include/libnurbs/Algorithm/OffsetBounds.hpp
.PHONY: base target
base: .task-build/baseline
.task-build/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
.task-build/baseline: $(OBJECTS) task_support/baseline.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) task_support/baseline.cpp $(OBJECTS) -o $@
target: $(OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -include $(word 1,$(HEADERS)) -include $(word 2,$(HEADERS)) -c src/Surface/OffsetFit.cpp -o .task-build/required-offset.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -include $(word 1,$(HEADERS)) -include $(word 2,$(HEADERS)) -c src/Algorithm/OffsetBounds.cpp -o .task-build/required-bounds.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -include $(word 1,$(HEADERS)) -include $(word 2,$(HEADERS)) task_support/driver.c checks/test_offset.cpp $(OBJECTS) -o .task-build/target
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -include $(word 1,$(HEADERS)) -include $(word 2,$(HEADERS)) examples/offset_obj.cpp $(OBJECTS) -o .task-build/offset-example
-include $(OBJECTS:.o=.d)
