//=============================================================================
//
// CBufferLayout.h
//
// Explicit packoffset aware constant buffer layout.
//
// The layout is expressed in float offsets: register cN covers float offsets
// 4*N .. 4*N+3 and the components .x/.y/.z/.w correspond to 0/1/2/3.
//
// A CBufferLayout is a standalone queryable result. The GLSL generator uses
// the exact same Build() result to size the emulated vec4 uniform array and to
// resolve every top level field access, so external C++ code can query the
// offsets/reserved sizes/array strides and rely on the generated expressions
// landing on the same addresses.
//
// The implementation is header-only (inline) so existing build commands that
// list the original source files still link when GLSLGenerator is used.
//
//=============================================================================

#ifndef CBUFFER_LAYOUT_H
#define CBUFFER_LAYOUT_H

#include "HLSLTree.h"
#include "Engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace M4
{

class HLSLTree;
struct HLSLBuffer;
struct HLSLDeclaration;
struct HLSLType;

namespace CBufferLayoutDetail
{
    inline void Printf(std::string& out, const char* format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        String_PrintfArgList(buffer, sizeof(buffer), format, args);
        va_end(args);
        out = buffer;
    }

    inline const char* TypeName(const HLSLType& type)
    {
        switch (type.baseType)
        {
        case HLSLBaseType_Float:    return "float";
        case HLSLBaseType_Float2:   return "float2";
        case HLSLBaseType_Float3:   return "float3";
        case HLSLBaseType_Float4:   return "float4";
        case HLSLBaseType_Float4x4: return "float4x4";
        case HLSLBaseType_Half:     return "half";
        case HLSLBaseType_Half2:    return "half2";
        case HLSLBaseType_Half3:    return "half3";
        case HLSLBaseType_Half4:    return "half4";
        case HLSLBaseType_Int:      return "int";
        case HLSLBaseType_Int2:     return "int2";
        case HLSLBaseType_Int3:     return "int3";
        case HLSLBaseType_Int4:     return "int4";
        case HLSLBaseType_Uint:     return "uint";
        case HLSLBaseType_Bool:     return "bool";
        case HLSLBaseType_UserDefined:
            return type.typeName != NULL ? type.typeName : "struct";
        default:                    return "unsupported type";
        }
    }

    /** Same register crossing rule as the original simulator alignForWrite. */
    inline void AlignForWrite(unsigned int& offset, unsigned int size)
    {
        if (offset / 4 != (offset + size - 1) / 4)
        {
            offset = (offset + 3u) & ~3u;
        }
    }
}

class CBufferLayout
{

public:

    /** Safety bound on every computed float offset. */
    enum { MaxFloatOffset = 64u * 1024u * 1024u };

    struct Field
    {
        Field()
        {
            declaration    = NULL;
            offset         = 0;
            size           = 0;
            arrayStride    = 0;
            arrayCount     = 0;
            explicitOffset = false;
            packRegister   = 0;
            packComponent  = 0;
        }

        const HLSLDeclaration* declaration;
        std::string             name;

        unsigned int offset;         // First float component occupied.
        unsigned int size;           // Reserved floats incl. aggregate padding.
        unsigned int arrayStride;    // Float stride between elements (0 non-array).
        unsigned int arrayCount;     // Number of elements (0 when not an array).

        bool explicitOffset;         // True for an explicit : packoffset(...).
        int  packRegister;           // Explicit cN register.
        int  packComponent;          // Explicit 0..3 component.

        unsigned int GetEnd() const { return offset + size; }
    };

    enum Rules
    {
        /** Legacy simulator packing; unchanged for packoffset-free buffers. */
        Rules_Legacy,
        /** Complete aggregate rules: struct ends are 4-float padded/reserved. */
        Rules_Complete,
    };

    CBufferLayout()
    {
        m_valid       = false;
        m_hasExplicit = false;
        m_occupiedEnd = 0;
    }

    bool IsValid() const          { return m_valid; }
    const std::string& GetError() const { return m_error; }
    bool HasExplicitOffsets() const     { return m_hasExplicit; }

    unsigned int GetFieldCount() const { return static_cast<unsigned int>(m_fields.size()); }
    const Field& GetField(unsigned int index) const { return m_fields[index]; }

    const Field* FindField(const char* name) const
    {
        for (size_t i = 0; i < m_fields.size(); ++i)
        {
            if (m_fields[i].name == name)
            {
                return &m_fields[i];
            }
        }
        return NULL;
    }

    const Field* FindFieldByDeclaration(const HLSLDeclaration* declaration) const
    {
        for (size_t i = 0; i < m_fields.size(); ++i)
        {
            if (m_fields[i].declaration == declaration)
            {
                return &m_fields[i];
            }
        }
        return NULL;
    }

    unsigned int GetOccupiedEnd() const  { return m_occupiedEnd; }
    unsigned int GetSizeFloats() const   { return (m_occupiedEnd + 3u) & ~3u; }
    unsigned int GetRegisterCount() const { return GetSizeFloats() / 4u; }

    //-------------------------------------------------------------------------
    // Shared type layout rules.
    //-------------------------------------------------------------------------

    static unsigned int AlignOffsetForType(const HLSLType& type, unsigned int offset)
    {
        using namespace CBufferLayoutDetail;

        if (type.array)
        {
            AlignForWrite(offset, 4);
        }
        else
        {
            switch (type.baseType)
            {
            case HLSLBaseType_Float:
                AlignForWrite(offset, 1);
                break;
            case HLSLBaseType_Float2:
                AlignForWrite(offset, 2);
                break;
            case HLSLBaseType_Float3:
                AlignForWrite(offset, 3);
                break;
            case HLSLBaseType_Float4:
            case HLSLBaseType_Float4x4:
            case HLSLBaseType_UserDefined:
                AlignForWrite(offset, 4);
                break;
            default:
                // Reported by MeasureElementType; leave offset alone.
                break;
            }
        }
        return offset;
    }

    static bool MeasureElementType(HLSLTree* tree, Rules rules, const HLSLType& type,
                                   unsigned int& size, std::string& error)
    {
        using namespace CBufferLayoutDetail;

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
            // Four column vectors, exactly as in the original emulation mode.
            size = 16;
            return true;
        case HLSLBaseType_UserDefined:
        {
            if (tree == NULL || type.typeName == NULL)
            {
                Printf(error, "Constant buffer layout: incomplete struct type");
                return false;
            }

            HLSLStruct* st = tree->FindGlobalStruct(type.typeName);
            if (st == NULL)
            {
                Printf(error, "Constant buffer layout: unknown type '%s'", type.typeName);
                return false;
            }

            unsigned int offset = 0;
            for (HLSLStructField* field = st->field; field != NULL; field = field->nextField)
            {
                if (!AdvanceOffset(tree, rules, field->type, offset, error))
                {
                    return false;
                }
                if (offset > static_cast<unsigned int>(MaxFloatOffset))
                {
                    Printf(error, "Constant buffer layout for struct '%s' is too large", st->name);
                    return false;
                }
            }

            if (rules == Rules_Complete)
            {
                // Structs start and end on a register boundary, so the
                // trailing padding is reserved by the struct.
                offset = (offset + 3u) & ~3u;
            }

            size = offset;
            return true;
        }
        default:
            Printf(error, "Constant buffer layout is not supported for %s", TypeName(type));
            return false;
        }
    }

    static bool GetArrayInfo(HLSLTree* tree, Rules rules, const HLSLType& arrayType,
                             int& count, unsigned int& elementSize,
                             unsigned int& stride, std::string& error)
    {
        using namespace CBufferLayoutDetail;

        count = 0;
        elementSize = 0;
        stride = 0;

        if (!arrayType.array)
        {
            Printf(error, "Internal layout error: expected an array type");
            return false;
        }

        if (tree == NULL || arrayType.arraySize == NULL ||
            !tree->GetExpressionValue(arrayType.arraySize, count))
        {
            Printf(error, "Constant buffer array length must be a known integer constant");
            return false;
        }

        if (count <= 0)
        {
            Printf(error, "Constant buffer array length must be positive (got %d)", count);
            return false;
        }

        if (!MeasureElementType(tree, rules, arrayType, elementSize, error))
        {
            return false;
        }

        if (elementSize > static_cast<unsigned int>(MaxFloatOffset))
        {
            Printf(error, "Constant buffer array element is too large");
            return false;
        }

        stride = (elementSize + 3u) & ~3u;
        return true;
    }

    static bool AdvanceOffset(HLSLTree* tree, Rules rules, const HLSLType& type,
                              unsigned int& offset, std::string& error)
    {
        offset = AlignOffsetForType(type, offset);

        unsigned int extent = 0;

        if (type.array)
        {
            int count = 0;
            unsigned int elementSize = 0;
            unsigned int stride = 0;
            if (!GetArrayInfo(tree, rules, type, count, elementSize, stride, error))
            {
                return false;
            }

            unsigned long long total =
                static_cast<unsigned long long>(stride) * static_cast<unsigned long long>(count);
            if (total > static_cast<unsigned long long>(MaxFloatOffset) ||
                static_cast<unsigned long long>(offset) + total >
                    static_cast<unsigned long long>(MaxFloatOffset))
            {
                CBufferLayoutDetail::Printf(
                    error, "Constant buffer layout exceeds the supported %d float bound",
                    static_cast<int>(MaxFloatOffset));
                return false;
            }
            extent = static_cast<unsigned int>(total);
        }
        else
        {
            if (!MeasureElementType(tree, rules, type, extent, error))
            {
                return false;
            }
            if (static_cast<unsigned long long>(offset) + extent >
                static_cast<unsigned long long>(MaxFloatOffset))
            {
                CBufferLayoutDetail::Printf(
                    error, "Constant buffer layout exceeds the supported %d float bound",
                    static_cast<int>(MaxFloatOffset));
                return false;
            }
        }

        offset += extent;
        return true;
    }

    /** Parses the text inside packoffset(...), e.g. "c12" or "c3.z". */
    static bool ParsePackOffsetText(const char* text, int& registerNumber,
                                    int& component, std::string& error)
    {
        using namespace CBufferLayoutDetail;

        registerNumber = 0;
        component = 0;

        if (text == NULL || text[0] == 0)
        {
            Printf(error, "packoffset is missing its cN register");
            return false;
        }

        if ((text[0] != 'c' && text[0] != 'C') || text[1] < '0' || text[1] > '9')
        {
            Printf(error, "packoffset '%s' is invalid: expected c<register>", text);
            return false;
        }

        unsigned long long reg = 0;
        int digits = 0;
        int i = 1;
        const unsigned long long maxRegister =
            static_cast<unsigned long long>(MaxFloatOffset / 4u);
        while (text[i] >= '0' && text[i] <= '9')
        {
            reg = reg * 10ull + static_cast<unsigned long long>(text[i] - '0');
            if (reg > maxRegister)
            {
                Printf(error, "packoffset register in '%s' is out of range", text);
                return false;
            }
            ++digits;
            ++i;
        }

        if (digits == 0)
        {
            Printf(error, "packoffset '%s' is missing a non-negative register number", text);
            return false;
        }

        component = 0;
        if (text[i] == '.')
        {
            ++i;
            char c = text[i];
            bool validComponent =
                (c == 'x' || c == 'y' || c == 'z' || c == 'w') && text[i + 1] == 0;
            if (c == 'x') component = 0;
            else if (c == 'y') component = 1;
            else if (c == 'z') component = 2;
            else if (c == 'w') component = 3;

            if (!validComponent)
            {
                Printf(error,
                       "packoffset '%s' has an illegal component (expected a single .x/.y/.z/.w)",
                       text);
                return false;
            }
            ++i;
        }

        if (text[i] != 0)
        {
            Printf(error, "packoffset '%s' is invalid (expected cN or cN.x/y/z/w)", text);
            return false;
        }

        unsigned long long start = reg * 4ull + static_cast<unsigned long long>(component);
        if (start + 4ull > static_cast<unsigned long long>(MaxFloatOffset))
        {
            Printf(error, "packoffset register in '%s' is out of range", text);
            return false;
        }

        registerNumber = static_cast<int>(reg);
        return true;
    }

    static std::string FormatOffset(unsigned int floatOffset)
    {
        char buffer[64];
        String_Printf(buffer, sizeof(buffer), "c%u.%c", floatOffset / 4u,
                      "xyzw"[floatOffset % 4u]);
        return std::string(buffer);
    }

    bool Build(const HLSLBuffer* buffer, HLSLTree* tree)
    {
        m_valid       = true;
        m_hasExplicit = false;
        m_occupiedEnd = 0;
        m_error.clear();
        m_fields.clear();

        if (buffer == NULL)
        {
            return Fail("CBufferLayout::Build called with a NULL buffer");
        }

        // Validate every explicit annotation and detect whether the buffer
        // contains any packoffset at all.
        for (HLSLDeclaration* field = buffer->field; field != NULL;
             field = static_cast<HLSLDeclaration*>(field->nextStatement))
        {
            if (!field->hasPackOffset)
            {
                continue;
            }

            m_hasExplicit = true;

            int reg = 0;
            int comp = 0;
            std::string parseError;
            if (!ParsePackOffsetText(field->registerName, reg, comp, parseError))
            {
                return Fail("cbuffer field '%s': %s",
                            field->name != NULL ? field->name : "?", parseError.c_str());
            }
        }

        const Rules rules = m_hasExplicit ? Rules_Complete : Rules_Legacy;

        unsigned int cursor = 0;

        for (HLSLDeclaration* declaration = buffer->field; declaration != NULL;
             declaration = static_cast<HLSLDeclaration*>(declaration->nextStatement))
        {
            Field layoutField;
            layoutField.declaration = declaration;
            layoutField.name = declaration->name != NULL ? declaration->name : "?";

            const HLSLType& type = declaration->type;

            unsigned int elementSize = 0;
            if (type.array)
            {
                int count = 0;
                unsigned int stride = 0;
                std::string infoError;
                if (!GetArrayInfo(tree, rules, type, count, elementSize, stride, infoError))
                {
                    return Fail("cbuffer field '%s': %s", layoutField.name.c_str(),
                                infoError.c_str());
                }
                layoutField.arrayCount  = static_cast<unsigned int>(count);
                layoutField.arrayStride = stride;
            }
            else
            {
                std::string measureError;
                if (!MeasureElementType(tree, rules, type, elementSize, measureError))
                {
                    return Fail("cbuffer field '%s': %s", layoutField.name.c_str(),
                                measureError.c_str());
                }
            }

            unsigned int extent = 0;
            if (type.array)
            {
                extent = layoutField.arrayStride * layoutField.arrayCount;
            }
            else
            {
                extent = elementSize;
            }

            unsigned int start = 0;

            if (declaration->hasPackOffset)
            {
                int explicitRegister = 0;
                int explicitComponent = 0;
                std::string parseError;
                if (!ParsePackOffsetText(declaration->registerName, explicitRegister,
                                         explicitComponent, parseError))
                {
                    return Fail("cbuffer field '%s': %s", layoutField.name.c_str(),
                                parseError.c_str());
                }

                layoutField.explicitOffset = true;
                layoutField.packRegister   = explicitRegister;
                layoutField.packComponent  = explicitComponent;

                unsigned long long explicitStart =
                    static_cast<unsigned long long>(explicitRegister) * 4ull +
                    static_cast<unsigned long long>(explicitComponent);

                if (explicitStart >= static_cast<unsigned long long>(MaxFloatOffset))
                {
                    return Fail("cbuffer field '%s': packoffset %s is out of range",
                                layoutField.name.c_str(),
                                FormatOffset(static_cast<unsigned int>(explicitStart)).c_str());
                }
                start = static_cast<unsigned int>(explicitStart);

                const bool aggregateLike =
                    type.array ||
                    type.baseType == HLSLBaseType_Float4x4 ||
                    type.baseType == HLSLBaseType_UserDefined;

                if (aggregateLike && explicitComponent != 0)
                {
                    return Fail("cbuffer field '%s': arrays, structures and matrices can only be "
                                "placed at the start of a register, but packoffset is %s",
                                layoutField.name.c_str(), FormatOffset(start).c_str());
                }

                if (!aggregateLike)
                {
                    unsigned int valueSize = elementSize;
                    if (start / 4u != (start + valueSize - 1u) / 4u)
                    {
                        return Fail("cbuffer field '%s': %s (%u float%s) crosses a register boundary",
                                    layoutField.name.c_str(), FormatOffset(start).c_str(),
                                    valueSize, valueSize == 1 ? "" : "s");
                    }
                }
            }
            else
            {
                // Automatic placement continues from the high-water cursor
                // using the original alignment rules.
                start = AlignOffsetForType(type, cursor);
            }

            if (extent == 0 ||
                static_cast<unsigned long long>(start) + extent >
                    static_cast<unsigned long long>(MaxFloatOffset))
            {
                if (extent == 0)
                {
                    return Fail("cbuffer field '%s': has zero reserved size",
                                layoutField.name.c_str());
                }
                return Fail("cbuffer field '%s': layout extends past the supported bound",
                            layoutField.name.c_str());
            }

            layoutField.offset = start;
            layoutField.size   = extent;

            const unsigned int end = start + extent;

            // Occupancy conflict against every previously placed field; the
            // reserved aggregate padding is part of [offset, end).
            for (size_t i = 0; i < m_fields.size(); ++i)
            {
                const Field& other = m_fields[i];
                if (start < other.GetEnd() && other.offset < end)
                {
                    return Fail("packoffset conflict between cbuffer fields '%s' "
                                "(%s, float range [%u,%u)) and '%s' (%s, float range [%u,%u))",
                                layoutField.name.c_str(), FormatOffset(start).c_str(), start, end,
                                other.name.c_str(), FormatOffset(other.offset).c_str(),
                                other.offset, other.GetEnd());
                }
            }

            m_fields.push_back(layoutField);

            // Explicit fields never pull the cursor backwards.
            if (end > cursor)
            {
                cursor = end;
            }
            if (end > m_occupiedEnd)
            {
                m_occupiedEnd = end;
            }
        }

        m_valid = true;
        return true;
    }

private:

    bool Fail(const char* format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start(args, format);
        String_PrintfArgList(buffer, sizeof(buffer), format, args);
        va_end(args);
        m_error = buffer;
        m_valid = false;
        return false;
    }

    bool               m_valid;
    bool               m_hasExplicit;
    unsigned int       m_occupiedEnd;
    std::string        m_error;
    std::vector<Field> m_fields;
};

}

#endif
