CXX ?= c++
CPPFLAGS := -I. -Iinclude -Itask_support/vendor/eigen
CXXFLAGS := -std=c++23 -O0 -g0 -ffp-contract=off -MMD -MP
SOURCES := $(shell find src -name '*.cpp' -print)
OBJECTS := $(patsubst src/%.cpp,.task-build/obj/%.o,$(SOURCES))
.PHONY: core FORCE
core: .task-build/libnurbs.a .task-build/baseline .task-build/driver.o
.task-build/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
.task-build/libnurbs.a: $(OBJECTS) FORCE
	@rm -f $@
	ar rcs $@ $(OBJECTS)
.task-build/baseline.o: task_support/baseline.cpp
	@mkdir -p .task-build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
.task-build/baseline: .task-build/baseline.o .task-build/libnurbs.a
	$(CXX) $^ -o $@
.task-build/driver.o: task_support/driver.c task_support/check.h
	@mkdir -p .task-build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -x c++ -c $< -o $@
-include $(OBJECTS:.o=.d) .task-build/baseline.d .task-build/driver.d
