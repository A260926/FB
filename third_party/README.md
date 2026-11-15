# third_party

Vendored third-party dependencies. Do not edit these by hand.

## glm 1.0.1

[OpenGL Mathematics](https://github.com/g-truc/glm) — a header-only vector/matrix
maths library. Vendored as a trimmed copy: only the `glm/` header tree and its
license (`glm/copying.txt`) are included. Upstream `cmake/`, `doc/`, `test/`,
`util/`, and the C++20 module (`glm.cppm`) are omitted.

Consumed via `#include <glm/glm.hpp>` with `third_party/` on the include path.
Being header-only, it contributes no translation units to the build.

To update: replace `glm/` with the `glm/` directory from the desired release
tag at https://github.com/g-truc/glm, keep `copying.txt`, and bump the version
here and in `../THIRD_PARTY_NOTICES.md`.
