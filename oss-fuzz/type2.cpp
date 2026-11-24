/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2024 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "type2.h"
#include "structured.h"

#include <cstring>
#include <utility>

namespace {

/* All decode state belongs to one buildTree() call.  The original generator
 * kept it in function-local statics, which made a second call (or a call for
 * a different seed) continue where the previous one stopped. */
struct Decoder {
    const uint8_t *data;
    size_t dataSize;
    size_t pos = 0;      // current "data" position
    int dataValue = 0;   // value extracted from data
    int ones = 0;        // tracks if more bits must be shifted in

    Decoder(const uint8_t *d, size_t size) : data(d), dataSize(size) {}

    int getValue(uint8_t maxValue, bool *done = nullptr)
    {
        // Shift more bits from "data" into "dataValue" if needed
        while (pos < dataSize && ones < 0xFFFF) {
            ones = (ones << 8) | 0xff;
            dataValue = (dataValue << 8) | data[pos];
            pos++;
        }

        if (done)
            *done = (ones == 0);

        if (maxValue == 0)
            return 0;

        // Shift out info from "dataValue" using % . Using & and >> would work but
        // then we are limited to "power of 2" max value.
        const int ret = dataValue % maxValue;
        ones /= maxValue;
        dataValue /= maxValue;
        return ret;
    }
};

std::unique_ptr<structured::Expr> buildLvalue(Decoder &dec)
{
    auto e = std::make_unique<structured::Expr>(structured::ExprKind::Lvalue);
    e->varIndex = 1 + dec.getValue(5);
    return e;
}

std::unique_ptr<structured::Expr> buildOp(Decoder &dec, int numberOfGlobalConstants)
{
    auto e = std::make_unique<structured::Expr>(structured::ExprKind::Literal);
    switch (dec.getValue(3)) {
    case 0:
        e->kind = structured::ExprKind::Lvalue;
        e->varIndex = 1 + dec.getValue(5);
        break;
    case 1:
        e->kind = structured::ExprKind::GlobalConstant;
        e->globalIndex = 1 + dec.getValue(static_cast<uint8_t>(numberOfGlobalConstants));
        break;
    case 2:
        // The original appended this int as a character.  The tree keeps the
        // value and the renderer emits it as a decimal number.
        e->kind = structured::ExprKind::Literal;
        e->literal = static_cast<unsigned>(dec.getValue(0x80) * 0x80 + dec.getValue(0x80));
        break;
    }
    return e;
}

std::unique_ptr<structured::Expr> buildExpr(Decoder &dec, int numberOfGlobalConstants, int depth = 0)
{
    ++depth;
    const int type = (depth > 3) ? 0 : dec.getValue(3);
    const char binop[] = "=<>+-*/%&|^";
    const char *unop[] = {"++", "--", "()", "~"};

    switch (type) {
    case 0:
        return buildOp(dec, numberOfGlobalConstants);
    case 1: {
        const char op = binop[dec.getValue(sizeof(binop) - 1)];
        auto e = std::make_unique<structured::Expr>(
                     op == '=' ? structured::ExprKind::Assign : structured::ExprKind::Binary);
        e->op = op;
        e->lhs = (op == '=') ? buildLvalue(dec)
                             : buildExpr(dec, numberOfGlobalConstants, depth);
        e->rhs = buildExpr(dec, numberOfGlobalConstants, depth);
        // Assign built below depth 1 is parenthesized by the generator.
        e->wrapParen = (depth > 1 && op == '=');
        return e;
    }
    case 2: {
        const char *u = unop[dec.getValue(sizeof(unop) / sizeof(*unop))];
        if (strcmp(u, "()") == 0) {
            auto e = std::make_unique<structured::Expr>(structured::ExprKind::Paren);
            e->lhs = buildExpr(dec, numberOfGlobalConstants, depth);
            return e;
        }
        auto e = std::make_unique<structured::Expr>(structured::ExprKind::Unary);
        if (strcmp(u, "++") == 0 || strcmp(u, "--") == 0) {
            e->unaryOp = u;
            e->lhs = buildLvalue(dec);
        } else {
            e->unaryOp = u;
            e->lhs = buildExpr(dec, numberOfGlobalConstants, depth);
        }
        return e;
    }
    default:
        break;
    }

    return std::make_unique<structured::Expr>(structured::ExprKind::Literal);
}

std::unique_ptr<structured::Expr> buildCond(Decoder &dec, int numberOfGlobalConstants)
{
    const char *comp[] = {"==", "!=", "<", "<=", ">", ">="};
    const int i = dec.getValue(6);
    auto e = std::make_unique<structured::Expr>(structured::ExprKind::Compare);
    e->compOp = comp[i];
    e->lhs = buildExpr(dec, numberOfGlobalConstants);
    e->rhs = buildExpr(dec, numberOfGlobalConstants);
    return e;
}

std::unique_ptr<structured::Block> buildBlock(Decoder &dec, size_t indentSize, int numberOfGlobalConstants)
{
    auto block = std::make_unique<structured::Block>();

    for (int line = 0; line < 4; ++line) {
        bool done = false;
        const int type1 = dec.getValue(8, &done);
        if (done)
            break;

        const int mostLikelyType = (line >= 2) ? 4 : 0;
        const int type2 = (indentSize >= 12) ?
                          mostLikelyType :
                          ((type1 >= 5) ? mostLikelyType : type1);

        if (type2 == 0) {
            auto s = std::make_unique<structured::Stmt>(structured::StmtKind::Assign);
            s->varIndex = 1 + dec.getValue(5);
            s->value = buildExpr(dec, numberOfGlobalConstants);
            block->stmts.push_back(std::move(s));
        } else if (type2 == 1 || type2 == 2 || type2 == 3) {
            const structured::StmtKind kind =
                (type2 == 1) ? structured::StmtKind::If :
                (type2 == 2) ? structured::StmtKind::IfElse :
                structured::StmtKind::While;
            auto s = std::make_unique<structured::Stmt>(kind);
            s->value = buildCond(dec, numberOfGlobalConstants);
            s->thenBlock = buildBlock(dec, indentSize + 4, numberOfGlobalConstants);
            if (type2 == 2)
                s->elseBlock = buildBlock(dec, indentSize + 4, numberOfGlobalConstants);
            block->stmts.push_back(std::move(s));
        } else if (type2 == 4) {
            auto s = std::make_unique<structured::Stmt>(structured::StmtKind::Return);
            s->value = buildExpr(dec, numberOfGlobalConstants);
            block->stmts.push_back(std::move(s));
            break;
        }
    }

    return block;
}

void collectRefs(const structured::Expr &e, bool decls[6])
{
    if (e.kind == structured::ExprKind::Lvalue && e.varIndex >= 1 && e.varIndex <= 5)
        decls[e.varIndex] = true;
    if (e.lhs)
        collectRefs(*e.lhs, decls);
    if (e.rhs)
        collectRefs(*e.rhs, decls);
}

void collectRefs(const structured::Block &block, bool decls[6]);

void collectRefs(const structured::Stmt &s, bool decls[6])
{
    if (s.kind == structured::StmtKind::Assign && s.varIndex >= 1 && s.varIndex <= 5)
        decls[s.varIndex] = true;
    if (s.value)
        collectRefs(*s.value, decls);
    if (s.thenBlock)
        collectRefs(*s.thenBlock, decls);
    if (s.elseBlock)
        collectRefs(*s.elseBlock, decls);
}

void collectRefs(const structured::Block &block, bool decls[6])
{
    for (const auto &s : block.stmts)
        collectRefs(*s, decls);
}

void renderExpr(const structured::Expr &e, std::string &out)
{
    switch (e.kind) {
    case structured::ExprKind::Lvalue:
        out += "var";
        out += std::to_string(e.varIndex);
        break;
    case structured::ExprKind::GlobalConstant:
        out += "globalconstant";
        out += std::to_string(e.globalIndex);
        break;
    case structured::ExprKind::Literal:
        out += std::to_string(e.literal);
        break;
    case structured::ExprKind::Binary:
        renderExpr(*e.lhs, out);
        out += e.op;
        renderExpr(*e.rhs, out);
        break;
    case structured::ExprKind::Assign:
        if (e.wrapParen)
            out += '(';
        renderExpr(*e.lhs, out);
        out += '=';
        renderExpr(*e.rhs, out);
        if (e.wrapParen)
            out += ')';
        break;
    case structured::ExprKind::Unary:
        out += e.unaryOp;
        renderExpr(*e.lhs, out);
        break;
    case structured::ExprKind::Paren:
        out += '(';
        renderExpr(*e.lhs, out);
        out += ')';
        break;
    case structured::ExprKind::Compare:
        renderExpr(*e.lhs, out);
        out += e.compOp;
        renderExpr(*e.rhs, out);
        break;
    }
}

void renderStmt(const structured::Stmt &s, const std::string &indent, std::string &out);

void renderBlock(const structured::Block &block, const std::string &indent, std::string &out)
{
    out += indent;
    out += "{\n";
    for (const auto &s : block.stmts)
        renderStmt(*s, indent, out);
    out += indent;
    out += "}\n";
}

void renderStmt(const structured::Stmt &s, const std::string &indent, std::string &out)
{
    const std::string inner = indent + "    ";
    switch (s.kind) {
    case structured::StmtKind::Assign:
        out += inner;
        out += "var";
        out += std::to_string(s.varIndex);
        out += '=';
        renderExpr(*s.value, out);
        out += ";\n";
        break;
    case structured::StmtKind::Return:
        out += inner;
        out += "return ";
        renderExpr(*s.value, out);
        out += ";\n";
        break;
    case structured::StmtKind::If:
        out += inner;
        out += "if (";
        renderExpr(*s.value, out);
        out += ")\n";
        renderBlock(*s.thenBlock, inner, out);
        break;
    case structured::StmtKind::IfElse:
        out += inner;
        out += "if (";
        renderExpr(*s.value, out);
        out += ")\n";
        renderBlock(*s.thenBlock, inner, out);
        out += inner;
        out += "else\n";
        renderBlock(*s.elseBlock, inner, out);
        break;
    case structured::StmtKind::While:
        out += inner;
        out += "while (";
        renderExpr(*s.value, out);
        out += ")\n";
        renderBlock(*s.thenBlock, inner, out);
        break;
    }
}

} // namespace

