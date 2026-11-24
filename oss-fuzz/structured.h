/* -*- C++ -*-
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/*
 * A small, typed model of the source that oss-fuzz/type2.cpp produces.
 *
 * The generator decodes its input while it builds this tree.  Keeping the
 * tree around instead of immediately flattening it to text is what makes it
 * possible to remove individual statements / expressions without disturbing
 * any of the decode choices that follow them.
 */
namespace structured {

enum class ExprKind {
    Lvalue,          // var1 .. var5
    GlobalConstant,  // globalconstantN
    Literal,         // decimal integer
    Binary,          // lhs op rhs
    Assign,          // lhs = rhs
    Unary,           // ++lvalue, --lvalue, ~expr
    Paren,           // (expr)
    Compare          // lhs compOp rhs (condition)
};

struct Expr {
    explicit Expr(ExprKind k) : kind(k) {}

    ExprKind kind;
    int varIndex = 0;           // Lvalue
    unsigned literal = 0;       // Literal
    int globalIndex = 0;        // GlobalConstant
    char op = 0;                // Binary
    std::string unaryOp;        // Unary: "++", "--", "~"
    std::string compOp;         // Compare: "==", "!=", "<", "<=", ">", ">="
    bool wrapParen = false;     // Assign built below depth 1 is parenthesized
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;
};

enum class StmtKind {
    Assign,   // varN = expr;
    Return,   // return expr;
    If,       // if (cond) block
    IfElse,   // if (cond) block else block
    While     // while (cond) block
};

struct Block;

struct Stmt {
    explicit Stmt(StmtKind k) : kind(k) {}

    StmtKind kind;
    int varIndex = 0;                  // Assign (left hand side)
    std::unique_ptr<Expr> value;       // Assign rhs / Return expr / condition
    std::unique_ptr<Block> thenBlock;  // If / IfElse / While
    std::unique_ptr<Block> elseBlock;  // IfElse
};

struct Block {
    std::vector<std::unique_ptr<Stmt>> stmts;
};

struct Decl {
    bool present = true;
    int varIndex = 0;
    long long initial = 0;
};

struct Function {
    std::unique_ptr<Block> body;
};

struct Tree {
    std::vector<Decl> decls;
    std::vector<std::unique_ptr<Function>> functions;
};

/* Build the typed tree from a seed.  The whole decode state (cursor, mixed
 * radix remainder and the number of the function being emitted) belongs to
 * this single call, so repeated calls and interleaved calls are independent. */
Tree buildTree(const uint8_t *data, size_t size);

/* Canonical text of the tree: decimal numbers, var1..var5 only and function
 * names assigned f1, f2, ... by the order the functions survive in. */
std::string renderTree(const Tree &tree);

/* Deep copy, used by the reducer to try one mutation at a time. */
Tree cloneTree(const Tree &tree);

/* True when any surviving expression reads/writes varIndex (including the
 * left hand side of an assignment).  A declaration may only be dropped when
 * this is false. */
bool declReferenced(const Tree &tree, int varIndex);

/* Implementation of the public entry point; the global reduceCode2() in
 * structured.cpp forwards here.  Declared so callers can use the namespaced
 * form as well. */
std::string reduceCode2Impl(const uint8_t *data,
                            size_t size,
                            const std::string &worker,
                            const std::vector<unsigned> &required,
                            size_t budget);

} // namespace structured

/*
 * Structured reduction of the code generated from `data`.
 *
 * Every round walks the tree in pre-order and tries, at each node, removing a
 * complete statement subtree, replacing a non-lvalue compound expression with
 * the integer 0, or deleting a global variable declaration that is no longer
 * referenced.  A candidate is accepted only when its canonical source is
 * strictly smaller and still contains every coverage id in `required`.
 * After every acceptance the walk restarts from the beginning.  Reduction
 * stops when a whole round makes no progress or when `budget` observations
 * have been spent.
 *
 * `worker` is a local executable.  It is started directly (never through a
 * shell) with the name of a temporary file holding the candidate source.  A
 * successful observation exits with status 0 and writes exactly one line:
 *
 *     COVER[ id ...]
 *
 * Throws std::runtime_error when the initial source does not carry all
 * required coverage, and when the worker cannot be started, fails, times out
 * after one second, or reports malformed output.
 */
std::string reduceCode2(const uint8_t *data,
                        size_t size,
                        const std::string &worker,
                        const std::vector<unsigned> &required,
                        size_t budget);
