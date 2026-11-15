CXX ?= c++
CPPFLAGS := -I. -Iinclude -Itask_support/vendor/eigen
CXXFLAGS := -std=c++23 -O0 -g0 -ffp-contract=off -MMD -MP
SOURCES := $(shell find src -type f -name '*.cpp' -print | LC_ALL=C sort)
OBJECTS := $(patsubst src/%.cpp,.task-build/obj/%.o,$(SOURCES))
.PHONY: core FORCE
core: .task-build/libnurbs.a .task-build/baseline .task-build/driver.o
.task-build/obj/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@
.task-build/libnurbs.a: $(OBJECTS) FORCE
	@test -n "$(SOURCES)"
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

.PHONY: target-link
target-link: $(OBJECTS) .task-build/driver.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) checks/test_parameter_domain.cpp .task-build/driver.o $(OBJECTS) -o .task-build/target
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) examples/domain_mesh.cpp $(OBJECTS) -o .task-build/domain_mesh
