//=============================================================================
//
// checks/glsl_exec.cpp
//
// A deliberately small interpreter for the subset of GLSL 140 produced by
// the matrix translation tests.  It is standard C++11 and needs no GPU or
// shader SDK.  Anything outside the supported subset is reported as an
// error rather than silently producing a wrong answer.
//
//=============================================================================

#include "glsl_exec.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace glsl
{

//-----------------------------------------------------------------------------
// Values
//-----------------------------------------------------------------------------

Value::Value()
{
    width = 1;
    isMatrix = false;
    size = 0;
    data[0] = data[1] = data[2] = data[3] = 0.0f;
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            columns[c][r] = 0.0f;
        }
    }
}

Value::Value(float x, float y, float z, float w)
{
    *this = Scalar(x);
    (void)y; (void)z; (void)w;
}

Value Value::Scalar(float x)
{
    Value v;
    v.width = 1;
    v.data[0] = x;
    return v;
}

Value Value::Vector(int width, const float* values)
{
    Value v;
    v.width = width;
    for (int i = 0; i < 4; ++i)
    {
        v.data[i] = i < width ? values[i] : 0.0f;
    }
    return v;
}

Value Value::Matrix(int size)
{
    Value v;
    v.width = 1;
    v.isMatrix = true;
    v.size = size;
    return v;
}

//-----------------------------------------------------------------------------
// Type and object model
//-----------------------------------------------------------------------------

struct TypeInfo;
typedef std::shared_ptr<TypeInfo> TypePtr;

struct TypeInfo
{
    enum Kind
    {
        Kind_Void,
        Kind_Scalar,
        Kind_Vector,
        Kind_Matrix,
        Kind_Struct,
        Kind_Array,
        Kind_Unknown
    };

    TypeInfo()
    {
        kind = Kind_Unknown;
        width = 0;
        scalarKind = Kind_Scalar;
        isInt = false;
        isBool = false;
        arraySize = 0;
    }

    Kind        kind;
    int         width;          // vector width or matrix dimension
    Kind        scalarKind;     // Kind_Scalar for numeric scalars
    bool        isInt;
    bool        isBool;
    std::string name;           // struct name
    TypePtr     element;        // array element type
    int         arraySize;
};

static TypePtr MakeScalarType(bool isInt = false, bool isBool = false)
{
    TypePtr t(new TypeInfo());
    t->kind = TypeInfo::Kind_Scalar;
    t->isInt = isInt;
    t->isBool = isBool;
    return t;
}

static TypePtr MakeVectorType(int width, bool isInt = false, bool isBool = false)
{
    TypePtr t(new TypeInfo());
    t->kind = TypeInfo::Kind_Vector;
    t->width = width;
    t->isInt = isInt;
    t->isBool = isBool;
    return t;
}

static TypePtr MakeMatrixType(int size)
{
    TypePtr t(new TypeInfo());
    t->kind = TypeInfo::Kind_Matrix;
    t->width = size;
    return t;
}

static TypePtr MakeVoidType()
{
    TypePtr t(new TypeInfo());
    t->kind = TypeInfo::Kind_Void;
    return t;
}

struct Obj;
typedef std::shared_ptr<Obj> ObjPtr;

struct Obj
{
    Obj()
    {
        f = 0.0f;
        v[0] = v[1] = v[2] = v[3] = 0.0f;
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                m[c][r] = 0.0f;
            }
        }
    }

    TypePtr type;
    float   f;
    float   v[4];
    float   m[4][4];
    std::map<std::string, ObjPtr> fields;
    std::vector<ObjPtr>           elems;
};

static ObjPtr Clone(const ObjPtr& src);

static void CopyContents(ObjPtr dst, const ObjPtr& src)
{
    dst->type = src->type;
    dst->f = src->f;
    for (int i = 0; i < 4; ++i)
    {
        dst->v[i] = src->v[i];
    }
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            dst->m[c][r] = src->m[c][r];
        }
    }
    dst->fields.clear();
    for (std::map<std::string, ObjPtr>::const_iterator it = src->fields.begin(); it != src->fields.end(); ++it)
    {
        dst->fields[it->first] = Clone(it->second);
    }
    dst->elems.clear();
    for (size_t i = 0; i < src->elems.size(); ++i)
    {
        dst->elems.push_back(Clone(src->elems[i]));
    }
}

static ObjPtr Clone(const ObjPtr& src)
{
    if (!src)
    {
        return ObjPtr();
    }
    ObjPtr result(new Obj());
    CopyContents(result, src);
    return result;
}

static ObjPtr MakeScalar(float value)
{
    ObjPtr o(new Obj());
    o->type = MakeScalarType();
    o->f = value;
    return o;
}

static ObjPtr MakeInt(int value)
{
    ObjPtr o(new Obj());
    o->type = MakeScalarType(true);
    o->f = (float)value;
    return o;
}

static ObjPtr MakeBool(bool value)
{
    ObjPtr o(new Obj());
    o->type = MakeScalarType(false, true);
    o->f = value ? 1.0f : 0.0f;
    return o;
}

static bool ToBool(const ObjPtr& value)
{
    if (value->type && value->type->kind == TypeInfo::Kind_Vector)
    {
        return value->v[0] != 0.0f;
    }
    return value->f != 0.0f;
}

static int ToInt(const ObjPtr& value)
{
    return (int)value->f;
}

//-----------------------------------------------------------------------------
// Lexer
//-----------------------------------------------------------------------------

namespace
{

enum TokenKind
{
    Tok_End,
    Tok_Identifier,
    Tok_Number,
    Tok_Punct
};

struct Token
{
    TokenKind   kind;
    std::string text;
    float       number;
};

struct Lexer
{
    std::string source;
    size_t      pos;
    std::vector<Token> tokens;

    explicit Lexer(const std::string& text) : source(text), pos(0) {}

    void Run()
    {
        while (pos < source.size())
        {
            char c = source[pos];
            if (c == '\n' || c == ' ' || c == '\t' || c == '\r')
            {
                ++pos;
                continue;
            }
            if (c == '#')
            {
                while (pos < source.size() && source[pos] != '\n')
                {
                    ++pos;
                }
                continue;
            }
            if (c == '/' && pos + 1 < source.size() && source[pos + 1] == '/')
            {
                while (pos < source.size() && source[pos] != '\n')
                {
                    ++pos;
                }
                continue;
            }
            if (c == '/' && pos + 1 < source.size() && source[pos + 1] == '*')
            {
                pos += 2;
                while (pos + 1 < source.size() && !(source[pos] == '*' && source[pos + 1] == '/'))
                {
                    ++pos;
                }
                pos += 2;
                continue;
            }
            if (isalpha((unsigned char)c) || c == '_')
            {
                size_t start = pos;
                while (pos < source.size() &&
                       (isalnum((unsigned char)source[pos]) || source[pos] == '_'))
                {
                    ++pos;
                }
                Token t;
                t.kind = Tok_Identifier;
                t.text = source.substr(start, pos - start);
                t.number = 0.0f;
                tokens.push_back(t);
                continue;
            }
            if (isdigit((unsigned char)c) || (c == '.' && pos + 1 < source.size() && isdigit((unsigned char)source[pos + 1])))
            {
                size_t start = pos;
                bool isHex = false;
                if (c == '0' && pos + 1 < source.size() && (source[pos + 1] == 'x' || source[pos + 1] == 'X'))
                {
                    isHex = true;
                    pos += 2;
                    while (pos < source.size() && isxdigit((unsigned char)source[pos]))
                    {
                        ++pos;
                    }
                }
                else
                {
                    while (pos < source.size() && (isdigit((unsigned char)source[pos]) || source[pos] == '.' ||
                           source[pos] == 'e' || source[pos] == 'E' ||
                           ((source[pos] == '+' || source[pos] == '-') && pos > start && (source[pos - 1] == 'e' || source[pos - 1] == 'E'))))
                    {
                        ++pos;
                    }
                }
                std::string text = source.substr(start, pos - start);
                // Suffix.
                while (pos < source.size() && (source[pos] == 'f' || source[pos] == 'F' || source[pos] == 'u' || source[pos] == 'U' || source[pos] == 'h' || source[pos] == 'H'))
                {
                    ++pos;
                }
                Token t;
                t.kind = Tok_Number;
                t.text = text;
                t.number = isHex ? (float)strtol(text.c_str(), NULL, 16) : (float)atof(text.c_str());
                tokens.push_back(t);
                continue;
            }

            static const char* twoCharOps[] = { "&&", "||", "==", "!=", "<=", ">=", "++", "--", "+=", "-=", "*=", "/=", "<<", ">>" };
            bool matched = false;
            for (size_t i = 0; i < sizeof(twoCharOps) / sizeof(twoCharOps[0]); ++i)
            {
                if (source.compare(pos, 2, twoCharOps[i]) == 0)
                {
                    Token t;
                    t.kind = Tok_Punct;
                    t.text = twoCharOps[i];
                    t.number = 0.0f;
                    tokens.push_back(t);
                    pos += 2;
                    matched = true;
                    break;
                }
            }
            if (matched)
            {
                continue;
            }

            Token t;
            t.kind = Tok_Punct;
            t.text = std::string(1, c);
            t.number = 0.0f;
            tokens.push_back(t);
            ++pos;
        }

        Token end;
        end.kind = Tok_End;
        end.number = 0.0f;
        tokens.push_back(end);
    }
};

//-----------------------------------------------------------------------------
// AST
//-----------------------------------------------------------------------------

struct Expr;
typedef std::shared_ptr<Expr> ExprPtr;

struct Expr
{
    enum Kind
    {
        Literal,
        Ident,
        Unary,
        Postfix,
        Binary,
        Assign,
        Conditional,
        Call,
        Member,
        Index
    };

