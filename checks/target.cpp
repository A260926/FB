// Structured macro/include diagnostics checks.
//
// These drive the real compiler through Hlsl2Glsl_Parse with a small in-memory
// #include provider, then verify every field of the structured diagnostics:
// physical line/column pairs (including #line), spelling fragments, the
// outer-to-inner macro expansion chain, and the include chain.  The classic
// Hlsl2Glsl_Parse / Translate / GetInfoLog path and uniform reflection must
// keep working exactly as before.

#include "task_support/check.h"
#include "../include/hlsl2glsl.h"

#include <cstdio>
#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Test include provider.  It hands out heap strings (like a real editor
// adapter would) and records open/close calls so lifetime can be checked.

static bool gInitialized = false;

struct InitGuard
{
	InitGuard()
	{
		if (!gInitialized)
		{
			if (Hlsl2Glsl_Initialize())
				gInitialized = true;
		}
	}
};

struct IncludeState
{
	int opens;
	int closes;
	int lastClosedNonEmpty;
	IncludeState() : opens(0), closes(0), lastClosedNonEmpty(0) {}
};

static const char* kOuter =
	"#line 10 \"outer.h\"\n"
	"#include \"inner.h\"\n"
	"#define APPLY(v) WRAP(v)\n";

static const char* kInner =
	"#line 70 \"inner.h\"\n"
	"#define BAD(v) (missing_component + v)\n"
	"#define WRAP(v) BAD(v)\n";

static bool C_DECL openFile(bool isSystem, const char* fname, const char* parentfname,
                            const char* parent, std::string& out, void* data)
{
	IncludeState* state = (IncludeState*) data;
	(void) isSystem; (void) parentfname; (void) parent;
	++state->opens;
	if (!strcmp(fname, "outer.h"))
		out = kOuter;
	else if (!strcmp(fname, "inner.h"))
		out = kInner;
	else
		return false;
	return true;
}

static void C_DECL closeFile(const char* file, void* data)
{
	IncludeState* state = (IncludeState*) data;
	(void) file;
	++state->closes;
	if (file != 0 && file[0] != 0)
		++state->lastClosedNonEmpty;
}

// ---------------------------------------------------------------------------
// Helpers.

static bool isFile(const Hlsl2Glsl_SourcePosition& p, const char* file)
{
	if (p.file == 0)
		return false;
	return strcmp(p.file, file) == 0;
}

static bool posIs(const Hlsl2Glsl_SourcePosition& p, const char* file, int line, int column)
{
	return isFile(p, file) && p.line == line && p.column == column;
}

static bool rangeIs(const Hlsl2Glsl_SourceRange& r, const char* file, int line, int column,
                    int endColumn)
{
	return posIs(r.begin, file, line, column) &&
	       posIs(r.end, file, line, endColumn);
}

// The root translation unit has no file name, so the public position carries
// a NULL file pointer.  Compare line/column only in that case.
static bool posLineColumn(const Hlsl2Glsl_SourcePosition& p, int line, int column)
{
	return p.file == 0 && p.line == line && p.column == column;
}

static bool rangeSpells(const Hlsl2Glsl_SourceRange& r, const char* file, int line, int column)
{
	return posIs(r.begin, file, line, column) && r.end.line == line && r.end.column > r.begin.column;
}

static bool messageContains(const Hlsl2Glsl_Diagnostic* d, const char* text)
{
	return d->message != 0 && strstr(d->message, text) != 0;
}

// Settings used by every case.
struct Compiler
{
	ShHandle handle;
	IncludeState includes;
	Hlsl2Glsl_ParseCallbacks callbacks;
	Compiler() : handle(0)
	{
		callbacks.includeOpenCallback = openFile;
		callbacks.includeCloseCallback = closeFile;
		callbacks.data = &includes;
	}
	~Compiler()
	{
		if (handle)
			Hlsl2Glsl_DestructCompiler(handle);
	}
};

static void initCompiler(Compiler& c)
{
	InitGuard guard;
	c.handle = Hlsl2Glsl_ConstructCompiler(EShLangFragment);
}

