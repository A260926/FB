// Fixture compiled with full debug info (/Zi /Od) so its PDB contains private
// symbols: S_GPROC32(_ID) records (function sizes), an LF_ARGLIST per function
// (argument counts/types), and full LF_STRUCTURE/LF_FIELDLIST layouts for the
// types below. The suite parses THIS module's PDB to validate the size,
// argument, and struct-layout resolution that a stripped public PDB can't
// exercise.
//
// Functions are exported (extern "C" => undecorated x64 names matching the
// public-symbol table) with deliberately distinct, known argument arities.
#include <cstdint>

#if defined(_WIN32)
#define FIX extern "C" __declspec(dllexport) __declspec(noinline)
#define FIX_DATA __declspec(dllexport)
#else
#define FIX extern "C" __attribute__((visibility("default"))) __attribute__((noinline))
#define FIX_DATA __attribute__((visibility("default")))
#endif

FIX int      fixture_add(int a, int b)                       { return a + b; }
FIX int      fixture_three(int a, int b, int c)              { return a + b + c; }
FIX long     fixture_five(int a, int b, int c, int d, int e) { return a + b + c + d + e; }
FIX int      fixture_none()                                  { return 42; }
FIX double   fixture_mixed(int a, double b, char c)          { return a + b + c; }
FIX uint64_t fixture_ptr(const char* s, uint32_t n)          { return (uint64_t)s + n; }

// --- Structs with known x64 layouts ----------------------------------------
// Known offsets/size: x@0, y@4, z@8; total 16.
struct FixturePoint
{
    int    x;
    int    y;
    double z;
};

// Known offsets/size: tag@0, ptr@8, id@16, arr@24; total 40.
struct FixtureMix
{
    char     tag;
    void*    ptr;
    uint64_t id;
    int      arr[4];
};

// Dereferencing the members forces the compiler to emit the *full* type
// definition (a field list), not just a forward reference.
FIX double   fixture_use_point(FixturePoint* p) { return p->x + p->y + p->z; }
FIX uint64_t fixture_use_mix(FixtureMix* m)
{
    return (uint64_t)m->tag + (uint64_t)m->ptr + m->id + (uint64_t)m->arr[0];
}

// --- Symbol-category coverage ----------------------------------------------
// A polymorphic class makes the compiler emit a vftable public symbol
// (??_7FixtureShape@@6B@) and, with /GR, RTTI symbols (??_R0.. etc).
// Virtual functions are defined out-of-line so the vtable lands in THIS module.
struct FixtureShape
{
    virtual int  area() const;
    virtual ~FixtureShape();
    int tag;
};
int  FixtureShape::area() const { return tag; }
FixtureShape::~FixtureShape() {}

// A C++ (not extern "C") const global -> decorated name ?...@@3?B => Constant.
FIX_DATA extern const unsigned g_FixtureConst = 0xC0FFEEu;

// Returning a string literal emits a ??_C@... StringLiteral public symbol.
FIX const char* fixture_str() { return "MemPDB fixture string literal"; }

// Construct the polymorphic type so its vtable/RTTI are referenced and emitted.
FIX FixtureShape* fixture_make_shape()
{
    FixtureShape* s = new FixtureShape();
    s->tag = (int)g_FixtureConst;
    return s;
}

// Reference everything so /OPT:REF cannot strip any of it.
extern "C" {
FIX_DATA const void* fixture_table[] = {
    (void*)&fixture_add,       (void*)&fixture_three, (void*)&fixture_five,
    (void*)&fixture_none,      (void*)&fixture_mixed, (void*)&fixture_ptr,
    (void*)&fixture_use_point, (void*)&fixture_use_mix,
    (void*)&fixture_str,       (void*)&fixture_make_shape, (const void*)&g_FixtureConst,
};
}
