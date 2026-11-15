CXX ?= c++
CPPFLAGS := -I. -Iinclude -Idependencies/glm
CXXFLAGS := -std=c++17 -O0 -g0 -ffp-contract=off -MMD -MP
.PHONY: core target-link
core: .task-build/baseline .task-build/driver.o
.task-build/baseline: task_support/baseline.cpp
	@mkdir -p .task-build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@
.task-build/driver.o: task_support/driver.c task_support/check.h
	@mkdir -p .task-build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -x c++ -c $< -o $@
target-link: .task-build/driver.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c src/patch_editor.cpp -o .task-build/patch_editor.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c src/patch_mesh.cpp -o .task-build/patch_mesh.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) checks/test_patch_editor.cpp .task-build/driver.o .task-build/patch_editor.o .task-build/patch_mesh.o -o .task-build/target
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) examples/patch_editor.cpp .task-build/patch_editor.o .task-build/patch_mesh.o -o .task-build/patch_editor
-include .task-build/baseline.d .task-build/driver.d