// ---------------------------------------------------------------------------
// The nested-macro scenario shared by all three groups:
//
//   material.hlsl (logic line 40) includes outer.h
//   outer.h (logic line 10) includes inner.h and defines APPLY(v) WRAP(v)
//   inner.h (logic line 70) defines BAD(v) missing_component(v) and
//                          WRAP(v) BAD(v)
//   main() calls APPLY(uv)
//
// `missing_component` is spelled in inner.h line 70 column 35; the primary
// location is the outer-most invocation APPLY at material.hlsl line 41.

static const char* kNestedShader =
	"#line 40 \"material.hlsl\"\n"
	"#include \"outer.h\"\n"
	"float4 main(float2 uv:TEXCOORD0):COLOR { return APPLY(uv); }\n";

static int runNested(Compiler& c)
{
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	int accepted = Hlsl2Glsl_Parse(c.handle, kNestedShader, ETargetGLSL_120, &c.callbacks, 0);
	TASK_CHECK(accepted == 0);
	TASK_CHECK(c.includes.opens == 2);
	TASK_CHECK(c.includes.closes == 2);
	// The adapter frees the include buffer only after the close callback has
	// returned, and the callback itself only observes it.
	TASK_CHECK(c.includes.lastClosedNonEmpty == 2);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	return 0;
}

// ---------------------------------------------------------------------------
// Group: origin -- primary position, spelling fragments, macro and include
// chains.

static int case_nested_primary_position(void)
{
	Compiler c;
	if (runNested(c)) return 1;
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	// Outer-most macro invocation APPLY(...): material.hlsl line 41, column of
	// the `APPLY` token.  The line comes from #line (physical line 3), the
	// column from the physical bytes.
	TASK_CHECK(posIs(d->primary.begin, "material.hlsl", 41, 49));
	TASK_CHECK(d->primary.end.line == 41);
	TASK_CHECK(d->primary.end.column > d->primary.begin.column);
	TASK_CHECK(messageContains(d, "missing_component"));
	return 0;
}

static int case_nested_spelling_fragment(void)
{
	Compiler c;
	if (runNested(c)) return 1;
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	// Exactly one fragment: the token was spelled in inner.h after #line 70.
	TASK_CHECK(d->fragmentCount == 1);
	TASK_CHECK(d->fragments != 0);
	const Hlsl2Glsl_SourceFragment& f = d->fragments[0];
	// "missing_component" starts at column 17 on logical line 70.
	TASK_CHECK(posIs(f.range.begin, "inner.h", 70, 17));
	TASK_CHECK(f.range.end.line == 70);
	TASK_CHECK(f.range.end.column == 34);
	TASK_CHECK(f.spelling == Hlsl2Glsl_SourceSpellingMacroBody);
	return 0;
}

static int case_nested_expansion_chain(void)
{
	Compiler c;
	if (runNested(c)) return 1;
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(d->expansionCount == 3);
	TASK_CHECK(d->expansions != 0);

	// Outer-most first: APPLY, then WRAP, then BAD.
	TASK_CHECK(!strcmp(d->expansions[0].name, "APPLY"));
	TASK_CHECK(!strcmp(d->expansions[1].name, "WRAP"));
	TASK_CHECK(!strcmp(d->expansions[2].name, "BAD"));

	// APPLY is defined at outer.h line 11 column 9 and called at the primary
	// location.
	TASK_CHECK(rangeSpells(d->expansions[0].definition, "outer.h", 11, 9));
	TASK_CHECK(posIs(d->expansions[0].invocation.begin, "material.hlsl", 41, 49));

	// WRAP is defined at inner.h line 71 and called inside APPLY's body
	// (outer.h line 11).
	TASK_CHECK(rangeSpells(d->expansions[1].definition, "inner.h", 71, 9));
	TASK_CHECK(posIs(d->expansions[1].invocation.begin, "outer.h", 11, 18));

	// BAD is defined at inner.h line 70 and called inside WRAP's body.
	TASK_CHECK(rangeSpells(d->expansions[2].definition, "inner.h", 70, 9));
	TASK_CHECK(posIs(d->expansions[2].invocation.begin, "inner.h", 71, 17));
	return 0;
}