    Expr(Kind k) : kind(k), number(0.0f), isInt(false) {}

    Kind        kind;
    std::string text;       // identifier / operator / field / callee
    float       number;
    bool        isInt;
    ExprPtr     a;
    ExprPtr     b;
    ExprPtr     c;
    std::vector<ExprPtr> args;
};

struct Stmt;
typedef std::shared_ptr<Stmt> StmtPtr;

struct Stmt
{
    enum Kind
    {
        Block,
        Decl,
        ExprStmt,
        If,
        For,
        While,
        Return,
        Break,
        Continue,
        Empty
    };

    Stmt(Kind k) : kind(k), hasInit(false) {}

    Kind      kind;
    StmtPtr   a;
    StmtPtr   b;
    ExprPtr   expr;
    ExprPtr   cond;
    ExprPtr   incr;
    ExprPtr   init;
    TypePtr   type;
    std::string name;
    bool      hasInit;
    std::vector<StmtPtr> statements;
};

struct Param
{
    bool    isOut;
    bool    isInout;
    TypePtr type;
    std::string name;

    Param() : isOut(false), isInout(false) {}
};

struct FunctionDef
{
    std::string name;
    TypePtr     returnType;
    std::vector<Param> params;
    StmtPtr     body;
    bool        hasBody;
};

struct StructDef
{
    std::vector<std::pair<std::string, TypePtr> > fields;
};

//-----------------------------------------------------------------------------
// Parser
//-----------------------------------------------------------------------------

struct Program
{
    std::map<std::string, StructDef> structs;
    std::map<std::string, std::vector<FunctionDef> > functions;
    std::vector<std::pair<std::string, TypePtr> > globals;
    std::vector<std::pair<std::string, ExprPtr> > globalInits;
};

class Parser
{
public:
    Parser(const std::vector<Token>& tokens) : m_tokens(tokens), m_pos(0) {}

    void ParseProgram(Program& program)
    {
        while (!AtEnd())
        {
            if (Peek().kind == Tok_Punct && Peek().text == ";")
            {
                ++m_pos;
                continue;
            }
            if (Peek().kind == Tok_Identifier && Peek().text == "struct")
            {
                ParseStruct(program);
                continue;
            }
            ParseGlobalOrFunction(program);
        }
    }

private:

    const Token& Peek(int offset = 0) const
    {
        size_t index = m_pos + offset;
        if (index >= m_tokens.size())
        {
            index = m_tokens.size() - 1;
        }
        return m_tokens[index];
    }

    bool AtEnd() const { return Peek().kind == Tok_End; }

    bool Accept(const char* text)
    {
        if (Peek().kind == Tok_Punct && Peek().text == text)
        {
            ++m_pos;
            return true;
        }
        return false;
    }

    bool AcceptIdent(const char* text)
    {
        if (Peek().kind == Tok_Identifier && Peek().text == text)
        {
            ++m_pos;
            return true;
        }
        return false;
    }

    void Expect(const char* text)
    {
        if (!Accept(text))
        {
            std::ostringstream os;
            os << "expected '" << text << "' near '" << Peek().text << "'";
            throw std::runtime_error(os.str());
        }
    }

    std::string ExpectIdentifier()
    {
        if (Peek().kind != Tok_Identifier)
        {
            throw std::runtime_error("expected identifier near '" + Peek().text + "'");
        }
        std::string text = Peek().text;
        ++m_pos;
        return text;
    }

    bool IsTypeName(const std::string& name) const
    {
        if (name == "float" || name == "int" || name == "uint" || name == "bool" ||
            name == "void" || name == "vec2" || name == "vec3" || name == "vec4" ||
            name == "ivec2" || name == "ivec3" || name == "ivec4" ||
            name == "uvec2" || name == "uvec3" || name == "uvec4" ||
            name == "bvec2" || name == "bvec3" || name == "bvec4" ||
            name == "mat2" || name == "mat3" || name == "mat4" ||
            name == "mat2x2" || name == "mat3x3" || name == "mat4x4" ||
            name == "sampler2D" || name == "sampler3D" || name == "samplerCube")
        {
            return true;
        }
        return m_structs.find(name) != m_structs.end();
    }

    TypePtr MakeType(const std::string& name)
    {
        if (name == "float") return MakeScalarType();
        if (name == "int") return MakeScalarType(true, false);
        if (name == "uint") return MakeScalarType(true, false);
        if (name == "bool") return MakeScalarType(false, true);
        if (name == "void") return MakeVoidType();
        if (name == "vec2") return MakeVectorType(2);
        if (name == "vec3") return MakeVectorType(3);
        if (name == "vec4") return MakeVectorType(4);
        if (name == "ivec2") return MakeVectorType(2, true);
        if (name == "ivec3") return MakeVectorType(3, true);
        if (name == "ivec4") return MakeVectorType(4, true);
        if (name == "uvec2") return MakeVectorType(2, true);
        if (name == "uvec3") return MakeVectorType(3, true);
        if (name == "uvec4") return MakeVectorType(4, true);
        if (name == "bvec2") return MakeVectorType(2, false, true);
        if (name == "bvec3") return MakeVectorType(3, false, true);
        if (name == "bvec4") return MakeVectorType(4, false, true);
        if (name == "mat2" || name == "mat2x2") return MakeMatrixType(2);
        if (name == "mat3" || name == "mat3x3") return MakeMatrixType(3);
        if (name == "mat4" || name == "mat4x4") return MakeMatrixType(4);

        std::map<std::string, StructDef>::const_iterator it = m_structs.find(name);
        if (it != m_structs.end())
        {
            TypePtr t(new TypeInfo());
            t->kind = TypeInfo::Kind_Struct;
            t->name = name;
            return t;
        }
        // Unknown types (samplers etc) are treated as ints for our tests.
        return MakeScalarType();
    }

    TypePtr ParseType()
    {
        std::string name = ExpectIdentifier();
        TypePtr type = MakeType(name);

        while (Accept("["))
        {
            int size = 0;
            if (!Accept("]"))
            {
                size = (int)EvalConstExpression();
                Expect("]");
            }
            TypePtr array(new TypeInfo());
            array->kind = TypeInfo::Kind_Array;
            array->element = type;
            array->arraySize = size;
            type = array;
        }
        return type;
    }

    bool LooksLikeDeclaration()
    {
        // Called with the current token being a potential type name.
        if (Peek().kind != Tok_Identifier)
        {
            return false;
        }
        std::string name = Peek().text;
        if (!IsTypeName(name))
        {
            return false;
        }
        if (Peek(1).kind != Tok_Identifier)
        {
            return false;
        }
        return true;
    }

