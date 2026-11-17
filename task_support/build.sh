#!/bin/bash
set -euo pipefail
mkdir -p .task-build
for src in src/*.cpp lib/EasyBMP_1.06/EasyBMP.cpp; do
  [[ "$src" == src/main.cpp ]] && continue
  c++ -std=c++11 -I. -Iincludes -Itask_support/vendor -include glm/gtc/quaternion.hpp -include cstdio -include cmath -include cfloat -include limits -Wno-c++11-narrowing -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES -c "$src" -o ".task-build/$(basename "${src%.cpp}").o"
done
c++ -std=c++11 -I. -Iincludes -Itask_support/vendor -include glm/gtc/quaternion.hpp -Wno-c++11-narrowing -D_BLINN_PHONG -D_PERSPECTIVE_TEXTURES task_support/baseline.cpp .task-build/*.o -lm -o .task-build/baseline
