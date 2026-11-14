//=============================================================================
//
// Render/ConstantBufferLayout.cpp
//
// Computes the register layout of a cbuffer/tbuffer for the GLSL constant
// buffer emulation. Offsets are linear float slots (one vec4 register is four
// slots); the same result drives the generated uniform array length and every
// member and array access.
//
// Buffers without any packoffset reproduce the original declaration order
// layout exactly. Buffers with at least one explicit packoffset use the full
// aggregate reservation rules: scalars and vectors cannot cross a register,
// arrays, structures and matrices start at a register boundary, array and
// structure extents are rounded up to four floats and matrices occupy four
// column vectors.
//
//=============================================================================

#include "ConstantBufferLayout.h"

#include <stdio.h>
#include <string.h>

namespace M4
{

static const unsigned int kSlotsPerRegister = 4;
static const unsigned int kMaxLinearOffset  = 0x7ffffff0u;

struct TypeLayout
{
    bool            supported;
    /** Unpadded size in floats of a single value of the type. */
    unsigned int    elementSize;
    /** Alignment in floats (1, 2, 3 for vectors, 4 for aggregates). */
    unsigned int    alignment;
    /** True for arrays, structures and matrices, which must start on .x. */
    bool            aggregate;
};

static void ReportError(ConstantBufferLayoutErrorFn errorFn, void* userData, const char* message)
{
    if (errorFn != NULL)
    {
        errorFn(userData, message);
    }
}

static void ReportErrorFormat(ConstantBufferLayoutErrorFn errorFn, void* userData,
                              const char* format, ...)
{
    if (errorFn == NULL)
    {
        return;
    }
    char buffer[256];
    va_list args;
    va_start(args, format);
    String_PrintfArgList(buffer, sizeof(buffer), format, args);
    va_end(args);
    errorFn(userData, buffer);
}

/** Moves offset up so a value of the given size does not straddle a register. */
static unsigned int AlignForWrite(unsigned int offset, unsigned int size)
{
    if (size > 1 && offset / kSlotsPerRegister != (offset + size - 1) / kSlotsPerRegister)
    {
        return (offset + kSlotsPerRegister - 1) & ~(kSlotsPerRegister - 1);
    }
    return offset;
}

static unsigned int AlignToRegister(unsigned int offset)
{
    return (offset + kSlotsPerRegister - 1) & ~(kSlotsPerRegister - 1);
}

static const char* LayoutTypeName(const HLSLType& type)
{
    if (type.baseType == HLSLBaseType_UserDefined)
    {
        return type.typeName != NULL ? type.typeName : "?";
    }
    switch (type.baseType)
    {
    case HLSLBaseType_Float:        return "float";
    case HLSLBaseType_Float2:       return "float2";
    case HLSLBaseType_Float3:       return "float3";
    case HLSLBaseType_Float4:       return "float4";
    case HLSLBaseType_Float4x4:     return "float4x4";
    case HLSLBaseType_Half:         return "half";
    case HLSLBaseType_Half2:        return "half2";
    case HLSLBaseType_Half3:        return "half3";
    case HLSLBaseType_Half4:        return "half4";
    case HLSLBaseType_Int:          return "int";
    case HLSLBaseType_Uint:         return "uint";
    case HLSLBaseType_Bool:         return "bool";
    default:                        return "?";
    }
}

static bool GetTypeLayout(HLSLTree* tree, const HLSLType& type, TypeLayout& result,
                          ConstantBufferLayoutErrorFn errorFn, void* userData);

/** Unpadded size of a (non-array) value of the type, in floats. */
static bool GetElementTypeSize(HLSLTree* tree, const HLSLType& type, unsigned int& size,
                               ConstantBufferLayoutErrorFn errorFn, void* userData)
{
    switch (type.baseType)
    {
    case HLSLBaseType_Float:
        size = 1;
        return true;
    case HLSLBaseType_Float2:
        size = 2;
        return true;
    case HLSLBaseType_Float3:
        size = 3;
        return true;
    case HLSLBaseType_Float4:
        size = 4;
        return true;
    case HLSLBaseType_Float4x4:
        // Four column vectors.
        size = 16;
        return true;
    case HLSLBaseType_UserDefined:
        {
            HLSLStruct* structure = tree->FindGlobalStruct(type.typeName);
            if (structure == NULL)
            {
                ReportErrorFormat(errorFn, userData, "Unknown type %s", type.typeName);
                return false;
            }
            unsigned int offset = 0;
            for (HLSLStructField* field = structure->field; field != NULL; field = field->nextField)
            {
                TypeLayout fieldLayout;
                if (!GetTypeLayout(tree, field->type, fieldLayout, errorFn, userData))
                {
                    return false;
                }
                unsigned int aligned = AlignForWrite(offset, fieldLayout.alignment);
                if (field->type.array)
                {
                    int arraySize = 0;
                    if (!tree->GetExpressionValue(field->type.arraySize, arraySize) || arraySize <= 0)
                    {
                        ReportErrorFormat(errorFn, userData,
                            "Field '%s' has an invalid constant array length", field->name);
                        return false;
                    }
                    unsigned int memberLength = static_cast<unsigned int>(arraySize);
                    if (memberLength > kMaxLinearOffset / fieldLayout.elementSize)
                    {
                        ReportErrorFormat(errorFn, userData,
                            "Field '%s' has an array length that is out of range", field->name);
                        return false;
                    }
                    offset = aligned + fieldLayout.elementSize * memberLength;
                }
                else
                {
                    offset = aligned + fieldLayout.elementSize;
                }
            }
            size = offset;
            return true;
        }
    default:
        ReportErrorFormat(errorFn, userData,
            "Constant buffer layout is not supported for %s", LayoutTypeName(type));
        return false;
    }
}

static bool GetTypeLayout(HLSLTree* tree, const HLSLType& type, TypeLayout& result,
                          ConstantBufferLayoutErrorFn errorFn, void* userData)
{
    result.supported  = false;
    result.aggregate  = type.array;

    if (type.array)
    {
        // Arrays always start on a register boundary and reserve register
        // rounded space per element.
        unsigned int elementSize = 0;
        HLSLType elementType = type;
        elementType.array = false;
        if (!GetElementTypeSize(tree, elementType, elementSize, errorFn, userData))
        {
            return false;
        }
        result.elementSize = AlignToRegister(elementSize);
        result.alignment   = kSlotsPerRegister;
        result.aggregate   = true;
        result.supported   = true;
        return true;
    }

    unsigned int elementSize = 0;
    if (!GetElementTypeSize(tree, type, elementSize, errorFn, userData))
    {
        return false;
    }
    result.elementSize = elementSize;

    switch (type.baseType)
    {
    case HLSLBaseType_Float:
        result.alignment = 1;
        break;
    case HLSLBaseType_Float2:
        result.alignment = 2;
        break;
    case HLSLBaseType_Float3:
        result.alignment = 3;
        break;
    case HLSLBaseType_Float4:
        result.alignment = 4;
        break;
    case HLSLBaseType_Float4x4:
        result.alignment = 4;
        result.aggregate  = true;
        break;
    case HLSLBaseType_UserDefined:
        result.alignment = kSlotsPerRegister;
        result.aggregate  = true;
        break;
    default:
        break;
    }

    result.supported = true;
    return true;
}

bool GetConstantArrayStride(HLSLTree* tree, const HLSLType& elementType, unsigned int& stride)
{
    unsigned int elementSize = 0;
    if (!GetElementTypeSize(tree, elementType, elementSize, NULL, NULL))
    {
        return false;
    }
    stride = AlignToRegister(elementSize);
    return true;
}

unsigned int AlignConstantBufferOffset(unsigned int offset, unsigned int alignment)
{
    return AlignForWrite(offset, alignment);
}

bool GetConstantTypeAlignment(HLSLTree* tree, const HLSLType& type, unsigned int& alignment)
{
    TypeLayout typeLayout;
    if (!GetTypeLayout(tree, type, typeLayout, NULL, NULL))
    {
        return false;
    }
    alignment = typeLayout.alignment;
    return true;
}

bool AdvanceConstantBufferType(HLSLTree* tree, const HLSLType& type, unsigned int& offset)
{
    TypeLayout typeLayout;
    if (!GetTypeLayout(tree, type, typeLayout, NULL, NULL))
    {
        return false;
    }

    offset = AlignForWrite(offset, typeLayout.alignment);

    if (type.array)
    {
        int arraySize = 0;
        if (!tree->GetExpressionValue(type.arraySize, arraySize) || arraySize <= 0)
        {
            return false;
        }
        offset += typeLayout.elementSize * static_cast<unsigned int>(arraySize);
    }
    else
    {
        offset += typeLayout.elementSize;
    }
    return true;
}

bool GetStructMemberOffset(HLSLTree* tree, const HLSLType& structType,
                           const char* memberName, unsigned int& offset)
{
    if (structType.baseType != HLSLBaseType_UserDefined)
    {
        return false;
    }
    HLSLStruct* structure = tree->FindGlobalStruct(structType.typeName);
    if (structure == NULL)
    {
        return false;
    }

    unsigned int cursor = 0;
    for (HLSLStructField* field = structure->field; field != NULL; field = field->nextField)
    {
        TypeLayout fieldLayout;
        if (!GetTypeLayout(tree, field->type, fieldLayout, NULL, NULL))
        {
            return false;
        }
        cursor = AlignForWrite(cursor, fieldLayout.alignment);
        if (String_Equal(field->name, memberName))
        {
            offset = cursor;
            return true;
        }
        if (field->type.array)
        {
            int arraySize = 0;
            if (!tree->GetExpressionValue(field->type.arraySize, arraySize) || arraySize <= 0)
            {
                return false;
            }
            cursor += fieldLayout.elementSize * static_cast<unsigned int>(arraySize);
        }
        else
        {
            cursor += fieldLayout.elementSize;
        }
    }
    return false;
}

bool ConstantBufferHasPackOffset(const HLSLBuffer* buffer)
{
    for (HLSLDeclaration* field = buffer->field; field != NULL;
         field = static_cast<HLSLDeclaration*>(field->nextStatement))
    {
        if (field->hasPackOffset)
        {
            return true;
        }
    }
    return false;
}

bool ValidateConstantBufferFieldType(HLSLTree* tree, const HLSLType& type,
                                     const char* fieldName,
                                     ConstantBufferLayoutErrorFn errorFn,
                                     void* errorUserData)
{
    bool ok = true;

    if (type.array)
    {
        int arraySize = 0;
        if (!tree->GetExpressionValue(type.arraySize, arraySize) || arraySize <= 0)
        {
            ReportErrorFormat(errorFn, errorUserData,
                "Constant buffer field '%s' requires a positive constant array length",
                fieldName != NULL ? fieldName : "?");
            ok = false;
        }
        HLSLType elementType = type;
        elementType.array = false;
        if (!ValidateConstantBufferFieldType(tree, elementType, fieldName, errorFn, errorUserData))
        {
            ok = false;
        }
        return ok;
    }

    if (type.baseType == HLSLBaseType_UserDefined)
    {
        HLSLStruct* structure = tree->FindGlobalStruct(type.typeName);
        if (structure == NULL)
        {
            ReportErrorFormat(errorFn, errorUserData, "Unknown type %s", type.typeName);
            return false;
        }
        for (HLSLStructField* field = structure->field; field != NULL; field = field->nextField)
        {
            if (!ValidateConstantBufferFieldType(tree, field->type, field->name,
                                                  errorFn, errorUserData))
            {
                ok = false;
            }
        }
        return ok;
    }

    unsigned int elementSize = 0;
    if (!GetElementTypeSize(tree, type, elementSize, errorFn, errorUserData))
    {
        ok = false;
    }
    return ok;
}

void FormatPackOffset(unsigned int linearOffset, char* buffer, size_t bufferSize)
{
    unsigned int reg = linearOffset / kSlotsPerRegister;
    unsigned int comp = linearOffset % kSlotsPerRegister;
    if (comp == 0)
    {
        String_Printf(buffer, static_cast<int>(bufferSize), "c%u", reg);
    }
    else
    {
        String_Printf(buffer, static_cast<int>(bufferSize), "c%u.%c", reg, "xyzw"[comp]);
    }
}

const ConstantBufferFieldLayout* ConstantBufferLayout::FindField(const char* fieldName) const
{
    for (int i = 0; i < fields.GetSize(); ++i)
    {
        if (String_Equal(fields[i].name, fieldName))
        {
            return &fields[i];
        }
    }
    return NULL;
}

const ConstantBufferFieldLayout* ConstantBufferLayout::FindField(const HLSLDeclaration* declaration) const
{
    for (int i = 0; i < fields.GetSize(); ++i)
    {
        if (fields[i].declaration == declaration)
        {
            return &fields[i];
        }
    }
    return NULL;
}

ConstantBufferLayout* BuildConstantBufferLayout(Allocator* allocator, HLSLTree* tree,
                                                 HLSLBuffer* buffer,
                                                 ConstantBufferLayoutErrorFn errorFn,
                                                 void* errorUserData)
{
    ConstantBufferLayout* layout = allocator->New<ConstantBufferLayout>();
    new (layout) ConstantBufferLayout(allocator);
    layout->buffer              = buffer;
    layout->hasExplicitOffsets  = ConstantBufferHasPackOffset(buffer);
    layout->valid               = true;
    layout->size                = 0;
    layout->numRegisters        = 0;
    new (&layout->fields) Array<ConstantBufferFieldLayout>(allocator);

    unsigned int cursor = 0;
    unsigned int highWaterMark = 0;

    for (HLSLDeclaration* field = buffer->field; field != NULL;
         field = static_cast<HLSLDeclaration*>(field->nextStatement))
    {
        ConstantBufferFieldLayout fieldLayout;
        fieldLayout.declaration    = field;
        fieldLayout.name           = field->name;
        fieldLayout.offset         = 0;
        fieldLayout.size           = 0;
        fieldLayout.arrayStride    = 0;
        fieldLayout.explicitOffset = field->hasPackOffset;

        TypeLayout typeLayout;
        bool typeOk = GetTypeLayout(tree, field->type, typeLayout, errorFn, errorUserData);
        if (!typeOk)
        {
            layout->valid = false;
        }

        unsigned int reservedSize = 0;
        unsigned int arrayLength  = 0;
        if (typeOk)
        {
            if (field->type.array)
            {
                int arraySize = 0;
                if (!tree->GetExpressionValue(field->type.arraySize, arraySize) || arraySize <= 0)
                {
                    ReportErrorFormat(errorFn, errorUserData,
                        "Constant buffer field '%s' requires a positive constant array length",
                        field->name);
                    layout->valid = false;
                }
                else
                {
                    arrayLength = static_cast<unsigned int>(arraySize);
                    if (arrayLength > kMaxLinearOffset / typeLayout.elementSize)
                    {
                        ReportErrorFormat(errorFn, errorUserData,
                            "packoffset for field '%s' is out of range", field->name);
                        layout->valid = false;
                    }
                    else
                    {
                        reservedSize = typeLayout.elementSize * arrayLength;
                        fieldLayout.arrayStride = typeLayout.elementSize;
                    }
                }
            }
            else
            {
                reservedSize = typeLayout.elementSize;
            }
        }

        unsigned int start = 0;
        if (field->hasPackOffset)
        {
            if (field->packOffset < 0)
            {
                ReportError(errorFn, errorUserData, "Invalid packoffset");
                layout->valid = false;
                start = 0;
            }
            else
            {
                start = static_cast<unsigned int>(field->packOffset);
            }

            if (start > kMaxLinearOffset || reservedSize > kMaxLinearOffset ||
                start + reservedSize < start)
            {
                ReportErrorFormat(errorFn, errorUserData,
                    "packoffset for field '%s' is out of range", field->name);
                layout->valid = false;
            }
            else if (typeOk)
            {
                if (typeLayout.aggregate && (start % kSlotsPerRegister) != 0)
                {
                    char position[32];
                    FormatPackOffset(start, position, sizeof(position));
                    ReportErrorFormat(errorFn, errorUserData,
                        "Field '%s' at %s: arrays, structures and matrices must be packed at the start of a register",
                        field->name, position);
                    layout->valid = false;
                }
                else if (!typeLayout.aggregate &&
                         AlignForWrite(start, typeLayout.elementSize) != start)
                {
                    char position[32];
                    FormatPackOffset(start, position, sizeof(position));
                    ReportErrorFormat(errorFn, errorUserData,
                        "Field '%s' at %s crosses a register boundary",
                        field->name, position);
                    layout->valid = false;
                }
            }
        }
        else
        {
            // Automatic fields continue from the current cursor; explicit
            // fields never pull the cursor backwards.
            start = typeOk ? AlignForWrite(cursor, typeLayout.alignment) : cursor;
        }

        fieldLayout.offset = start;
        fieldLayout.size   = reservedSize;
        layout->fields.PushBack(fieldLayout);

        unsigned int end = start + reservedSize;
        if (end > highWaterMark)
        {
            highWaterMark = end;
        }
        if (end > cursor)
        {
            cursor = end;
        }
    }

    // Pairwise occupancy check: every reserved float, including aggregate
    // padding, belongs to at most one field.
    for (int i = 0; i < layout->fields.GetSize() && layout->valid; ++i)
    {
        const ConstantBufferFieldLayout& a = layout->fields[i];
        if (a.size == 0)
        {
            continue;
        }
        for (int j = i + 1; j < layout->fields.GetSize(); ++j)
        {
            const ConstantBufferFieldLayout& b = layout->fields[j];
            if (b.size == 0)
            {
                continue;
            }
            if (a.offset < b.offset + b.size && b.offset < a.offset + a.size)
            {
                char posA[32];
                char posB[32];
                FormatPackOffset(a.offset, posA, sizeof(posA));
                FormatPackOffset(b.offset, posB, sizeof(posB));
                ReportErrorFormat(errorFn, errorUserData,
                    "Constant buffer packoffset conflict: fields '%s' (%s) and '%s' (%s) overlap",
                    a.name, posA, b.name, posB);
                layout->valid = false;
                break;
            }
        }
    }

    // The buffer size is the highest occupied end, rounded up to a register,
    // not the end of the final declaration.
    layout->size         = AlignToRegister(highWaterMark);
    layout->numRegisters = layout->size / kSlotsPerRegister;

    return layout;
}

} // M4