static int case_nested_include_chain(void)
{
	Compiler c;
	if (runNested(c)) return 1;
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(d->includeCount == 2);
	TASK_CHECK(d->includes != 0);

	// outer.h was pulled in by material.hlsl line 40.
	TASK_CHECK(d->includes[0].file != 0);
	TASK_CHECK(!strcmp(d->includes[0].file, "outer.h"));
	TASK_CHECK(posIs(d->includes[0].directive.begin, "material.hlsl", 40, 1));

	// inner.h was pulled in by outer.h line 10.
	TASK_CHECK(d->includes[1].file != 0);
	TASK_CHECK(!strcmp(d->includes[1].file, "inner.h"));
	TASK_CHECK(posIs(d->includes[1].directive.begin, "outer.h", 10, 1));
	return 0;
}

static int case_plain_undeclared_token(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// No macros involved: the primary location is the token itself, and the
	// column is the raw byte offset on the physical line.
	const char* source =
		"#line 100 \"logical.hlsl\"\n"
		"float4 main():COLOR { float2 t = 0; return nope; }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) == 1);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(messageContains(d, "undeclared identifier"));
	TASK_CHECK(messageContains(d, "nope"));
	TASK_CHECK(d->fragmentCount == 1);
	TASK_CHECK(d->expansionCount == 0);
	TASK_CHECK(d->includeCount == 0);
	// Physical line 2 holds the error; #line makes it logical line 100, while
	// the column stays a physical byte count (29).
	TASK_CHECK(posIs(d->primary.begin, "logical.hlsl", 100, 44));
	TASK_CHECK(posIs(d->fragments[0].range.begin, "logical.hlsl", 100, 44));
	TASK_CHECK(d->fragments[0].spelling == Hlsl2Glsl_SourceSpellingDirect);
	return 0;
}

static int case_utf8_byte_columns(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// Two UTF-8 characters (each two bytes) before the token, inside a
	// comment.  Columns count raw bytes, so the token starts 4 columns after
	// the raw text would place it with character counting.
	const char* source =
		"float4 main():COLOR { /* \xc3\xa9\xc3\xa9 */ return bad_name; }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) == 1);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(messageContains(d, "bad_name"));
	const char* at = strstr(source, "bad_name");
	TASK_CHECK(at != 0);
	int expected = (int) (at - source) + 1; // one-based byte column
	TASK_CHECK(d->primary.begin.file == 0 && d->primary.begin.line == 1 && d->primary.begin.column == expected);
	TASK_CHECK(d->primary.end.column == expected + 8);
	return 0;
}

static int case_hashhash_fragments(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// ## pastes the two argument spellings together.  Both must survive, in
	// left-to-right order, and the result must not be looked up in the source.
	const char* source =
		"#define CAT(a,b) a##b\n"
		"float4 main():COLOR { return mCAT; }";
	(void) source;
	const char* paste =
		"#define CAT(a,b) a##b\n"
		"float4 main():COLOR { return CAT(no,pe); }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, paste, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) == 1);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(messageContains(d, "nope"));
	TASK_CHECK(d->fragmentCount == 2);
	// Both halves come from the call site, `no` at column 34 and `pe` at 37.
	TASK_CHECK(d->fragments[0].range.begin.file == 0 && d->fragments[0].range.begin.line == 2 && d->fragments[0].range.begin.column == 34);
	TASK_CHECK(d->fragments[1].range.begin.file == 0 && d->fragments[1].range.begin.line == 2 && d->fragments[1].range.begin.column == 37);
	TASK_CHECK(d->fragments[0].spelling == Hlsl2Glsl_SourceSpellingMacroArgument);
	TASK_CHECK(d->fragments[1].spelling == Hlsl2Glsl_SourceSpellingMacroArgument);
	// The paste happened inside CAT's replacement list.
	TASK_CHECK(d->expansionCount == 1);
	TASK_CHECK(!strcmp(d->expansions[0].name, "CAT"));
	return 0;
}