    void ParseStruct(Program& program)
    {
        AcceptIdent("struct");
        std::string name = ExpectIdentifier();
        Expect("{");
        StructDef def;
        while (!Accept("}"))
        {
            TypePtr type = ParseType();
            std::string field = ExpectIdentifier();
            while (Accept("["))
            {
                int size = 0;
                if (!Accept("]"))
                {
                    size = (int)EvalConstExpression();
                    Expect("]");
                }
                TypePtr array(new TypeInfo());
                array->kind = TypeInfo::Kind_Array;
                array->element = type;
                array->arraySize = size;
                type = array;
            }
            Accept(";");
            def.fields.push_back(std::make_pair(field, type));
        }
        Expect(";");
        program.structs[name] = def;
        m_structs[name] = def;
    }

    void ParseGlobalOrFunction(Program& program)
    {
        // Qualifiers.
        while (true)
        {
            if (AcceptIdent("in") || AcceptIdent("out") || AcceptIdent("inout") ||
                AcceptIdent("uniform") || AcceptIdent("const") || AcceptIdent("flat") ||
                AcceptIdent("centroid") || AcceptIdent("smooth") || AcceptIdent("noperspective"))
            {
                continue;
            }
            if (AcceptIdent("layout"))
            {
                Expect("(");
                int depth = 1;
                while (depth > 0)
                {
                    if (AtEnd()) throw std::runtime_error("unterminated layout");
                    if (Peek().text == "(") ++depth;
                    else if (Peek().text == ")") --depth;
                    ++m_pos;
                }
                continue;
            }
            break;
        }

        TypePtr type = ParseType();
        std::string name = ExpectIdentifier();

        if (Accept("("))
        {
            FunctionDef function;
            function.name = name;
            function.returnType = type;
            function.hasBody = false;

            while (!Accept(")"))
            {
                if (!function.params.empty())
                {
                    Expect(",");
                }
                Param param;
                if (AcceptIdent("out")) param.isOut = true;
                else if (AcceptIdent("inout")) { param.isOut = true; param.isInout = true; }
                else if (AcceptIdent("in")) {}
                else if (AcceptIdent("const")) {}
                param.type = ParseType();
                param.name = ExpectIdentifier();
                while (Accept("["))
                {
                    int size = 0;
                    if (!Accept("]"))
                    {
                        size = (int)EvalConstExpression();
                        Expect("]");
                    }
                    TypePtr array(new TypeInfo());
                    array->kind = TypeInfo::Kind_Array;
                    array->element = param.type;
                    array->arraySize = size;
                    param.type = array;
                }
                function.params.push_back(param);
            }

            if (Accept(";"))
            {
                // Forward declaration.
                function.hasBody = false;
            }
            else
            {
                function.body = ParseBlockStatement();
                function.hasBody = true;
            }

            std::vector<FunctionDef>& list = program.functions[name];
            // Prefer a definition over a forward declaration.
            bool replaced = false;
            for (size_t i = 0; i < list.size(); ++i)
            {
                if (list[i].params.size() == function.params.size() && !list[i].hasBody && function.hasBody)
                {
                    list[i] = function;
                    replaced = true;
                }
            }
            if (!replaced)
            {
                list.push_back(function);
            }
            return;
        }

        // Global declaration, possibly a comma separated list.
        while (true)
        {
            TypePtr declared = type;
            if (Accept("["))
            {
                int size = 0;
                if (!Accept("]"))
                {
                    size = (int)EvalConstExpression();
                    Expect("]");
                }
                TypePtr array(new TypeInfo());
                array->kind = TypeInfo::Kind_Array;
                array->element = declared;
                array->arraySize = size;
                declared = array;
            }

            ExprPtr init;
            if (Accept("="))
            {
                init = ParseExpression();
            }
            program.globals.push_back(std::make_pair(name, declared));
            program.globalInits.push_back(std::make_pair(name, init));

            if (!Accept(","))
            {
                break;
            }
            name = ExpectIdentifier();
        }
        Expect(";");
    }

    float EvalConstExpression()
    {
        // Only integer literal constants appear as array sizes in generated
        // code.
        bool negate = false;
        if (Accept("-"))
        {
            negate = true;
        }
        if (Peek().kind != Tok_Number)
        {
            throw std::runtime_error("expected constant expression");
        }
        float value = Peek().number;
        ++m_pos;
        return negate ? -value : value;
    }

    StmtPtr ParseBlockStatement()
    {
        Expect("{");
        StmtPtr block(new Stmt(Stmt::Block));
        while (!Accept("}"))
        {
            if (AtEnd()) throw std::runtime_error("unterminated block");
            block->statements.push_back(ParseStatement());
        }
        return block;
    }

    StmtPtr ParseStatement()
    {
        if (Peek().kind == Tok_Punct && Peek().text == "{")
        {
            return ParseBlockStatement();
        }
        if (Accept(";"))
        {
            return StmtPtr(new Stmt(Stmt::Empty));
        }
        if (AcceptIdent("if"))
        {
            StmtPtr stmt(new Stmt(Stmt::If));
            Expect("(");
            stmt->cond = ParseExpression();
            Expect(")");
            stmt->a = ParseStatement();
            if (AcceptIdent("else"))
            {
                stmt->b = ParseStatement();
            }
            return stmt;
        }
        if (AcceptIdent("for"))
        {
            StmtPtr stmt(new Stmt(Stmt::For));
            Expect("(");

            // Initializer: declaration or expression.
            if (Accept(";"))
            {
            }
            else if (LooksLikeDeclaration())
            {
                TypePtr type = ParseType();
                std::string name = ExpectIdentifier();
                stmt->type = type;
                stmt->name = name;
                if (Accept("="))
                {
                    stmt->init = ParseExpression();
                    stmt->hasInit = true;
                }
                Expect(";");
            }
            else
            {
                stmt->init = ParseExpression();
                Expect(";");
            }

            if (!Accept(";"))
            {
                stmt->cond = ParseExpression();
                Expect(";");
            }
            if (!Accept(")"))
            {
                stmt->incr = ParseExpression();
                Expect(")");
            }
            stmt->a = ParseStatement();
            return stmt;
        }
        if (AcceptIdent("while"))
        {
            StmtPtr stmt(new Stmt(Stmt::While));
            Expect("(");
            stmt->cond = ParseExpression();
            Expect(")");
            stmt->a = ParseStatement();
            return stmt;
        }
        if (AcceptIdent("return"))
        {
            StmtPtr stmt(new Stmt(Stmt::Return));
            if (!Accept(";"))
            {
                stmt->expr = ParseExpression();
                Expect(";");
            }
            return stmt;
        }
        if (AcceptIdent("break"))
        {
            Expect(";");
            return StmtPtr(new Stmt(Stmt::Break));
        }
        if (AcceptIdent("continue"))
        {
            Expect(";");
            return StmtPtr(new Stmt(Stmt::Continue));
        }
        if (AcceptIdent("discard"))
        {
            Expect(";");
            return StmtPtr(new Stmt(Stmt::Return));
        }

        if (LooksLikeDeclaration())
        {
            StmtPtr stmt(new Stmt(Stmt::Decl));
            stmt->type = ParseType();
            stmt->name = ExpectIdentifier();
            if (Accept("["))
            {
                int size = 0;
                if (!Accept("]"))
                {
                    size = (int)EvalConstExpression();
                    Expect("]");
                }
                TypePtr array(new TypeInfo());
                array->kind = TypeInfo::Kind_Array;
                array->element = stmt->type;
                array->arraySize = size;
                stmt->type = array;
            }
            if (Accept("="))
            {
                stmt->init = ParseExpression();
                stmt->hasInit = true;
            }
            Expect(";");
            return stmt;
        }

        StmtPtr stmt(new Stmt(Stmt::ExprStmt));
        stmt->expr = ParseExpression();
        Expect(";");
        return stmt;
    }

