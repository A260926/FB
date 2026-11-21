CXX = c++
CXXFLAGS = -O1 -std=c++20 -ISources
SOURCES = Sources/BSplineLib/InputOutput/iges.cpp Sources/BSplineLib/InputOutput/operations.cpp Sources/BSplineLib/ParameterSpaces/knot_vector.cpp Sources/BSplineLib/Splines/spline_item.cpp Sources/BSplineLib/Utilities/math_operations.cpp Sources/BSplineLib/Utilities/string_operations.cpp Sources/BSplineLib/Utilities/system_operations.cpp Sources/BSplineLib/VectorSpaces/vector_space.cpp Sources/BSplineLib/VectorSpaces/weighted_vector_space.cpp
OBJECTS = $(patsubst Sources/%.cpp,.task-build/core/%.o,$(SOURCES))
.task-build/core/%.o: Sources/%.cpp $(shell find Sources -name "*.hpp" -o -name "*.inl")
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@
.task-build/libbspline.a: $(OBJECTS)
	ar rcs $@ $(OBJECTS)
.task-build/baseline: task_support/baseline.cpp .task-build/libbspline.a
	$(CXX) $(CXXFLAGS) $< .task-build/libbspline.a -o $@