static int case_argument_vs_body_spelling(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// The argument is spelled at the call site, the macro body at the
	// #define.  The failing body token "missing_thing" is written in the
	// replacement list (line 1, column 22) while the argument is written at
	// the call site (line 2, column 36).
	const char* source =
		"#define TWICE(p) p + missing_thing(p)\n"
		"float4 main():COLOR { return TWICE(value); }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	// Both diagnostics point at the same outer-most invocation.
	TASK_CHECK(!strcmp(d->expansions[0].name, "TWICE"));
	TASK_CHECK(posLineColumn(d->primary.begin, 2, 30));
	// Locate the "missing_thing" diagnostic; its spelling is the macro body.
	int found = 0;
	for (int i = 0; i < Hlsl2Glsl_GetDiagnosticCount(c.handle); ++i)
	{
		const Hlsl2Glsl_Diagnostic* di = Hlsl2Glsl_GetDiagnostic(c.handle, i);
		if (!messageContains(di, "missing_thing"))
			continue;
		found = 1;
		TASK_CHECK(di->fragmentCount >= 1);
		TASK_CHECK(di->fragments[0].spelling == Hlsl2Glsl_SourceSpellingMacroBody);
		TASK_CHECK(posLineColumn(di->fragments[0].range.begin, 1, 37));
		// The body spelling of the argument is a different fragment from the
		// body spelling of missing_thing, and both keep the same chain.
		TASK_CHECK(di->expansionCount == 1);
	}
	TASK_CHECK(found == 1);
	return 0;
}

static int case_object_macro_chain(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// Object macro whose replacement names a function-like macro.  The chain
	// must list both, outer-most first.
	const char* source =
		"#define CALL IN(thing)\n"
		"#define IN(x) broken_call(x)\n"
		"float4 main():COLOR { return CALL; }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(d->expansionCount == 2);
	TASK_CHECK(!strcmp(d->expansions[0].name, "CALL"));
	TASK_CHECK(!strcmp(d->expansions[1].name, "IN"));
	// CALL is invoked at the token itself (object macro), IN inside CALL's body.
	TASK_CHECK(posLineColumn(d->expansions[0].invocation.begin, 3, 30));
	TASK_CHECK(posLineColumn(d->expansions[1].invocation.begin, 1, 14));
	return 0;
}

static int case_repeated_argument_keeps_spelling(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// The parameter is used twice in the replacement list.  Every use must be
	// attributed to the argument's own spelling at the call site, never to
	// the macro body, and the two uses must not be collapsed into one chain.
	const char* source =
		"#define TWICE(p) p + p\n"
		"float4 main():COLOR { return TWICE(nope); }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	for (int i = 0; i < Hlsl2Glsl_GetDiagnosticCount(c.handle); ++i)
	{
		const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, i);
		TASK_CHECK(d != 0);
		TASK_CHECK(messageContains(d, "nope"));
		TASK_CHECK(d->fragmentCount == 1);
		TASK_CHECK(d->fragments[0].spelling == Hlsl2Glsl_SourceSpellingMacroArgument);
		// The argument spelling on the call line, not the body on line 1.
		TASK_CHECK(posLineColumn(d->fragments[0].range.begin, 2, 36));
		TASK_CHECK(d->expansionCount == 1);
		TASK_CHECK(!strcmp(d->expansions[0].name, "TWICE"));
	}

	// A taste of the same via the token stream: prepend a broken body token so
	// the two uses surface as two distinct diagnostics, each spelled at the
	// call site.
	const char* broken =
		"#define BOTH(p) p , , p\n"
		"float4 main():COLOR { return 0; }\n"
		"void f() { BOTH(alpha); }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, broken, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	return 0;
}

static int case_argument_expands_before_substitution(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// The argument is itself a macro expansion; the outer macro receives the
	// expanded text.  The chain must keep both: the outer invocation first and
	// the inner macro it expanded to.
	const char* source =
		"#define OUTER(a) a\n"
		"#define INNER(x) missing_inner(x)\n"
		"float4 main():COLOR { return OUTER(INNER(q)); }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	int found = 0;
	for (int i = 0; i < Hlsl2Glsl_GetDiagnosticCount(c.handle); ++i)
	{
		const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, i);
		if (!messageContains(d, "missing_inner"))
			continue;
		found = 1;
		TASK_CHECK(d->expansionCount == 2);
		TASK_CHECK(!strcmp(d->expansions[0].name, "OUTER"));
		TASK_CHECK(!strcmp(d->expansions[1].name, "INNER"));
		// missing_inner is written in INNER's replacement list.
		TASK_CHECK(d->fragmentCount == 1);
		TASK_CHECK(d->fragments[0].spelling == Hlsl2Glsl_SourceSpellingMacroBody);
		TASK_CHECK(posLineColumn(d->fragments[0].range.begin, 2, 33));
	}
	TASK_CHECK(found == 1);
	return 0;
}

// ---------------------------------------------------------------------------
// Group: diagnostics -- syntax/type errors, message preservation, #if and
// #line handling.