    ExprPtr ParseExpression()
    {
        ExprPtr expr = ParseAssignment();
        while (Accept(","))
        {
            ExprPtr next(new Expr(Expr::Binary));
            next->text = ",";
            next->a = expr;
            next->b = ParseAssignment();
            expr = next;
        }
        return expr;
    }

    ExprPtr ParseAssignment()
    {
        ExprPtr expr = ParseConditional();
        static const char* ops[] = { "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=" };
        for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i)
        {
            if (Peek().kind == Tok_Punct && Peek().text == ops[i])
            {
                ++m_pos;
                ExprPtr assign(new Expr(Expr::Assign));
                assign->text = ops[i];
                assign->a = expr;
                assign->b = ParseAssignment();
                return assign;
            }
        }
        return expr;
    }

    ExprPtr ParseConditional()
    {
        ExprPtr expr = ParseBinary(0);
        if (Accept("?"))
        {
            ExprPtr cond(new Expr(Expr::Conditional));
            cond->a = expr;
            cond->b = ParseAssignment();
            Expect(":");
            cond->c = ParseAssignment();
            return cond;
        }
        return expr;
    }

    static int Priority(const std::string& op)
    {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "|") return 3;
        if (op == "^") return 4;
        if (op == "&") return 5;
        if (op == "==" || op == "!=") return 6;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 7;
        if (op == "+" || op == "-") return 8;
        if (op == "*" || op == "/" || op == "%") return 9;
        return -1;
    }

    ExprPtr ParseBinary(int minPriority)
    {
        ExprPtr left = ParseUnary();
        while (true)
        {
            std::string op = Peek().kind == Tok_Punct ? Peek().text : std::string();
            int priority = Priority(op);
            if (priority < 0 || priority < minPriority)
            {
                break;
            }
            ++m_pos;
            ExprPtr right = ParseBinary(priority + 1);
            ExprPtr binary(new Expr(Expr::Binary));
            binary->text = op;
            binary->a = left;
            binary->b = right;
            left = binary;
        }
        return left;
    }

    ExprPtr ParseUnary()
    {
        if (Peek().kind == Tok_Punct &&
            (Peek().text == "-" || Peek().text == "+" || Peek().text == "!" || Peek().text == "~"))
        {
            std::string op = Peek().text;
            ++m_pos;
            ExprPtr unary(new Expr(Expr::Unary));
            unary->text = op;
            unary->a = ParseUnary();
            return unary;
        }
        if (Peek().kind == Tok_Punct && (Peek().text == "++" || Peek().text == "--"))
        {
            std::string op = Peek().text;
            ++m_pos;
            ExprPtr unary(new Expr(Expr::Unary));
            unary->text = op;
            unary->a = ParseUnary();
            return unary;
        }
        return ParsePostfix();
    }

    ExprPtr ParsePostfix()
    {
        ExprPtr expr = ParsePrimary();
        while (true)
        {
            if (Accept("."))
            {
                ExprPtr member(new Expr(Expr::Member));
                member->a = expr;
                member->text = ExpectIdentifier();
                expr = member;
            }
            else if (Accept("["))
            {
                ExprPtr index(new Expr(Expr::Index));
                index->a = expr;
                index->b = ParseExpression();
                Expect("]");
                expr = index;
            }
            else if (Accept("++"))
            {
                ExprPtr post(new Expr(Expr::Postfix));
                post->text = "++";
                post->a = expr;
                expr = post;
            }
            else if (Accept("--"))
            {
                ExprPtr post(new Expr(Expr::Postfix));
                post->text = "--";
                post->a = expr;
                expr = post;
            }
            else
            {
                break;
            }
        }
        return expr;
    }

    ExprPtr ParsePrimary()
    {
        if (Peek().kind == Tok_Number)
        {
            ExprPtr expr(new Expr(Expr::Literal));
            expr->number = Peek().number;
            expr->isInt = Peek().text.find('.') == std::string::npos &&
                          Peek().text.find('e') == std::string::npos &&
                          Peek().text.find('E') == std::string::npos;
            ++m_pos;
            return expr;
        }
        if (Accept("("))
        {
            ExprPtr expr = ParseExpression();
            Expect(")");
            return expr;
        }
        if (Peek().kind == Tok_Identifier)
        {
            std::string name = Peek().text;
            if (IsTypeName(name))
            {
                ++m_pos;
                ExprPtr call(new Expr(Expr::Call));
                call->text = name;
                Expect("(");
                while (!Accept(")"))
                {
                    if (!call->args.empty())
                    {
                        Expect(",");
                    }
                    call->args.push_back(ParseAssignment());
                }
                return call;
            }

            ++m_pos;
            if (Peek().kind == Tok_Punct && Peek().text == "(")
            {
                ++m_pos;
                ExprPtr call(new Expr(Expr::Call));
                call->text = name;
                while (!Accept(")"))
                {
                    if (!call->args.empty())
                    {
                        Expect(",");
                    }
                    call->args.push_back(ParseAssignment());
                }
                return call;
            }

            ExprPtr expr(new Expr(Expr::Ident));
            expr->text = name;
            return expr;
        }

        throw std::runtime_error("unexpected token '" + Peek().text + "'");
    }

    const std::vector<Token>& m_tokens;
    size_t                    m_pos;
    std::map<std::string, StructDef> m_structs;
};

//-----------------------------------------------------------------------------
// Execution
//-----------------------------------------------------------------------------

enum Flow
{
    Flow_Normal,
    Flow_Break,
    Flow_Continue,
    Flow_Return
};

struct Location
{
    Location() : vecComp(-1), matCol(-1), matRow(-1), column(-1) {}

    ObjPtr obj;
    int    vecComp;    // component of a vector
    int    matCol;     // matrix column
    int    matRow;     // matrix row
    int    column;     // matrix column view
};

class Interpreter
{
public:
    Interpreter(const Program& program) : m_program(program), m_maxSteps(200000), m_steps(0) {}

    Result Run(float uvX, float uvY);

private:

    struct Scope
    {
        std::map<std::string, ObjPtr> vars;
    };

    const Program& m_program;
    std::vector<Scope> m_scopes;
    int m_maxSteps;
    int m_steps;
    ObjPtr m_rastFragData;

    void Tick()
    {
        if (++m_steps > m_maxSteps)
        {
            throw std::runtime_error("execution step limit exceeded (possible infinite loop)");
        }
    }

    ObjPtr Lookup(const std::string& name)
    {
        for (int i = (int)m_scopes.size() - 1; i >= 0; --i)
        {
            std::map<std::string, ObjPtr>::iterator it = m_scopes[i].vars.find(name);
            if (it != m_scopes[i].vars.end())
            {
                return it->second;
            }
        }
        throw std::runtime_error("undeclared identifier '" + name + "'");
    }

    void Declare(const std::string& name, TypePtr type, ObjPtr value)
    {
        m_scopes.back().vars[name] = value ? value : DefaultValueFor(type);
    }

    ObjPtr DefaultValueFor(TypePtr type)
    {
        if (type && type->kind == TypeInfo::Kind_Struct)
        {
            ObjPtr obj(new Obj());
            obj->type = type;
            std::map<std::string, StructDef>::const_iterator it = m_program.structs.find(type->name);
            if (it != m_program.structs.end())
            {
                for (size_t i = 0; i < it->second.fields.size(); ++i)
                {
                    obj->fields[it->second.fields[i].first] = DefaultValueFor(it->second.fields[i].second);
                }
            }
            return obj;
        }
        if (type && type->kind == TypeInfo::Kind_Array)
        {
            ObjPtr obj(new Obj());
            obj->type = type;
            for (int i = 0; i < type->arraySize; ++i)
            {
                obj->elems.push_back(DefaultValueFor(type->element));
            }
            return obj;
        }
        ObjPtr obj(new Obj());
        obj->type = type;
        if (type && type->kind == TypeInfo::Kind_Vector)
        {
            obj->v[0] = obj->v[1] = obj->v[2] = obj->v[3] = 0.0f;
        }
        return obj;
    }

    TypePtr TypeOfObject(const ObjPtr& obj)
    {
        return obj->type;
    }

