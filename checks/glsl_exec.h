#ifndef CHECKS_GLSL_INTERPRETER_H
#define CHECKS_GLSL_INTERPRETER_H

// A very small interpreter for the subset of GLSL 140 that the matrix tests
// generate.  It is deliberately written in standard C++11 and understands
// only what the tests need:
//
//   float/vec2/vec3/vec4/mat2/mat3/mat4 types, constructors, swizzles,
//   matrix subscripts, + - * / compound assignment, if/else, for, while,
//   break, continue, return, user functions with out/inout parameters, and
//   comma expressions.
//
// It works on the generated GLSL text, not on the HLSL tree, so the tests
// check the actual translation result numerically.

#include <string>
#include <vector>

namespace glsl
{

struct Value
{
    Value();
    Value(float x, float y = 0.0f, float z = 0.0f, float w = 0.0f);

    // Scalar or vector components.  A matrix is stored by columns.
    int       width;        // 1, 2, 3 or 4
    float     data[4];
    bool      isMatrix;
    int       size;         // matrix dimension
    float     columns[4][4];

    static Value Scalar(float x);
    static Value Vector(int width, const float* values);
    static Value Matrix(int size);
};

struct Result
{
    bool        ok;
    std::string error;
    Value       shaderOutput;   // value written to rast_FragData[0]
    int         outWrites;      // number of stores to output arguments / arrays

    Result() : ok(false), outWrites(0) {}
};

/**
 * Executes the fragment shader entry point with the supplied fragment
 * input.  Only the first float2 input attribute is supported.
 */
Result RunFragment(const std::string& source, float uvX, float uvY);

} // namespace glsl

#endif