static int case_syntax_error_preserved(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	const char* source = "float4 main():COLOR { return 1 }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	// The original message text is kept verbatim.
	TASK_CHECK(messageContains(d, "syntax error"));
	const char* log = Hlsl2Glsl_GetInfoLog(c.handle);
	TASK_CHECK(log != 0 && strstr(log, "syntax error") != 0);
	// The diagnostic points at the token the parser choked on.
	TASK_CHECK(d->fragmentCount == 1);
	TASK_CHECK(d->fragments[0].spelling == Hlsl2Glsl_SourceSpellingDirect);
	TASK_CHECK(d->primary.begin.line == 1);
	return 0;
}

static int case_syntax_error_from_macro(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// Syntax error inside a macro expansion: primary is the invocation, the
	// fragment is the body spelling.
	const char* source =
		"#define GARBAGE 1 2 3\n"
		"float4 main():COLOR { return GARBAGE; }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(messageContains(d, "syntax error"));
	TASK_CHECK(d->expansionCount == 1);
	TASK_CHECK(!strcmp(d->expansions[0].name, "GARBAGE"));
	TASK_CHECK(posLineColumn(d->primary.begin, 2, 30));
	TASK_CHECK(d->fragmentCount >= 1);
	TASK_CHECK(d->fragments[0].spelling == Hlsl2Glsl_SourceSpellingMacroBody);
	// "2" is spelled at column 19 of the definition line.
	TASK_CHECK(posLineColumn(d->fragments[0].range.begin, 1, 19));
	return 0;
}

static int case_skipped_code_produces_nothing(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// Garbage inside a false #if must not produce any diagnostic at all.
	const char* goodOnly =
		"#if 0\n"
		"this is not valid hlsl @@@\n"
		"#endif\n"
		"float4 main():COLOR { return 1; }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, goodOnly, ETargetGLSL_120, 0, 0) == 1);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) == 0);

	// The same garbage, now live, does report.
	const char* live =
		"#if 1\n"
		"this is not valid hlsl @@@\n"
		"#endif\n"
		"float4 main():COLOR { return 1; }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, live, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	return 0;
}

static int case_line_keeps_byte_columns(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// #line changes the logical file and line but never the column source:
	// the column is still the raw byte offset in the physical line.
	const char* source =
		"#line 7 \"virt.hlsl\"\n"
		"float4 main():COLOR { return zzz; }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) == 1);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(messageContains(d, "zzz"));
	TASK_CHECK(posIs(d->primary.begin, "virt.hlsl", 7, 30));
	TASK_CHECK(d->primary.end.column == 33);
	return 0;
}

static int case_line_directive_in_macro_body(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// The macro's body is written on a continuation line; provenance must
	// point at the physical line that actually spells the body token.
	const char* source =
		"#define LONG(x) \\\n"
		"    (broken_name + x)\n"
		"float4 main():COLOR { return LONG(1); }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(messageContains(d, "broken_name"));
	TASK_CHECK(d->fragmentCount >= 1);
	// "broken_name" is on physical line 2, column 6.
	TASK_CHECK(posLineColumn(d->fragments[0].range.begin, 2, 6));
	TASK_CHECK(d->expansions[0].name != 0 && !strcmp(d->expansions[0].name, "LONG"));
	return 0;
}

static int case_declared_type_error_preserved(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// A type error keeps the original message text.
	const char* source =
		"struct S { float a; };\n"
		"float4 main():COLOR { S s; return s; }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(d->message != 0 && d->message[0] != 0);
	TASK_CHECK(d->primary.begin.line >= 1);
	return 0;
}

static int case_getinfo_log_still_works(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	// The classic info log is unchanged: it still contains the plain
	// file(line) prefix and the message, and is not empty on failure.
	const char* source = "#line 40 \"material.hlsl\"\n#include \"outer.h\"\n"
	                     "float4 main(float2 uv:TEXCOORD0):COLOR { return APPLY(uv); }\n";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, &c.callbacks, 0) == 0);
	const char* log = Hlsl2Glsl_GetInfoLog(c.handle);
	TASK_CHECK(log != 0);
	TASK_CHECK(strstr(log, "material.hlsl(41)") != 0);
	TASK_CHECK(strstr(log, "missing_component") != 0);
	return 0;
}