    TypePtr TypeOfLocation(const Location& loc)
    {
        if (loc.matRow >= 0 && loc.matCol >= 0)
        {
            return MakeScalarType();
        }
        if (loc.column >= 0)
        {
            return MakeVectorType(loc.obj->type->width);
        }
        if (loc.vecComp >= 0)
        {
            return MakeScalarType(loc.obj->type->isInt, loc.obj->type->isBool);
        }
        return loc.obj->type;
    }

    ObjPtr ReadLocation(const Location& loc)
    {
        if (loc.matRow >= 0 && loc.matCol >= 0)
        {
            return MakeScalar(loc.obj->m[loc.matCol][loc.matRow]);
        }
        if (loc.column >= 0)
        {
            ObjPtr result(new Obj());
            result->type = MakeVectorType(loc.obj->type->width);
            for (int i = 0; i < loc.obj->type->width; ++i)
            {
                result->v[i] = loc.obj->m[loc.column][i];
            }
            return result;
        }
        if (loc.vecComp >= 0)
        {
            return MakeScalar(loc.obj->v[loc.vecComp]);
        }
        return Clone(loc.obj);
    }

    void WriteLocation(const Location& loc, const ObjPtr& value)
    {
        if (loc.matRow >= 0 && loc.matCol >= 0)
        {
            loc.obj->m[loc.matCol][loc.matRow] = value->f;
            return;
        }
        if (loc.column >= 0)
        {
            for (int i = 0; i < loc.obj->type->width; ++i)
            {
                loc.obj->m[loc.column][i] = i < 4 ? value->v[i] : 0.0f;
            }
            return;
        }
        if (loc.vecComp >= 0)
        {
            loc.obj->v[loc.vecComp] = value->f;
            return;
        }
        CopyContents(loc.obj, value);
    }

    bool ResolvePath(ExprPtr expr, Location& out);

    void ResolveSwizzle(ExprPtr baseExpr, const std::string& field, std::vector<Location>& out);

    ObjPtr Eval(ExprPtr expr);
    ObjPtr ValueObject(TypePtr type, const float* values);
    ObjPtr Construct(const std::string& typeName, const std::vector<ObjPtr>& args);

    ObjPtr EvalCall(ExprPtr expr);
    ObjPtr CallFunction(const FunctionDef& function, ExprPtr call);

    void ExecStatement(StmtPtr stmt, Flow& flow, ObjPtr& returnValue);
    void ExecBlock(StmtPtr stmt, Flow& flow, ObjPtr& returnValue);

    ObjPtr BinaryOp(const std::string& op, const ObjPtr& a, const ObjPtr& b);
    ObjPtr UnaryOp(const std::string& op, const ObjPtr& a);
    ObjPtr AssignOp(const std::string& op, ExprPtr lhs, const ObjPtr& rhs);
    ObjPtr Intrinsic(const std::string& name, const std::vector<ObjPtr>& args, bool& handled);
};

static float Component(const ObjPtr& value, int index)
{
    if (value->type->kind == TypeInfo::Kind_Vector)
    {
        return index < value->type->width ? value->v[index] : 0.0f;
    }
    return index == 0 ? value->f : 0.0f;
}

static int ComponentCount(const ObjPtr& value)
{
    if (value->type->kind == TypeInfo::Kind_Vector)
    {
        return value->type->width;
    }
    return 1;
}

ObjPtr Interpreter::Construct(const std::string& typeName, const std::vector<ObjPtr>& args)
{
    ObjPtr result(new Obj());

    if (typeName == "float")
    {
        result->type = MakeScalarType();
        result->f = args.empty() ? 0.0f : Component(args[0], 0);
        return result;
    }
    if (typeName == "int" || typeName == "uint")
    {
        result->type = MakeScalarType(true);
        result->f = args.empty() ? 0.0f : (float)(int)Component(args[0], 0);
        return result;
    }
    if (typeName == "bool")
    {
        result->type = MakeScalarType(false, true);
        result->f = (!args.empty() && ToBool(args[0])) ? 1.0f : 0.0f;
        return result;
    }

    bool isVec = typeName.compare(0, 3, "vec") == 0 || typeName.compare(0, 4, "ivec") == 0 ||
                 typeName.compare(0, 4, "uvec") == 0 || typeName.compare(0, 4, "bvec") == 0;
    if (isVec)
    {
        int width = typeName[typeName.size() - 1] - '0';
        bool isInt = typeName.compare(0, 4, "ivec") == 0 || typeName.compare(0, 4, "uvec") == 0;
        bool isBool = typeName.compare(0, 4, "bvec") == 0;
        result->type = MakeVectorType(width, isInt, isBool);

        float flat[16];
        int count = 0;
        for (size_t i = 0; i < args.size(); ++i)
        {
            int components = ComponentCount(args[i]);
            for (int c = 0; c < components && count < 16; ++c)
            {
                flat[count++] = Component(args[i], c);
            }
        }
        for (int i = 0; i < width; ++i)
        {
            result->v[i] = i < count ? flat[i] : (i < 3 ? flat[count - 1] : 0.0f);
            if (isInt) result->v[i] = (float)(int)result->v[i];
        }
        return result;
    }

    if (typeName.compare(0, 3, "mat") == 0)
    {
        int size = typeName[3] - '0';
        result->type = MakeMatrixType(size);
        if (args.size() == 1 && args[0]->type->kind == TypeInfo::Kind_Matrix)
        {
            for (int c = 0; c < size; ++c)
            {
                for (int r = 0; r < size; ++r)
                {
                    result->m[c][r] = args[0]->m[c][r];
                }
            }
            return result;
        }

        float flat[16];
        int count = 0;
        for (size_t i = 0; i < args.size(); ++i)
        {
            int components = ComponentCount(args[i]);
            for (int c = 0; c < components && count < 16; ++c)
            {
                flat[count++] = Component(args[i], c);
            }
        }
        for (int c = 0; c < size; ++c)
        {
            for (int r = 0; r < size; ++r)
            {
                int index = c * size + r;
                result->m[c][r] = index < count ? flat[index] : 0.0f;
            }
        }
        return result;
    }

    // Struct constructor.
    std::map<std::string, StructDef>::const_iterator it = m_program.structs.find(typeName);
    if (it != m_program.structs.end())
    {
        TypePtr type(new TypeInfo());
        type->kind = TypeInfo::Kind_Struct;
        type->name = typeName;
        result->type = type;
        for (size_t i = 0; i < it->second.fields.size(); ++i)
        {
            if (i < args.size())
            {
                result->fields[it->second.fields[i].first] = Clone(args[i]);
            }
            else
            {
                result->fields[it->second.fields[i].first] = DefaultValueFor(it->second.fields[i].second);
            }
        }
        return result;
    }

    throw std::runtime_error("unsupported constructor '" + typeName + "'");
}

