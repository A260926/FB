//=============================================================================
//
// Render/MatrixTarget.cpp
//
// Helpers for understanding HLSL matrix rows and multi component selections.
//
//=============================================================================

#include "MatrixTarget.h"

#include <ctype.h>

namespace M4
{

bool GetMatrixTypeShape(HLSLBaseType baseType, int& rows, int& columns)
{
    switch (baseType)
    {
    case HLSLBaseType_Float2x2:
    case HLSLBaseType_Half2x2:
        rows = 2;
        columns = 2;
        return true;
    case HLSLBaseType_Float3x3:
    case HLSLBaseType_Half3x3:
        rows = 3;
        columns = 3;
        return true;
    case HLSLBaseType_Float4x4:
    case HLSLBaseType_Half4x4:
        rows = 4;
        columns = 4;
        return true;
    default:
        return false;
    }
}

bool GetMatrixSelectorBounds(HLSLBaseType baseType, int& firstDigitMax, int& secondDigitMax)
{
    // These bounds match the parser's historical BaseTypeDescription use
    // (first digit bounded by height, second by numComponents), so the
    // row/column correspondence of the asymmetric matrix types is unchanged.
    switch (baseType)
    {
    case HLSLBaseType_Float2x2:
    case HLSLBaseType_Half2x2:
        firstDigitMax = 2;
        secondDigitMax = 2;
        return true;
    case HLSLBaseType_Float3x3:
    case HLSLBaseType_Half3x3:
        firstDigitMax = 3;
        secondDigitMax = 3;
        return true;
    case HLSLBaseType_Float4x4:
    case HLSLBaseType_Half4x4:
        firstDigitMax = 4;
        secondDigitMax = 4;
        return true;
    case HLSLBaseType_Float4x3:
    case HLSLBaseType_Half4x3:
        firstDigitMax = 3;
        secondDigitMax = 4;
        return true;
    case HLSLBaseType_Float4x2:
    case HLSLBaseType_Half4x2:
        firstDigitMax = 2;
        secondDigitMax = 4;
        return true;
    default:
        return false;
    }
}

bool GetSwizzleComponentIndex(char c, int& index)
{
    switch (c)
    {
    case 'x': case 'r': index = 0; return true;
    case 'y': case 'g': index = 1; return true;
    case 'z': case 'b': index = 2; return true;
    case 'w': case 'a': index = 3; return true;
    default: return false;
    }
}

static bool IsRgbaAlphabet(char c)
{
    return c == 'r' || c == 'g' || c == 'b' || c == 'a';
}

bool DecodeSwizzleField(const char* field, int indices[4], int& count)
{
    count = 0;
    if (field == NULL || field[0] == 0)
    {
        return false;
    }

    int alphabet = -1;
    for (const char* n = field; *n != 0; ++n)
    {
        int index = 0;
        if (!GetSwizzleComponentIndex(*n, index))
        {
            return false;
        }
        int thisAlphabet = IsRgbaAlphabet(*n) ? 1 : 0;
        if (alphabet == -1)
        {
            alphabet = thisAlphabet;
        }
        else if (alphabet != thisAlphabet)
        {
            // GLSL does not allow mixing xyzw and rgba in one swizzle.
            return false;
        }
        if (count >= 4)
        {
            return false;
        }
        indices[count] = index;
        ++count;
    }

    return count >= 1 && count <= 4;
}

bool DecodeMatrixSelectorField(const char* field, int rows, int columns,
                               int row[4], int column[4], int& count)
{
    count = 0;
    if (field == NULL || field[0] != '_')
    {
        return false;
    }

    int numbering = -1;
    const char* n = field;
    while (*n != 0)
    {
        if (*n != '_')
        {
            return false;
        }
        ++n;

        int base = 1;
        if (*n == 'm')
        {
            base = 0;
            ++n;
        }
        if (!isdigit((unsigned char)n[0]) || !isdigit((unsigned char)n[1]))
        {
            return false;
        }
        if (numbering == -1)
        {
            numbering = base;
        }
        else if (numbering != base)
        {
            // Zero based and one based numbering can not be mixed.
            return false;
        }

        int r = (n[0] - '0') - base;
        int c = (n[1] - '0') - base;
        if (r < 0 || c < 0 || r >= rows || c >= columns)
        {
            return false;
        }
        if (count >= 4)
        {
            return false;
        }
        row[count] = r;
        column[count] = c;
        ++count;
        n += 2;
    }

    return count >= 1 && count <= 4;
}

namespace
{

bool IsMatrixExpression(HLSLExpression* expression)
{
    if (expression == NULL)
    {
        return false;
    }
    int rows = 0, columns = 0;
    return !expression->expressionType.array &&
        GetMatrixTypeShape(expression->expressionType.baseType, rows, columns);
}

bool GetConstantIndex(HLSLTree* tree, HLSLExpression* expression, int& value)
{
    if (expression == NULL)
    {
        return false;
    }
    if (tree != NULL)
    {
        return tree->GetExpressionValue(expression, value);
    }
    if (expression->nodeType == HLSLNodeType_LiteralExpression)
    {
        HLSLLiteralExpression* literal = static_cast<HLSLLiteralExpression*>(expression);
        if (literal->type == HLSLBaseType_Int || literal->type == HLSLBaseType_Bool)
        {
            value = literal->type == HLSLBaseType_Int ? literal->iValue : (int)literal->bValue;
            return true;
        }
    }
    return false;
}

bool IsFullRowInOrder(const HLSLMatrixTarget& target)
{
    if (target.dynamicColumn || target.count != target.columns)
    {
        return false;
    }
    for (int i = 0; i < target.count; ++i)
    {
        if (target.column[i] != i)
        {
            return false;
        }
        if (target.row[i] != target.row[0])
        {
            return false;
        }
    }
    return true;
}

bool DecomposeMatrixTarget(HLSLTree* tree, HLSLExpression* expression, HLSLMatrixTarget& target, int depth)
{
    if (expression == NULL || depth > 8)
    {
        return false;
    }

    if (expression->nodeType == HLSLNodeType_MemberAccess)
    {
        HLSLMemberAccess* memberAccess = static_cast<HLSLMemberAccess*>(expression);

        if (IsMatrixExpression(memberAccess->object))
        {
            int rows = 0, columns = 0;
            GetMatrixTypeShape(memberAccess->object->expressionType.baseType, rows, columns);

            int row[4];
            int column[4];
            int count = 0;
            if (!DecodeMatrixSelectorField(memberAccess->field, rows, columns, row, column, count))
            {
                return false;
            }

            HLSLMatrixTarget result;
            result.base = memberAccess->object;
            result.rows = rows;
            result.columns = columns;
            result.dynamicRow = false;
            result.dynamicColumn = false;
            result.count = count;
            for (int i = 0; i < count; ++i)
            {
                result.row[i] = row[i];
                result.column[i] = column[i];
            }
            target = result;
            return true;
        }

        HLSLMatrixTarget sub;
        if (!DecomposeMatrixTarget(tree, memberAccess->object, sub, depth + 1))
        {
            return false;
        }

        int indices[4];
        int count = 0;
        if (!DecodeSwizzleField(memberAccess->field, indices, count))
        {
            return false;
        }
        for (int i = 0; i < count; ++i)
        {
            if (indices[i] < 0 || indices[i] >= sub.count)
            {
                return false;
            }
        }

        HLSLMatrixTarget result;
        result.base = sub.base;
        result.rows = sub.rows;
        result.columns = sub.columns;
        result.dynamicRow = sub.dynamicRow;
        result.rowExpression = sub.rowExpression;
        result.dynamicColumn = sub.dynamicColumn;
        result.columnExpression = sub.columnExpression;
        result.count = count;
        for (int i = 0; i < count; ++i)
        {
            int source = indices[i];
            result.row[i] = sub.row[source];
            result.column[i] = sub.column[source];
        }
        target = result;
        return true;
    }

    if (expression->nodeType == HLSLNodeType_ArrayAccess)
    {
        HLSLArrayAccess* arrayAccess = static_cast<HLSLArrayAccess*>(expression);

        if (IsMatrixExpression(arrayAccess->array))
        {
            int rows = 0, columns = 0;
            GetMatrixTypeShape(arrayAccess->array->expressionType.baseType, rows, columns);

            HLSLMatrixTarget result;
            result.base = arrayAccess->array;
            result.rows = rows;
            result.columns = columns;
            result.dynamicColumn = false;
            result.columnExpression = NULL;
            result.count = columns;
            for (int i = 0; i < columns; ++i)
            {
                result.column[i] = i;
            }

            int row = 0;
            if (GetConstantIndex(tree, arrayAccess->index, row) && row >= 0 && row < rows)
            {
                result.dynamicRow = false;
                result.rowExpression = NULL;
                for (int i = 0; i < columns; ++i)
                {
                    result.row[i] = row;
                }
            }
            else
            {
                result.dynamicRow = true;
                result.rowExpression = arrayAccess->index;
                for (int i = 0; i < columns; ++i)
                {
                    result.row[i] = 0;
                }
            }
            target = result;
            return true;
        }

        HLSLMatrixTarget sub;
        if (!DecomposeMatrixTarget(tree, arrayAccess->array, sub, depth + 1))
        {
            return false;
        }

        int index = 0;
        if (GetConstantIndex(tree, arrayAccess->index, index))
        {
            if (index < 0 || index >= sub.count)
            {
                return false;
            }

            HLSLMatrixTarget result;
            result.base = sub.base;
            result.rows = sub.rows;
            result.columns = sub.columns;
            result.dynamicRow = sub.dynamicRow;
            result.rowExpression = sub.rowExpression;
            result.dynamicColumn = false;
            result.columnExpression = NULL;
            result.count = 1;
            result.row[0] = sub.row[index];
            result.column[0] = sub.column[index];
            target = result;
            return true;
        }

        if (sub.dynamicColumn)
        {
            // Can not dynamically index a single selected component.
            return false;
        }

        // A dynamic index selects a column of the current row selection.
        // This is only representable when the selection is a full row in
        // natural column order.
        if (!IsFullRowInOrder(sub))
        {
            return false;
        }

        HLSLMatrixTarget result;
        result.base = sub.base;
        result.rows = sub.rows;
        result.columns = sub.columns;
        result.dynamicRow = sub.dynamicRow;
        result.rowExpression = sub.rowExpression;
        result.dynamicColumn = true;
        result.columnExpression = arrayAccess->index;
        result.count = 1;
        result.row[0] = sub.row[0];
        result.column[0] = 0;
        target = result;
        return true;
    }

    return false;
}

} // namespace

bool GetHLSLMatrixTarget(HLSLTree* tree, HLSLExpression* expression, HLSLMatrixTarget& target)
{
    return DecomposeMatrixTarget(tree, expression, target, 0);
}

bool MatrixTargetHasUniqueComponents(const HLSLMatrixTarget& target)
{
    if (target.dynamicColumn)
    {
        return target.count == 1;
    }
    for (int i = 0; i < target.count; ++i)
    {
        for (int j = i + 1; j < target.count; ++j)
        {
            if (target.column[i] != target.column[j])
            {
                continue;
            }
            if (target.dynamicRow || target.row[i] == target.row[j])
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace M4