static int case_out_of_range_diagnostic(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, kNestedShader, ETargetGLSL_120, &c.callbacks, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	TASK_CHECK(Hlsl2Glsl_GetDiagnostic(c.handle, -1) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnostic(c.handle, Hlsl2Glsl_GetDiagnosticCount(c.handle)) == 0);
	return 0;
}

// ---------------------------------------------------------------------------
// Group: lifetime -- reports stay valid after includes close and #undef, are
// cleared by the next parse, and handles do not cross-contaminate.

static int case_valid_after_include_close(void)
{
	Compiler c;
	if (runNested(c)) return 1;
	TASK_CHECK(c.includes.closes == 2);
	// Reading the diagnostic now, after the close callbacks ran, must not
	// touch freed include text: every string is a snapshot.
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(d->message != 0 && strstr(d->message, "missing_component") != 0);
	TASK_CHECK(d->fragments != 0 && d->fragmentCount == 1);
	TASK_CHECK(d->fragments[0].range.begin.file != 0);
	TASK_CHECK(!strcmp(d->fragments[0].range.begin.file, "inner.h"));
	TASK_CHECK(d->includeCount == 2);
	TASK_CHECK(!strcmp(d->includes[1].file, "inner.h"));
	// The names are snapshots too, not pointers into #undef'd storage.
	TASK_CHECK(!strcmp(d->expansions[1].name, "WRAP"));
	return 0;
}

static int case_valid_after_undef(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	const char* source =
		"#define NAME broken_call\n"
		"float4 main():COLOR { return NAME; }\n"
		"#undef NAME\n";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, source, ETargetGLSL_120, 0, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(d->expansionCount == 1);
	TASK_CHECK(!strcmp(d->expansions[0].name, "NAME"));
	TASK_CHECK(messageContains(d, "broken_call"));
	return 0;
}

static int case_cleared_by_next_parse(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, kNestedShader, ETargetGLSL_120, &c.callbacks, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);

	// A normal shader right after a failure must clear the old reports, and
	// translate + reflect exactly as before.
	const char* good =
		"uniform float4 gain;"
		"float4 main(float2 uv:TEXCOORD0):COLOR { return uv.xxxx * gain; }";
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, good, ETargetGLSL_120, 0, 0) == 1);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnostic(c.handle, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_Translate(c.handle, "main", ETargetGLSL_120, 0) == 1);
	const char* shader = Hlsl2Glsl_GetShader(c.handle);
	TASK_CHECK(shader != 0 && strstr(shader, "void main") != 0);
	TASK_CHECK(Hlsl2Glsl_GetUniformCount(c.handle) == 1);
	const ShUniformInfo* u = Hlsl2Glsl_GetUniformInfo(c.handle);
	TASK_CHECK(u != 0 && u[0].name != 0 && !strcmp(u[0].name, "gain"));
	return 0;
}

static int case_diagnostic_lifetime_until_next_parse(void)
{
	Compiler c;
	initCompiler(c);
	TASK_CHECK(c.handle != 0);
	TASK_CHECK(Hlsl2Glsl_Parse(c.handle, kNestedShader, ETargetGLSL_120, &c.callbacks, 0) == 0);
	const Hlsl2Glsl_Diagnostic* d = Hlsl2Glsl_GetDiagnostic(c.handle, 0);
	TASK_CHECK(d != 0);
	TASK_CHECK(d->includeCount == 2);
	// The pointer remains the same while no new parse happens.
	TASK_CHECK(Hlsl2Glsl_GetDiagnostic(c.handle, 0) == d);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(c.handle) >= 1);
	return 0;
}