ObjPtr Interpreter::Eval(ExprPtr expr)
{
    Tick();
    switch (expr->kind)
    {
    case Expr::Literal:
        if (expr->isInt)
        {
            return MakeInt((int)expr->number);
        }
        return MakeScalar(expr->number);

    case Expr::Ident:
        return Lookup(expr->text);

    case Expr::Unary:
        {
            if (expr->text == "++" || expr->text == "--")
            {
                Location loc;
                if (!ResolvePath(expr->a, loc))
                {
                    throw std::runtime_error("increment of non l-value");
                }
                ObjPtr old = ReadLocation(loc);
                ObjPtr value = MakeScalar(old->f + (expr->text == "++" ? 1.0f : -1.0f));
                WriteLocation(loc, value);
                return value;
            }
            ObjPtr value = Eval(expr->a);
            return UnaryOp(expr->text, value);
        }

    case Expr::Postfix:
        {
            Location loc;
            if (!ResolvePath(expr->a, loc))
            {
                throw std::runtime_error("increment of non l-value");
            }
            ObjPtr old = ReadLocation(loc);
            ObjPtr value = MakeScalar(old->f + (expr->text == "++" ? 1.0f : -1.0f));
            WriteLocation(loc, value);
            return old;
        }

    case Expr::Binary:
        {
            if (expr->text == ",")
            {
                Eval(expr->a);
                return Eval(expr->b);
            }
            if (expr->text == "&&")
            {
                if (!ToBool(Eval(expr->a))) return MakeBool(false);
                return MakeBool(ToBool(Eval(expr->b)));
            }
            if (expr->text == "||")
            {
                if (ToBool(Eval(expr->a))) return MakeBool(true);
                return MakeBool(ToBool(Eval(expr->b)));
            }
            ObjPtr a = Eval(expr->a);
            ObjPtr b = Eval(expr->b);
            return BinaryOp(expr->text, a, b);
        }

    case Expr::Assign:
        {
            ObjPtr rhs = Eval(expr->b);
            return AssignOp(expr->text, expr->a, rhs);
        }

    case Expr::Conditional:
        return ToBool(Eval(expr->a)) ? Eval(expr->b) : Eval(expr->c);

    case Expr::Call:
        return EvalCall(expr);

    case Expr::Member:
        {
            ObjPtr base = Eval(expr->a);
            if (base->type->kind == TypeInfo::Kind_Struct)
            {
                std::map<std::string, ObjPtr>::iterator it = base->fields.find(expr->text);
                if (it == base->fields.end())
                {
                    throw std::runtime_error("unknown struct field '" + expr->text + "'");
                }
                return Clone(it->second);
            }
            // Swizzle.
            float values[4];
            int count = 0;
            for (size_t i = 0; i < expr->text.size(); ++i)
            {
                int index = -1;
                char c = expr->text[i];
                if (c == 'x' || c == 'r') index = 0;
                else if (c == 'y' || c == 'g') index = 1;
                else if (c == 'z' || c == 'b') index = 2;
                else if (c == 'w' || c == 'a') index = 3;
                if (index < 0)
                {
                    throw std::runtime_error("unsupported member '" + expr->text + "'");
                }
                values[count++] = Component(base, index);
            }
            if (count == 1)
            {
                return MakeScalar(values[0]);
            }
            return ValueObject(MakeVectorType(count), values);
        }

    case Expr::Index:
        {
            Location loc;
            if (!ResolvePath(expr, loc))
            {
                throw std::runtime_error("index of non l-value");
            }
            return ReadLocation(loc);
        }
    }

    throw std::runtime_error("unsupported expression");
}

// Build a vector object from components.
ObjPtr Interpreter::ValueObject(TypePtr type, const float* values)
{
    ObjPtr obj(new Obj());
    obj->type = type;
    for (int i = 0; i < 4; ++i)
    {
        obj->v[i] = values[i];
    }
    return obj;
}

bool Interpreter::ResolvePath(ExprPtr expr, Location& out)
{
    if (!expr)
    {
        return false;
    }
    if (expr->kind == Expr::Ident)
    {
        out = Location();
        out.obj = Lookup(expr->text);
        return true;
    }
    if (expr->kind == Expr::Member)
    {
        Location base;
        if (!ResolvePath(expr->a, base))
        {
            return false;
        }

        if (base.obj->type->kind == TypeInfo::Kind_Struct)
        {
            std::map<std::string, ObjPtr>::iterator it = base.obj->fields.find(expr->text);
            if (it == base.obj->fields.end())
            {
                return false;
            }
            out = Location();
            out.obj = it->second;
            return true;
        }

        // Single component swizzle on a vector or a matrix column view.
        if (expr->text.size() != 1)
        {
            return false;
        }
        int index = -1;
        char c = expr->text[0];
        if (c == 'x' || c == 'r') index = 0;
        else if (c == 'y' || c == 'g') index = 1;
        else if (c == 'z' || c == 'b') index = 2;
        else if (c == 'w' || c == 'a') index = 3;
        if (index < 0)
        {
            return false;
        }

        out = base;
        if (base.column >= 0)
        {
            out.matCol = base.column;
            out.matRow = index;
            out.column = -1;
        }
        else if (base.vecComp >= 0)
        {
            out.vecComp = base.vecComp; // scalar swizzle maps to itself
        }
        else if (base.obj->type->kind == TypeInfo::Kind_Vector)
        {
            out.vecComp = index;
        }
        else
        {
            return false;
        }
        return true;
    }
    if (expr->kind == Expr::Index)
    {
        Location base;
        if (!ResolvePath(expr->a, base))
        {
            return false;
        }
        int index = ToInt(Eval(expr->b));

        out = Location();
        out.obj = base.obj;

        if (base.column >= 0)
        {
            out.matCol = base.column;
            out.matRow = index;
            return true;
        }
        if (base.vecComp >= 0)
        {
            return false;
        }
        if (base.obj->type->kind == TypeInfo::Kind_Array)
        {
            if (index < 0 || index >= (int)base.obj->elems.size())
            {
                throw std::runtime_error("array index out of bounds");
            }
            out.obj = base.obj->elems[index];
            return true;
        }
        if (base.obj->type->kind == TypeInfo::Kind_Vector)
        {
            out.vecComp = index;
            return true;
        }
        if (base.obj->type->kind == TypeInfo::Kind_Matrix)
        {
            out.column = index;
            return true;
        }
        return false;
    }
    // A temporary expression used as an l-value is not supported.
    return false;
}

ObjPtr Interpreter::UnaryOp(const std::string& op, const ObjPtr& a)
{
    if (op == "-")
    {
        ObjPtr r = Clone(a);
        if (a->type->kind == TypeInfo::Kind_Vector)
        {
            for (int i = 0; i < a->type->width; ++i) r->v[i] = -a->v[i];
        }
        else if (a->type->kind == TypeInfo::Kind_Matrix)
        {
            for (int c = 0; c < a->type->width; ++c)
                for (int rr = 0; rr < a->type->width; ++rr)
                    r->m[c][rr] = -a->m[c][rr];
        }
        else
        {
            r->f = -a->f;
        }
        return r;
    }
    if (op == "+")
    {
        return Clone(a);
    }
    if (op == "!")
    {
        return MakeBool(!ToBool(a));
    }
    if (op == "~")
    {
        return MakeInt(~(int)a->f);
    }
    throw std::runtime_error("unsupported unary operator " + op);
}

ObjPtr Interpreter::BinaryOp(const std::string& op, const ObjPtr& a, const ObjPtr& b)
{
    bool aMatrix = a->type->kind == TypeInfo::Kind_Matrix;
    bool bMatrix = b->type->kind == TypeInfo::Kind_Matrix;
    bool aVec = a->type->kind == TypeInfo::Kind_Vector;
    bool bVec = b->type->kind == TypeInfo::Kind_Vector;

    if (op == "*" && aMatrix && bVec)
    {
        ObjPtr r(new Obj());
        r->type = MakeVectorType(a->type->width);
        for (int row = 0; row < a->type->width; ++row)
        {
            float sum = 0.0f;
            for (int c = 0; c < a->type->width; ++c)
            {
                sum += a->m[c][row] * b->v[c];
            }
            r->v[row] = sum;
        }
        return r;
    }
    if (op == "*" && aVec && bMatrix)
    {
        ObjPtr r(new Obj());
        r->type = MakeVectorType(b->type->width);
        for (int c = 0; c < b->type->width; ++c)
        {
            float sum = 0.0f;
            for (int row = 0; row < b->type->width; ++row)
            {
                sum += a->v[row] * b->m[c][row];
            }
            r->v[c] = sum;
        }
        return r;
    }
    if (op == "*" && aMatrix && bMatrix)
    {
        int n = a->type->width;
        ObjPtr r(new Obj());
        r->type = MakeMatrixType(n);
        for (int c = 0; c < n; ++c)
        {
            for (int row = 0; row < n; ++row)
            {
                float sum = 0.0f;
                for (int k = 0; k < n; ++k)
                {
                    sum += a->m[k][row] * b->m[c][k];
                }
                r->m[c][row] = sum;
            }
        }
        return r;
    }

    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=")
    {
        float x = a->f;
        float y = b->f;
        if (aVec || bVec)
        {
            // Compare vectors component-wise; the tests only use scalar
            // comparisons, but be permissive.
            int n = aVec ? a->type->width : b->type->width;
            bool equal = true;
            for (int i = 0; i < n; ++i)
            {
                if (Component(a, i) != Component(b, i)) equal = false;
            }
            if (op == "==") return MakeBool(equal);
            if (op == "!=") return MakeBool(!equal);
            x = Component(a, 0);
            y = Component(b, 0);
        }
        if (op == "==") return MakeBool(x == y);
        if (op == "!=") return MakeBool(x != y);
        if (op == "<") return MakeBool(x < y);
        if (op == ">") return MakeBool(x > y);
        if (op == "<=") return MakeBool(x <= y);
        return MakeBool(x >= y);
    }

    if (aMatrix || bMatrix)
    {
        throw std::runtime_error("unsupported matrix operation " + op);
    }

    int width = aVec ? a->type->width : (bVec ? b->type->width : 1);
    ObjPtr r(new Obj());
    if (width > 1)
    {
        r->type = MakeVectorType(width, a->type->isInt, false);
    }
    else
    {
        r->type = MakeScalarType(a->type->isInt && b->type->isInt);
    }

    if (op == "%")
    {
        r->f = (float)((int)Component(a, 0) % (int)Component(b, 0));
        if (width > 1) r->v[0] = r->f;
        return r;
    }

    for (int i = 0; i < width; ++i)
    {
        float x = aVec ? Component(a, i) : a->f;
        float y = bVec ? Component(b, i) : b->f;
        float value = 0.0f;
        if (op == "+") value = x + y;
        else if (op == "-") value = x - y;
        else if (op == "*") value = x * y;
        else if (op == "/") value = y == 0.0f ? 0.0f : x / y;
        else throw std::runtime_error("unsupported binary operator " + op);

        if (width > 1) r->v[i] = value;
        else r->f = value;
    }
    return r;
}

