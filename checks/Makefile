CXX = c++
CPPFLAGS = -DLNLIB_STATIC '-D__declspec(x)=' -Isrc/LNLib/include -Ichecks/vendor/eigen -Ichecks
CXXFLAGS = -std=c++17 -O0 -w
SOURCES := $(shell find src/LNLib -name '*.cpp' -print | LC_ALL=C sort)
OBJECTS := $(patsubst %.cpp,.checks-build/%.o,$(SOURCES))
.PHONY: FORCE
FORCE:
.checks-build/liblnlib.a: $(OBJECTS) FORCE
	rm -f $@
	ar rcs $@ $(OBJECTS)
.checks-build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
-include $(OBJECTS:.o=.d)