namespace structured {

Tree buildTree(const uint8_t *data, size_t size)
{
    Tree tree;
    tree.decls.clear();
    const long long initialValues[5] = {1, 0, 1, 0, -1};
    for (int i = 0; i < 5; ++i) {
        Decl d;
        d.present = true;
        d.varIndex = i + 1;
        d.initial = initialValues[i];
        tree.decls.push_back(d);
    }

    Decoder dec(data, size);

    // create global constants
    constexpr int numberOfGlobalConstants = 0;
    /*
       const int numberOfGlobalConstants = dec.getValue(5);
       for (int nr = 1; nr <= numberOfGlobalConstants; nr++) {
        const char *types[4] = {"char", "int", "long long", "float"};
        code << "const " << types[dec.getValue(4)] << " globalconstant" << nr << " = " << generateExpression2_Expr(dec, nr - 1) << ";\n";
       }
     */

    // Every function is terminated by a return; the top level loop below
    // appends the implicit "return 0;" of the function it is filling.
    auto current = std::make_unique<Function>();
    current->body = std::make_unique<Block>();

    for (int line = 0;; ++line) {
        bool done = false;
        const int type1 = dec.getValue(8, &done);
        if (done)
            break;

        const int mostLikelyType = (line >= 2) ? 4 : 0;
        const int type2 = (type1 >= 5) ? mostLikelyType : type1;

        if (type2 == 0) {
            auto s = std::make_unique<Stmt>(StmtKind::Assign);
            s->varIndex = 1 + dec.getValue(5);
            s->value = buildExpr(dec, numberOfGlobalConstants);
            current->body->stmts.push_back(std::move(s));
        } else if (type2 == 1 || type2 == 2 || type2 == 3) {
            const StmtKind kind =
                (type2 == 1) ? StmtKind::If :
                (type2 == 2) ? StmtKind::IfElse :
                StmtKind::While;
            auto s = std::make_unique<Stmt>(kind);
            s->value = buildCond(dec, numberOfGlobalConstants);
            s->thenBlock = buildBlock(dec, 4, numberOfGlobalConstants);
            if (type2 == 2)
                s->elseBlock = buildBlock(dec, 4, numberOfGlobalConstants);
            current->body->stmts.push_back(std::move(s));
        } else if (type2 == 4) {
            auto s = std::make_unique<Stmt>(StmtKind::Return);
            s->value = buildExpr(dec, numberOfGlobalConstants);
            current->body->stmts.push_back(std::move(s));
            tree.functions.push_back(std::move(current));
            current = std::make_unique<Function>();
            current->body = std::make_unique<Block>();
        }
    }

    {
        auto s = std::make_unique<Stmt>(StmtKind::Return);
        auto zero = std::make_unique<Expr>(ExprKind::Literal);
        zero->literal = 0;
        s->value = std::move(zero);
        current->body->stmts.push_back(std::move(s));
        tree.functions.push_back(std::move(current));
    }

    return tree;
}

std::string renderTree(const Tree &tree)
{
    std::string out;

    bool anyDecl = false;
    for (const auto &d : tree.decls) {
        if (!d.present)
            continue;
        anyDecl = true;
        out += "int var";
        out += std::to_string(d.varIndex);
        out += " = ";
        out += std::to_string(d.initial);
        out += ";\n";
    }
    if (anyDecl)
        out += '\n';

    for (size_t i = 0; i < tree.functions.size(); ++i) {
        if (i)
            out += '\n';
        out += "int f";
        out += std::to_string(i + 1);
        out += "()\n{\n";
        for (const auto &s : tree.functions[i]->body->stmts)
            renderStmt(*s, "", out);
        out += "}\n";
    }

    return out;
}

bool declReferenced(const Tree &tree, int varIndex)
{
    bool refs[6] = {false, false, false, false, false, false};
    for (const auto &f : tree.functions)
        collectRefs(*f->body, refs);
    return varIndex >= 1 && varIndex <= 5 && refs[varIndex];
}

} // namespace structured

std::string generateCode2(const uint8_t *data, size_t dataSize)
{
    return structured::renderTree(structured::buildTree(data, dataSize));
}