ObjPtr Interpreter::AssignOp(const std::string& op, ExprPtr lhs, const ObjPtr& rhs)
{
    // Multi component swizzle assignment.
    if (lhs->kind == Expr::Member && lhs->text.size() > 1)
    {
        Location base;
        if (ResolvePath(lhs->a, base))
        {
            std::vector<Location> places;
            for (size_t i = 0; i < lhs->text.size(); ++i)
            {
                char c = lhs->text[i];
                int index = -1;
                if (c == 'x' || c == 'r') index = 0;
                else if (c == 'y' || c == 'g') index = 1;
                else if (c == 'z' || c == 'b') index = 2;
                else if (c == 'w' || c == 'a') index = 3;
                if (index < 0) throw std::runtime_error("bad swizzle write");
                Location place = base;
                if (base.column >= 0)
                {
                    place.matCol = base.column;
                    place.matRow = index;
                    place.column = -1;
                }
                else
                {
                    place.vecComp = index;
                }
                places.push_back(place);
            }
            for (size_t i = 0; i < places.size(); ++i)
            {
                ObjPtr value = MakeScalar(Component(rhs, (int)i));
                if (op != "=")
                {
                    ObjPtr old = ReadLocation(places[i]);
                    value = BinaryOp(op.substr(0, 1), old, value);
                }
                WriteLocation(places[i], value);
            }
            return rhs;
        }
    }

    Location loc;
    if (!ResolvePath(lhs, loc))
    {
        throw std::runtime_error("assignment to non l-value");
    }

    if (op == "=")
    {
        WriteLocation(loc, rhs);
        return ReadLocation(loc);
    }

    ObjPtr old = ReadLocation(loc);
    std::string binaryOp = op.substr(0, op.size() - 1);
    ObjPtr value = BinaryOp(binaryOp, old, rhs);
    WriteLocation(loc, value);
    return ReadLocation(loc);
}

ObjPtr Interpreter::EvalCall(ExprPtr expr)
{
    // Builtin constructor?
    bool isConstructor = false;
    {
        static const char* names[] = { "float", "int", "uint", "bool", "vec2", "vec3", "vec4",
                                       "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4",
                                       "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4",
                                       "mat2x2", "mat3x3", "mat4x4" };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        {
            if (expr->text == names[i]) isConstructor = true;
        }
        if (!isConstructor && m_program.structs.find(expr->text) != m_program.structs.end())
        {
            isConstructor = true;
        }
    }
    if (isConstructor)
    {
        std::vector<ObjPtr> args;
        for (size_t i = 0; i < expr->args.size(); ++i)
        {
            args.push_back(Eval(expr->args[i]));
        }
        return Construct(expr->text, args);
    }

    std::map<std::string, std::vector<FunctionDef> >::const_iterator it = m_program.functions.find(expr->text);
    if (it != m_program.functions.end())
    {
        for (size_t i = 0; i < it->second.size(); ++i)
        {
            if (it->second[i].hasBody && it->second[i].params.size() == expr->args.size())
            {
                return CallFunction(it->second[i], expr);
            }
        }
    }

    std::vector<ObjPtr> args;
    for (size_t i = 0; i < expr->args.size(); ++i)
    {
        args.push_back(Eval(expr->args[i]));
    }
    bool handled = false;
    ObjPtr result = Intrinsic(expr->text, args, handled);
    if (handled)
    {
        return result;
    }

    throw std::runtime_error("call to unknown function '" + expr->text + "'");
}

ObjPtr Interpreter::CallFunction(const FunctionDef& function, ExprPtr call)
{
    // Resolve out/inout locations before entering the function, mirroring
    // GLSL's requirement that the argument expression is evaluated once.
    std::vector<Location> outLocs;
    std::vector<ObjPtr>   outValues;
    outLocs.resize(function.params.size());
    outValues.resize(function.params.size());

    for (size_t i = 0; i < function.params.size(); ++i)
    {
        if (function.params[i].isOut)
        {
            Location loc;
            if (!ResolvePath(call->args[i], loc))
            {
                throw std::runtime_error("output argument is not an l-value");
            }
            outLocs[i] = loc;
            if (function.params[i].isInout)
            {
                outValues[i] = ReadLocation(loc);
            }
        }
    }

    m_scopes.push_back(Scope());
    for (size_t i = 0; i < function.params.size(); ++i)
    {
        const Param& param = function.params[i];
        ObjPtr value;
        if (param.isOut)
        {
            value = outValues[i] ? Clone(outValues[i]) : DefaultValueFor(param.type);
        }
        else
        {
            value = Clone(Eval(call->args[i]));
        }
        // Coerce to the parameter type.
        value->type = param.type;
        m_scopes.back().vars[param.name] = value;
    }

    Flow flow = Flow_Normal;
    ObjPtr returnValue;
    ExecBlock(function.body, flow, returnValue);

    // Copy out/inout values back before the scope disappears.
    for (size_t i = 0; i < function.params.size(); ++i)
    {
        if (function.params[i].isOut)
        {
            std::map<std::string, ObjPtr>::iterator it = m_scopes.back().vars.find(function.params[i].name);
            if (it != m_scopes.back().vars.end())
            {
                WriteLocation(outLocs[i], it->second);
            }
        }
    }
    m_scopes.pop_back();

    if (function.returnType && function.returnType->kind != TypeInfo::Kind_Void)
    {
        return returnValue ? returnValue : DefaultValueFor(function.returnType);
    }
    return MakeScalar(0.0f);
}

