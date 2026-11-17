#include "helper.h"
void shader_step(simdfloat& value, vec2& uv) {
    value += 1.25f;
    uv += vec2(0.25f, 0.75f);
}
