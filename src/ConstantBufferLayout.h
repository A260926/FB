//=============================================================================
//
// Render/ConstantBufferLayout.h
//
// Explicit and implicit constant buffer register layout used by the GLSL
// constant buffer emulation. The result is a standalone, queryable description
// of a cbuffer: every top level field gets a linear float offset, a reserved
// length and (for arrays) an element stride. GLSL generation, C++ queries and
// the checks all consume this same result.
//
//=============================================================================

#ifndef CONSTANT_BUFFER_LAYOUT_H
#define CONSTANT_BUFFER_LAYOUT_H

#include "Engine.h"
#include "HLSLTree.h"

namespace M4
{

/** Layout description of a single top level cbuffer field. */
struct ConstantBufferFieldLayout
{
    const HLSLDeclaration* declaration;
    const char*             name;

    /** Linear offset in floats from the start of the buffer. */
    unsigned int            offset;

    /** Number of floats reserved for the field, including padding. */
    unsigned int            size;

    /** Element stride in floats for arrays, 0 for non-array fields. */
    unsigned int            arrayStride;

    /** True when the field carries an explicit packoffset. */
    bool                    explicitOffset;
};

/** Complete layout of one constant buffer. */
struct ConstantBufferLayout
{
    const HLSLBuffer*                 buffer;
    bool                              hasExplicitOffsets;
    bool                              valid;
    /** Total reserved size in floats, always rounded up to a vec4 register. */
    unsigned int                      size;
    /** Number of vec4 registers (size / 4). */
    unsigned int                      numRegisters;
    Array<ConstantBufferFieldLayout>  fields;

    explicit ConstantBufferLayout(Allocator* allocator)
        : buffer(NULL), hasExplicitOffsets(false), valid(true),
          size(0), numRegisters(0), fields(allocator) {}

    const ConstantBufferFieldLayout* FindField(const char* fieldName) const;
    const ConstantBufferFieldLayout* FindField(const HLSLDeclaration* declaration) const;
};

/** Callback used to report layout diagnostics instead of asserting. */
typedef void (*ConstantBufferLayoutErrorFn)(void* userData, const char* message);

/** Returns true if any field of the buffer carries an explicit packoffset. */
bool ConstantBufferHasPackOffset(const HLSLBuffer* buffer);

/**
 * Validates the types and constant array lengths reachable from one cbuffer
 * field (recursing into structures). Reports a diagnostic for unsupported
 * types or non-positive array lengths. Returns true when everything is valid.
 */
bool ValidateConstantBufferFieldType(HLSLTree* tree, const HLSLType& type,
                                     const char* fieldName,
                                     ConstantBufferLayoutErrorFn errorFn,
                                     void* errorUserData);

/**
 * Builds the layout of a buffer. Diagnostics are reported through the error
 * callback (if provided) and cause valid to be set to false; the returned
 * object is still populated as far as possible so callers never dereference
 * a missing result. The layout is allocated through allocator.
 */
ConstantBufferLayout* BuildConstantBufferLayout(Allocator* allocator, HLSLTree* tree,
                                                 HLSLBuffer* buffer,
                                                 ConstantBufferLayoutErrorFn errorFn = NULL,
                                                 void* errorUserData = NULL);

/** Stride in floats between elements of an array of the given element type. */
bool GetConstantArrayStride(HLSLTree* tree, const HLSLType& elementType, unsigned int& stride);

/** Linear float offset of a member within a user defined struct type. */
bool GetStructMemberOffset(HLSLTree* tree, const HLSLType& structType,
                           const char* memberName, unsigned int& offset);

/** Applies the packing alignment of the type to offset (scalars/vectors only). */
unsigned int AlignConstantBufferOffset(unsigned int offset, unsigned int alignment);

/** Returns the packing alignment (in floats) of a non-array value type. */
bool GetConstantTypeAlignment(HLSLTree* tree, const HLSLType& type, unsigned int& alignment);

/** Advances offset past a value of the given type, including array padding. */
bool AdvanceConstantBufferType(HLSLTree* tree, const HLSLType& type, unsigned int& offset);

/** Formats a linear float slot as "cN" or "cN.x/y/z/w" into buffer. */
void FormatPackOffset(unsigned int linearOffset, char* buffer, size_t bufferSize);

} // M4

#endif