ObjPtr Interpreter::Intrinsic(const std::string& name, const std::vector<ObjPtr>& args, bool& handled)
{
    handled = true;
    if (args.empty())
    {
        handled = false;
        return ObjPtr();
    }

    int width = args[0]->type->kind == TypeInfo::Kind_Vector ? args[0]->type->width : 1;
    bool isVec = width > 1;

    if (name == "abs")
    {
        ObjPtr r = Clone(args[0]);
        for (int i = 0; i < width; ++i)
        {
            if (isVec) r->v[i] = fabsf(args[0]->v[i]);
            else r->f = fabsf(args[0]->f);
        }
        return r;
    }
    if (name == "floor" || name == "ceil" || name == "fract" || name == "sqrt")
    {
        ObjPtr r = Clone(args[0]);
        for (int i = 0; i < width; ++i)
        {
            float x = isVec ? args[0]->v[i] : args[0]->f;
            float y = x;
            if (name == "floor") y = floorf(x);
            else if (name == "ceil") y = ceilf(x);
            else if (name == "fract") y = x - floorf(x);
            else if (name == "sqrt") y = sqrtf(x);
            if (isVec) r->v[i] = y; else r->f = y;
        }
        return r;
    }
    if (name == "min" || name == "max")
    {
        ObjPtr r = Clone(args[0]);
        for (int i = 0; i < width; ++i)
        {
            float x = isVec ? args[0]->v[i] : args[0]->f;
            float y = args[1]->type->kind == TypeInfo::Kind_Vector ? args[1]->v[i] : args[1]->f;
            float v = (name == "min") ? (x < y ? x : y) : (x > y ? x : y);
            if (isVec) r->v[i] = v; else r->f = v;
        }
        return r;
    }
    if (name == "clamp")
    {
        ObjPtr r = Clone(args[0]);
        for (int i = 0; i < width; ++i)
        {
            float x = isVec ? args[0]->v[i] : args[0]->f;
            float lo = args[1]->type->kind == TypeInfo::Kind_Vector ? args[1]->v[i] : args[1]->f;
            float hi = args[2]->type->kind == TypeInfo::Kind_Vector ? args[2]->v[i] : args[2]->f;
            float v = x < lo ? lo : (x > hi ? hi : x);
            if (isVec) r->v[i] = v; else r->f = v;
        }
        return r;
    }
    if (name == "dot")
    {
        int n = args[0]->type->width;
        float sum = 0.0f;
        for (int i = 0; i < n; ++i) sum += args[0]->v[i] * args[1]->v[i];
        return MakeScalar(sum);
    }
    if (name == "length")
    {
        int n = args[0]->type->width;
        float sum = 0.0f;
        for (int i = 0; i < n; ++i) sum += args[0]->v[i] * args[0]->v[i];
        return MakeScalar(sqrtf(sum));
    }
    if (name == "mix")
    {
        ObjPtr r = Clone(args[0]);
        for (int i = 0; i < width; ++i)
        {
            float x = args[0]->v[i];
            float y = args[1]->v[i];
            float t = args[2]->type->kind == TypeInfo::Kind_Vector ? args[2]->v[i] : args[2]->f;
            r->v[i] = x + (y - x) * t;
        }
        return r;
    }
    if (name == "mod")
    {
        ObjPtr r = Clone(args[0]);
        for (int i = 0; i < width; ++i)
        {
            float x = args[0]->v[i];
            float y = args[1]->type->kind == TypeInfo::Kind_Vector ? args[1]->v[i] : args[1]->f;
            r->v[i] = x - y * floorf(x / y);
        }
        return r;
    }
    if (name == "transpose")
    {
        ObjPtr r(new Obj());
        int n = args[0]->type->width;
        r->type = MakeMatrixType(n);
        for (int c = 0; c < n; ++c)
            for (int row = 0; row < n; ++row)
                r->m[c][row] = args[0]->m[row][c];
        return r;
    }

    handled = false;
    return ObjPtr();
}

void Interpreter::ExecBlock(StmtPtr stmt, Flow& flow, ObjPtr& returnValue)
{
    m_scopes.push_back(Scope());
    for (size_t i = 0; i < stmt->statements.size() && flow == Flow_Normal; ++i)
    {
        ExecStatement(stmt->statements[i], flow, returnValue);
    }
    m_scopes.pop_back();
}

void Interpreter::ExecStatement(StmtPtr stmt, Flow& flow, ObjPtr& returnValue)
{
    Tick();
    switch (stmt->kind)
    {
    case Stmt::Empty:
        break;

    case Stmt::Block:
        ExecBlock(stmt, flow, returnValue);
        break;

    case Stmt::Decl:
        {
            ObjPtr value;
            if (stmt->hasInit)
            {
                value = Clone(Eval(stmt->init));
                value->type = stmt->type;
            }
            Declare(stmt->name, stmt->type, value);
        }
        break;

    case Stmt::ExprStmt:
        Eval(stmt->expr);
        break;

    case Stmt::If:
        if (ToBool(Eval(stmt->cond)))
        {
            ExecStatement(stmt->a, flow, returnValue);
        }
        else if (stmt->b)
        {
            ExecStatement(stmt->b, flow, returnValue);
        }
        break;

    case Stmt::For:
        {
            m_scopes.push_back(Scope());
            if (stmt->name.size())
            {
                ObjPtr value;
                if (stmt->hasInit)
                {
                    value = Clone(Eval(stmt->init));
                    value->type = stmt->type;
                }
                Declare(stmt->name, stmt->type, value);
            }
            else if (stmt->init)
            {
                Eval(stmt->init);
            }

            while (true)
            {
                if (stmt->cond && !ToBool(Eval(stmt->cond)))
                {
                    break;
                }
                Flow inner = Flow_Normal;
                ExecStatement(stmt->a, inner, returnValue);
                if (inner == Flow_Return)
                {
                    flow = Flow_Return;
                    break;
                }
                if (inner == Flow_Break)
                {
                    break;
                }
                if (stmt->incr)
                {
                    Eval(stmt->incr);
                }
            }
            m_scopes.pop_back();
        }
        break;

    case Stmt::While:
        while (ToBool(Eval(stmt->cond)))
        {
            Flow inner = Flow_Normal;
            ExecStatement(stmt->a, inner, returnValue);
            if (inner == Flow_Return)
            {
                flow = Flow_Return;
                break;
            }
            if (inner == Flow_Break)
            {
                break;
            }
        }
        break;

    case Stmt::Return:
        if (stmt->expr)
        {
            returnValue = Eval(stmt->expr);
        }
        flow = Flow_Return;
        break;

    case Stmt::Break:
        flow = Flow_Break;
        break;

    case Stmt::Continue:
        flow = Flow_Continue;
        break;
    }
}

Result Interpreter::Run(float uvX, float uvY)
{
    Result result;

    m_scopes.push_back(Scope());

    // Globals.
    for (size_t i = 0; i < m_program.globals.size(); ++i)
    {
        const std::string& name = m_program.globals[i].first;
        TypePtr type = m_program.globals[i].second;
        ObjPtr value = DefaultValueFor(type);
        if (m_program.globalInits[i].second)
        {
            value = Clone(Eval(m_program.globalInits[i].second));
            value->type = type;
        }
        m_scopes.back().vars[name] = value;
        if (name == "rast_FragData")
        {
            m_rastFragData = value;
        }
    }

    if (m_scopes.back().vars.count("frag_TEXCOORD0"))
    {
        ObjPtr uv = m_scopes.back().vars["frag_TEXCOORD0"];
        uv->v[0] = uvX;
        uv->v[1] = uvY;
    }
    // Common alternative input name.
    if (m_scopes.back().vars.count("uv"))
    {
        ObjPtr uv = m_scopes.back().vars["uv"];
        uv->v[0] = uvX;
        uv->v[1] = uvY;
    }

    try
    {
        std::map<std::string, std::vector<FunctionDef> >::const_iterator it = m_program.functions.find("main");
        if (it == m_program.functions.end())
        {
            result.error = "shader has no main()";
            return result;
        }
        for (size_t i = 0; i < it->second.size(); ++i)
        {
            if (it->second[i].hasBody)
            {
                Flow flow = Flow_Normal;
                ObjPtr returnValue;
                ExecBlock(it->second[i].body, flow, returnValue);
                break;
            }
        }

        if (m_rastFragData && !m_rastFragData->elems.empty())
        {
            ObjPtr color = m_rastFragData->elems[0];
            result.shaderOutput = Value::Vector(4, color->v);
        }
        else
        {
            result.error = "shader did not write rast_FragData[0]";
            return result;
        }
    }
    catch (const std::exception& e)
    {
        result.error = e.what();
        return result;
    }

    result.ok = true;
    return result;
}

} // anonymous parsing namespace (ends implicitly below)

Result RunFragment(const std::string& source, float uvX, float uvY)
{
    Result result;
    try
    {
        Lexer lexer(source);
        lexer.Run();

        Program program;
        Parser parser(lexer.tokens);
        parser.ParseProgram(program);

        Interpreter interpreter(program);
        return interpreter.Run(uvX, uvY);
    }
    catch (const std::exception& e)
    {
        result.error = e.what();
        return result;
    }
}

} // namespace glsl