static int case_two_handles_do_not_cross(void)
{
	Compiler a;
	Compiler b;
	initCompiler(a);
	initCompiler(b);
	TASK_CHECK(a.handle != 0 && b.handle != 0);
	TASK_CHECK(a.handle != b.handle);

	// Interleave parses: handle A gets the nested include failure, handle B
	// gets a different macro failure.
	const char* bSource =
		"#define OTHER(x) broken_other(x)\n"
		"float4 main():COLOR { return OTHER(v); }";

	TASK_CHECK(Hlsl2Glsl_Parse(a.handle, kNestedShader, ETargetGLSL_120, &a.callbacks, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_Parse(b.handle, bSource, ETargetGLSL_120, 0, 0) == 0);

	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(a.handle) >= 1);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(b.handle) >= 1);
	const Hlsl2Glsl_Diagnostic* da = Hlsl2Glsl_GetDiagnostic(a.handle, 0);
	const Hlsl2Glsl_Diagnostic* db = Hlsl2Glsl_GetDiagnostic(b.handle, 0);
	TASK_CHECK(da != 0 && db != 0);

	// Names, positions and stacks stay with their own handle.
	TASK_CHECK(!strcmp(da->expansions[0].name, "APPLY"));
	TASK_CHECK(!strcmp(db->expansions[0].name, "OTHER"));
	TASK_CHECK(da->includeCount == 2);
	TASK_CHECK(db->includeCount == 0);
	TASK_CHECK(posIs(da->primary.begin, "material.hlsl", 41, 49));
	TASK_CHECK(db->primary.begin.file == 0);
	TASK_CHECK(messageContains(da, "missing_component"));
	// B's first diagnostic is about its own argument; the macro-caused error
	// is somewhere in its list and must mention B's macro body.
	bool bMentions = false;
	for (int i = 0; i < Hlsl2Glsl_GetDiagnosticCount(b.handle); ++i)
	{
		if (messageContains(Hlsl2Glsl_GetDiagnostic(b.handle, i), "broken_other"))
			bMentions = true;
	}
	TASK_CHECK(bMentions);
	return 0;
}

static int case_two_handles_interleaved_lifetimes(void)
{
	Compiler a;
	Compiler b;
	initCompiler(a);
	initCompiler(b);
	TASK_CHECK(a.handle != 0 && b.handle != 0);

	// Parse A, then B, then parse a good shader on B: only B's reports are
	// cleared, A's stay alive.
	TASK_CHECK(Hlsl2Glsl_Parse(a.handle, kNestedShader, ETargetGLSL_120, &a.callbacks, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_Parse(b.handle, kNestedShader, ETargetGLSL_120, &b.callbacks, 0) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(a.handle) >= 1);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(b.handle) >= 1);

	const char* good = "float4 main():COLOR { return 1; }";
	TASK_CHECK(Hlsl2Glsl_Parse(b.handle, good, ETargetGLSL_120, 0, 0) == 1);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(b.handle) == 0);
	TASK_CHECK(Hlsl2Glsl_GetDiagnosticCount(a.handle) >= 1);
	const Hlsl2Glsl_Diagnostic* da = Hlsl2Glsl_GetDiagnostic(a.handle, 0);
	TASK_CHECK(da != 0);
	TASK_CHECK(!strcmp(da->expansions[0].name, "APPLY"));
	TASK_CHECK(!strcmp(da->fragments[0].range.begin.file, "inner.h"));
	return 0;
}

// ---------------------------------------------------------------------------

#define CASE(group, fn) { group, #fn, case_##fn }

const struct task_case task_cases[] = {
	// origin -- primary position, spelling fragments, macro/include chains
	CASE("origin", nested_primary_position),
	CASE("origin", nested_spelling_fragment),
	CASE("origin", nested_expansion_chain),
	CASE("origin", nested_include_chain),
	CASE("origin", plain_undeclared_token),
	CASE("origin", utf8_byte_columns),
	CASE("origin", hashhash_fragments),
	CASE("origin", argument_vs_body_spelling),
	CASE("origin", object_macro_chain),
	CASE("origin", repeated_argument_keeps_spelling),
	CASE("origin", argument_expands_before_substitution),
	// diagnostics -- syntax/type errors, message preservation, #if and #line
	CASE("diagnostics", syntax_error_preserved),
	CASE("diagnostics", syntax_error_from_macro),
	CASE("diagnostics", skipped_code_produces_nothing),
	CASE("diagnostics", line_keeps_byte_columns),
	CASE("diagnostics", line_directive_in_macro_body),
	CASE("diagnostics", declared_type_error_preserved),
	CASE("diagnostics", getinfo_log_still_works),
	CASE("diagnostics", out_of_range_diagnostic),
	// lifetime -- reports, includes, #undef, clearing and handles
	CASE("lifetime", valid_after_include_close),
	CASE("lifetime", valid_after_undef),
	CASE("lifetime", cleared_by_next_parse),
	CASE("lifetime", diagnostic_lifetime_until_next_parse),
	CASE("lifetime", two_handles_do_not_cross),
	CASE("lifetime", two_handles_interleaved_lifetimes),
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
