#ifndef MATRIX_TARGET_H
#define MATRIX_TARGET_H

#include "HLSLTree.h"

namespace M4
{

/**
 * Returns the shape of one of the square matrix types (2x2, 3x3, 4x4) in
 * rows/columns.  Returns false for anything else.
 */
bool GetMatrixTypeShape(HLSLBaseType baseType, int& rows, int& columns);

/**
 * Returns the bounds used by the historical parser code for matrix component
 * selectors (the first digit is bounded by the type description's height and
 * the second by its numComponents).  This also accepts the asymmetric matrix
 * types so their existing row/column correspondence does not change.
 */
bool GetMatrixSelectorBounds(HLSLBaseType baseType, int& firstDigitMax, int& secondDigitMax);

/**
 * Returns true when the character is one of the vector swizzle letters.
 * The returned component index is 0 based.
 */
bool GetSwizzleComponentIndex(char c, int& index);

/**
 * Decodes a plain vector swizzle such as "xyzw", "rgba" or "yx".  Returns
 * false for anything that is not a valid sequence of one to four swizzle
 * letters (mixing the xyzw and rgba alphabets is rejected).
 */
bool DecodeSwizzleField(const char* field, int indices[4], int& count);

/**
 * Decodes an HLSL matrix component selector such as "_m00_m12" (zero based)
 * or "_11_23" (one based).  Each selected component is stored as a row and
 * column pair in the order it was written.  Returns false for anything that
 * is not a selector, mixes the two numbering schemes, is out of bounds or
 * selects more than four components.
 */
bool DecodeMatrixSelectorField(const char* field, int rows, int columns,
                               int row[4], int column[4], int& count);

/**
 * A matrix component selection/build target, normalised into the form used
 * by both the parser (for validation) and the GLSL generator.
 *
 * There are two shapes of target:
 *  - a fixed selection: every selected component has a static row and
 *    column.  This is what matrix selectors such as _m00_m12 and swizzles
 *    of them produce.
 *  - a row selection: the row is an arbitrary expression and every selected
 *    component names a (possibly dynamic) column.  This covers m[r],
 *    m[r].xy and m[r][c].
 */
struct HLSLMatrixTarget
{
    HLSLMatrixTarget()
    {
        base = NULL;
        rows = 0;
        columns = 0;
        dynamicRow = false;
        rowExpression = NULL;
        dynamicColumn = false;
        columnExpression = NULL;
        count = 0;
        rhs = NULL;
        for (int i = 0; i < 4; ++i)
        {
            row[i] = 0;
            column[i] = 0;
        }
    }

    HLSLExpression* base;
    int             rows;
    int             columns;
    bool            dynamicRow;
    HLSLExpression* rowExpression;

    bool            dynamicColumn;
    HLSLExpression* columnExpression;

    int             count;
    int             row[4];
    int             column[4];

    // Only used while generating an assignment.
    HLSLExpression* rhs;
};

/**
 * Attempts to interpret expression as a matrix row or matrix element
 * selection target.  Returns false for ordinary expressions (plain
 * variables, struct fields, vector swizzles that are not rooted at a matrix,
 * etc).  The tree is used to evaluate constant indices; it may be NULL.
 */
bool GetHLSLMatrixTarget(HLSLTree* tree, HLSLExpression* expression, HLSLMatrixTarget& target);

/** True if every selected matrix component is distinct. */
bool MatrixTargetHasUniqueComponents(const HLSLMatrixTarget& target);

} // namespace M4

#endif
