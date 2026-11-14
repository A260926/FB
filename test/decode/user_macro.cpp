/*
 * Tests for user-defined macro support (ARIB STD-B24 MACRO command).
 *
 * Registered through task_support/check.h and driven by task_support/driver.cpp.
 * All checks go through the public C++ / C decoder APIs with self-constructed PES data.
 */

#include <cstdint>
#include <cstring>
#include <vector>

#include "aribcaption/context.hpp"
#include "aribcaption/decoder.hpp"
#include "aribcaption/context.h"
#include "aribcaption/decoder.h"
#include "aribcaption/caption.h"

#include "task_support/check.h"
#include "task_support/pes.hpp"

namespace {

using namespace aribcaption;

// Control codes used in the test streams
constexpr uint8_t kCS   = 0x0C;  // Clear Screen
constexpr uint8_t kLS1  = 0x0E;  // Locking shift 1 (GL <- G1)
constexpr uint8_t kESC  = 0x1B;
constexpr uint8_t kAPS  = 0x1C;  // Active Position Set
constexpr uint8_t kSS3  = 0x1D;
constexpr uint8_t kRDF  = 0x81;  // Red foreground
constexpr uint8_t kMSZ  = 0x89;  // Middle size
constexpr uint8_t kMACRO = 0x95;

// Decode one PES containing the given statement data unit bodies
DecodeStatus DecodeBodies(Decoder& dec,
                          const std::vector<std::vector<uint8_t>>& bodies,
                          int64_t pts,
                          DecodeResult& out) {
    std::vector<uint8_t> pes = task_support::pes(bodies);
    return dec.Decode(pes.data(), pes.size(), pts, out);
}

// U+FF21 FULLWIDTH LATIN CAPITAL LETTER A (Alphanumeric set, normal size)
const char kFullwidthA[] = "\xEF\xBC\xA1";
// U+3061 HIRAGANA LETTER TI (Hiragana set 0x41)
const char kHiraganaTi[] = "\xE3\x81\xA1";

// ---------------------------------------------------------------------------
// definitions group
// ---------------------------------------------------------------------------

// MACRO 0x40 defines silently; MACRO 0x41 defines and executes immediately
int def_define_vs_immediate_execute() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    // MSZ, APS(0,0) [APS coordinates sit in the low 6 bits: 0x40 => 0], LS1, 'X',
    // then define macro 0x21 = {MSZ LS1 'A'}, then 'Y'
    TASK_CHECK(DecodeBodies(dec, {{kMSZ, kAPS, 0x40, 0x40, kLS1, 0x58,
                                   kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F,
                                   0x59}}, 1000, r) == DecodeStatus::kGotCaption);
    // The definition itself must not produce text nor move the cursor
    TASK_CHECK(r.caption && r.caption->text == "XY");
    TASK_CHECK(r.caption->regions.size() == 1);
    TASK_CHECK(r.caption->regions[0].chars.size() == 2);
    TASK_CHECK(r.caption->regions[0].chars[0].x == 0);
    TASK_CHECK(r.caption->regions[0].chars[1].x == 20);  // MSZ section width

    // The stored macro can be invoked afterwards through SS3 (G3 = Macro)
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 2000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "A");

    // MACRO 0x41: define macro 0x22 and execute it right after the commit
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x41, 0x22, kMSZ, kLS1, 0x42, kMACRO, 0x4F}},
                            3000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "B");

    // ... and the defined macro stays usable
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x22}}, 4000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "B");
    return 0;
}

// A definition may span data units; a new define command implicitly ends the previous one
int def_cross_unit_and_implicit_end() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    // Macro 0x21 body is split across two data units; MACRO 0x40 0x22 inside the
    // second unit implicitly commits 0x21 and starts defining 0x22
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kMSZ, kLS1},
                                  {0x41, kMACRO, 0x40, 0x22, kMSZ},
                                  {kLS1, 0x42, kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(!r.caption);

    // Both committed macros are invocable from later Decode() calls
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 2000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "A");
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x22}}, 3000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "B");
    return 0;
}

// A user-defined (even empty) macro takes precedence over the 16 default macros
int def_empty_overrides_default() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    // Default macro 0x6F redesignates G0 to Alphanumeric and locking-shifts GL to G0,
    // so the final 0x41 is read as a fullwidth alphanumeric
    TASK_CHECK(DecodeBodies(dec, {{kCS, kESC, 0x6E, kSS3, 0x6F, 0x41}},
                            1000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == kFullwidthA);

    // Define macro 0x6F with an empty body: overrides the default macro
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x6F, kMACRO, 0x4F}},
                            2000, r) == DecodeStatus::kNoCaption);

    // Same stream again: CS restores default G sets, LS2 puts Hiragana into GL,
    // the empty macro changes nothing, so 0x41 is now read as hiragana
    TASK_CHECK(DecodeBodies(dec, {{kCS, kESC, 0x6E, kSS3, 0x6F, 0x41}},
                            3000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == kHiraganaTi);
    return 0;
}

// Malformed definitions are reported as kError and don't clobber committed macros
int def_invalid_definition_errors() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    // A good definition first: macro 0x21 = 'A'
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    // MACRO 0x40 without the macro code byte
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40}}, 2000, r) == DecodeStatus::kError);
    TASK_CHECK(!r.caption);
    // Macro code out of range (valid: 0x21..0x7E)
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x20}}, 3000, r) == DecodeStatus::kError);
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x7F}}, 4000, r) == DecodeStatus::kError);
    // Unknown MACRO parameter
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x42, 0x21}}, 5000, r) == DecodeStatus::kError);
    // MACRO 0x4F without an active definition
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x4F}}, 6000, r) == DecodeStatus::kError);
    // MACRO byte at the very end of the statement body
    TASK_CHECK(DecodeBodies(dec, {{kMACRO}}, 7000, r) == DecodeStatus::kError);
    // Dangling MACRO byte inside a definition: the incomplete item (0x22) is dropped
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x22, kMSZ, kLS1, 0x5A, kMACRO}},
                            8000, r) == DecodeStatus::kError);

    // The previously committed macro survives all these errors
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 9000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "A");
    // ... while the incomplete 0x22 definition was never committed
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x22}}, 10000, r) == DecodeStatus::kNoCaption);
    return 0;
}

// ---------------------------------------------------------------------------
// execution group
// ---------------------------------------------------------------------------

// Macros can be invoked through SS3, GL (LS3) and GR (LS3R) alike
int exe_invoke_via_gl_gr_ss3() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    // MSZ, APS(0,0); SS3 0x21; LS3 then GL 0x21; LS3R then GR 0xA1
    TASK_CHECK(DecodeBodies(dec, {{kMSZ, kAPS, 0x40, 0x40,
                                   kSS3, 0x21,
                                   kESC, 0x6F, 0x21,
                                   kESC, 0x7C, 0xA1}},
                            2000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "AAA");
    TASK_CHECK(r.caption->regions.size() == 1);
    TASK_CHECK(r.caption->regions[0].chars.size() == 3);
    TASK_CHECK(r.caption->regions[0].chars[0].x == 0);
    TASK_CHECK(r.caption->regions[0].chars[1].x == 20);
    TASK_CHECK(r.caption->regions[0].chars[2].x == 40);
    return 0;
}

// The same macro body is interpreted under the G sets active at invocation time
int exe_codeset_state_at_invocation() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    // Macro 0x21 = a single raw GL byte 0x41
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, 0x41, kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    // Invoke with GL = G1 (Alphanumeric): 0x41 becomes a fullwidth 'A'
    TASK_CHECK(DecodeBodies(dec, {{kCS, kLS1, kSS3, 0x21}},
                            2000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == kFullwidthA);

    // Invoke the same macro again with GL = G2 (Hiragana): 0x41 becomes hiragana
    TASK_CHECK(DecodeBodies(dec, {{kCS, kESC, 0x6E, kSS3, 0x21}},
                            3000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == kHiraganaTi);
    return 0;
}

// Codeset / color / position changes made inside a macro leak into the outer statement
int exe_state_changes_leak_to_outer() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    // Macro 0x21 = { RDF, MSZ, LS1, 'A' }
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kRDF, kMSZ, kLS1, 0x41, kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    // APS(0,0) under NSZ, invoke the macro, then emit an outer 0x42
    TASK_CHECK(DecodeBodies(dec, {{kAPS, 0x40, 0x40, kSS3, 0x21, 0x42}},
                            2000, r) == DecodeStatus::kGotCaption);
    // The outer 0x42 is read through G1 (macro's LS1) in MSZ as halfwidth 'B'
    TASK_CHECK(r.caption && r.caption->text == "AB");
    TASK_CHECK(r.caption->regions.size() == 1);
    TASK_CHECK(r.caption->regions[0].chars.size() == 2);
    // The macro's red foreground applies to its own char and leaks to the outer one
    TASK_CHECK(r.caption->regions[0].chars[0].text_color.u32 == ColorRGBA(255, 0, 0, 255).u32);
    TASK_CHECK(r.caption->regions[0].chars[1].text_color.u32 == ColorRGBA(255, 0, 0, 255).u32);
    // The macro advanced the cursor: outer char lands right after the macro's char
    TASK_CHECK(r.caption->regions[0].chars[0].x == 0);
    TASK_CHECK(r.caption->regions[0].chars[1].x == 20);
    TASK_CHECK(r.caption->regions[0].chars[1].codepoint == 0x42);
    return 0;
}

// Macros may call other macros; sequential reuse of one macro is not recursion
int exe_nested_and_sequential_reuse() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    // 0x22 = 'B'; 0x21 = 'A' + invoke(0x22) + 'C'; 0x23 = invoke(0x22) twice in a row
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x22, kMSZ, kLS1, 0x42,
                                   kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kSS3, 0x22, kMSZ, kLS1, 0x43,
                                   kMACRO, 0x40, 0x23, kSS3, 0x22, kSS3, 0x22,
                                   kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 2000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "ABC");

    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x23}}, 3000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "BB");
    return 0;
}

// True invocation cycles are detected through the active chain and reported as errors
int exe_cycle_detection_error() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    // 0x21 calls itself; 0x22 <-> 0x23 form an indirect cycle; 0x24 is a good macro
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kSS3, 0x21,
                                   kMACRO, 0x40, 0x22, kSS3, 0x23,
                                   kMACRO, 0x40, 0x23, kSS3, 0x22,
                                   kMACRO, 0x40, 0x24, kMSZ, kLS1, 0x5A,
                                   kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 2000, r) == DecodeStatus::kError);
    TASK_CHECK(!r.caption);
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x22}}, 3000, r) == DecodeStatus::kError);
    TASK_CHECK(!r.caption);

    // The error clears the active chain; intact definitions remain usable
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x24}}, 4000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "Z");
    return 0;
}

// A truncated character at the end of a macro body cannot borrow the caller's next byte
int exe_trailing_incomplete_char() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    // 0x21 = lone first byte of a 2-byte kanji; 0x22 = dangling SS2; 0x23 = dangling SS3
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, 0x24,
                                   kMACRO, 0x40, 0x22, 0x19,
                                   kMACRO, 0x40, 0x23, kSS3,
                                   kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    // If the decoder borrowed the caller's 0x41, the kanji would complete and succeed
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21, 0x41}}, 2000, r) == DecodeStatus::kError);
    TASK_CHECK(!r.caption);
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x22, 0x41}}, 3000, r) == DecodeStatus::kError);
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x23, 0x41}}, 4000, r) == DecodeStatus::kError);
    return 0;
}

// The outer stream only consumes the invocation itself; expanded chars occupy positions
int exe_position_accounting() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    // Macro 0x21 = two MSZ chars "AA"
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, 0x41, kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    // MSZ, APS(0,0), LS1, 'X', invoke, 'Y'
    TASK_CHECK(DecodeBodies(dec, {{kMSZ, kAPS, 0x40, 0x40, kLS1, 0x58, kSS3, 0x21, 0x59}},
                            2000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "XAAY");
    TASK_CHECK(r.caption->regions.size() == 1);
    TASK_CHECK(r.caption->regions[0].chars.size() == 4);
    TASK_CHECK(r.caption->regions[0].chars[0].x == 0);
    TASK_CHECK(r.caption->regions[0].chars[1].x == 20);
    TASK_CHECK(r.caption->regions[0].chars[2].x == 40);
    TASK_CHECK(r.caption->regions[0].chars[3].x == 60);
    return 0;
}

// ---------------------------------------------------------------------------
// lifecycle group
// ---------------------------------------------------------------------------

// Committed definitions persist across data units and subsequent Decode() calls
int life_persist_across_decodes() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    for (int64_t pts = 2000; pts <= 4000; pts += 1000) {
        TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, pts, r) == DecodeStatus::kGotCaption);
        TASK_CHECK(r.caption && r.caption->text == "A");
    }
    // Two invocations inside one later statement work as well
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21, kSS3, 0x21}}, 5000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "AA");

    // A definition may even span two Decode() calls: a data unit end is not a terminator
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x22, kMSZ, kLS1}}, 6000, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(DecodeBodies(dec, {{0x42, kMACRO, 0x4F}}, 7000, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x22}}, 8000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "B");
    return 0;
}

// CS resets display state but keeps definitions; Flush drops them
int life_cs_vs_flush() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    // Macro 0x21 = red 'A'
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kRDF, kMSZ, kLS1, 0x41, kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    // CS then invoke: the definition survives CS
    TASK_CHECK(DecodeBodies(dec, {{kCS, kSS3, 0x21}}, 2000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "A");
    TASK_CHECK(r.caption->flags & kCaptionFlagsClearScreen);
    TASK_CHECK(r.caption->regions[0].chars[0].text_color.u32 == ColorRGBA(255, 0, 0, 255).u32);

    // CS did reset the display state: a plain char afterwards is default white again
    TASK_CHECK(DecodeBodies(dec, {{kCS, kMSZ, kLS1, 0x42}}, 3000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "B");
    TASK_CHECK(r.caption->regions[0].chars[0].text_color.u32 == ColorRGBA(255, 255, 255, 255).u32);

    // The macro is still defined after all these CS resets
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 4000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "A");

    // Flush drops the definition (invoking an undefined macro is a no-op)
    dec.Flush();
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 5000, r) == DecodeStatus::kNoCaption);
    return 0;
}

// Disabling the switch restores the conventional behavior and drops definitions;
// re-Initialize clears the table and restores the disabled default
int life_disable_and_reinitialize() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    dec.SetUserMacroEnabled(false);
    // Disabled: MACRO keeps the conventional behavior — statement parsing stops there,
    // the partial caption is still delivered, and no error is raised
    TASK_CHECK(DecodeBodies(dec, {{kMSZ, kLS1, 0x58,
                                   kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F,
                                   0x59}},
                            2000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "X");

    // Re-enabling cannot restore the dropped definition
    dec.SetUserMacroEnabled(true);
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 3000, r) == DecodeStatus::kNoCaption);

    // Define it again, then re-Initialize: table cleared and switch back to disabled
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F}},
                            4000, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 5000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "A");

    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 6000, r) == DecodeStatus::kNoCaption);
    // The switch is off again after Initialize: conventional MACRO behavior
    TASK_CHECK(DecodeBodies(dec, {{kMSZ, kLS1, 0x58,
                                   kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F,
                                   0x59}},
                            7000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "X");
    return 0;
}

// An actual encoding scheme change clears the table but keeps the switch;
// repeated same-encoding sets and SetProfile don't
int life_encoding_change_clears() {
    Context ctx;
    Decoder dec(ctx);
    TASK_CHECK(dec.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    dec.SetUserMacroEnabled(true);

    DecodeResult r;
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);

    dec.SetEncodingScheme(EncodingScheme::kARIB_STD_B24_JIS);  // same encoding: kept
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 2000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "A");

    dec.SetProfile(Profile::kProfileA);  // profile change: kept
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 3000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "A");

    // Actual encoding change: table cleared, switch preserved
    dec.SetEncodingScheme(EncodingScheme::kARIB_STD_B24_UTF8);
    dec.SetEncodingScheme(EncodingScheme::kARIB_STD_B24_JIS);
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 4000, r) == DecodeStatus::kNoCaption);

    // The switch is still on: a new definition works without re-enabling
    TASK_CHECK(DecodeBodies(dec, {{kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F}},
                            5000, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(DecodeBodies(dec, {{kSS3, 0x21}}, 6000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "A");
    return 0;
}

// The C API exposes the switch, and macro tables are per-instance
int life_c_api_and_instance_isolation() {
    aribcc_context_t* ctx = aribcc_context_alloc();
    TASK_CHECK(ctx != nullptr);
    aribcc_decoder_t* d1 = aribcc_decoder_alloc(ctx);
    aribcc_decoder_t* d2 = aribcc_decoder_alloc(ctx);
    TASK_CHECK(d1 != nullptr && d2 != nullptr);
    TASK_CHECK(aribcc_decoder_initialize(d1, ARIBCC_ENCODING_SCHEME_ARIB_STD_B24_JIS,
                                         ARIBCC_CAPTIONTYPE_DEFAULT, ARIBCC_PROFILE_DEFAULT,
                                         ARIBCC_LANGUAGEID_DEFAULT));
    TASK_CHECK(aribcc_decoder_initialize(d2, ARIBCC_ENCODING_SCHEME_ARIB_STD_B24_JIS,
                                         ARIBCC_CAPTIONTYPE_DEFAULT, ARIBCC_PROFILE_DEFAULT,
                                         ARIBCC_LANGUAGEID_DEFAULT));
    aribcc_decoder_set_user_macro_enabled(d1, true);
    aribcc_decoder_set_user_macro_enabled(d2, true);

    std::vector<uint8_t> def_a = task_support::pes({{kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F}});
    std::vector<uint8_t> def_b = task_support::pes({{kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x42, kMACRO, 0x4F}});
    std::vector<uint8_t> invoke = task_support::pes({{kSS3, 0x21}});
    aribcc_caption_t cap;

    // Define macro 0x21 = 'A' in d1 only
    TASK_CHECK(aribcc_decoder_decode(d1, def_a.data(), def_a.size(), 1000, &cap) ==
               ARIBCC_DECODE_STATUS_NO_CAPTION);
    TASK_CHECK(aribcc_decoder_decode(d1, invoke.data(), invoke.size(), 2000, &cap) ==
               ARIBCC_DECODE_STATUS_GOT_CAPTION);
    TASK_CHECK(cap.text != nullptr && std::strcmp(cap.text, "A") == 0);
    aribcc_caption_cleanup(&cap);

    // d2 doesn't share the table: 0x21 is undefined there
    TASK_CHECK(aribcc_decoder_decode(d2, invoke.data(), invoke.size(), 3000, &cap) ==
               ARIBCC_DECODE_STATUS_NO_CAPTION);

    // Define 0x21 = 'B' in d2; d1's definition is unaffected
    TASK_CHECK(aribcc_decoder_decode(d2, def_b.data(), def_b.size(), 4000, &cap) ==
               ARIBCC_DECODE_STATUS_NO_CAPTION);
    TASK_CHECK(aribcc_decoder_decode(d2, invoke.data(), invoke.size(), 5000, &cap) ==
               ARIBCC_DECODE_STATUS_GOT_CAPTION);
    TASK_CHECK(cap.text != nullptr && std::strcmp(cap.text, "B") == 0);
    aribcc_caption_cleanup(&cap);
    TASK_CHECK(aribcc_decoder_decode(d1, invoke.data(), invoke.size(), 6000, &cap) ==
               ARIBCC_DECODE_STATUS_GOT_CAPTION);
    TASK_CHECK(cap.text != nullptr && std::strcmp(cap.text, "A") == 0);
    aribcc_caption_cleanup(&cap);

    // A decoder that never enabled the switch keeps the conventional behavior
    aribcc_decoder_t* d3 = aribcc_decoder_alloc(ctx);
    TASK_CHECK(d3 != nullptr);
    TASK_CHECK(aribcc_decoder_initialize(d3, ARIBCC_ENCODING_SCHEME_ARIB_STD_B24_JIS,
                                         ARIBCC_CAPTIONTYPE_DEFAULT, ARIBCC_PROFILE_DEFAULT,
                                         ARIBCC_LANGUAGEID_DEFAULT));
    std::vector<uint8_t> legacy = task_support::pes({{kMSZ, kLS1, 0x58,
                                                      kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F,
                                                      0x59}});
    TASK_CHECK(aribcc_decoder_decode(d3, legacy.data(), legacy.size(), 7000, &cap) ==
               ARIBCC_DECODE_STATUS_GOT_CAPTION);
    TASK_CHECK(cap.text != nullptr && std::strcmp(cap.text, "X") == 0);
    aribcc_caption_cleanup(&cap);

    aribcc_decoder_free(d3);
    aribcc_decoder_free(d2);
    aribcc_decoder_free(d1);
    aribcc_context_free(ctx);

    // Same isolation guarantee for C++ Decoder instances
    Context cxx_ctx;
    Decoder cxx_a(cxx_ctx);
    Decoder cxx_b(cxx_ctx);
    TASK_CHECK(cxx_a.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    TASK_CHECK(cxx_b.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
    cxx_a.SetUserMacroEnabled(true);
    cxx_b.SetUserMacroEnabled(true);

    DecodeResult r;
    TASK_CHECK(DecodeBodies(cxx_a, {{kMACRO, 0x40, 0x21, kMSZ, kLS1, 0x41, kMACRO, 0x4F}},
                            1000, r) == DecodeStatus::kNoCaption);
    TASK_CHECK(DecodeBodies(cxx_a, {{kSS3, 0x21}}, 2000, r) == DecodeStatus::kGotCaption);
    TASK_CHECK(r.caption && r.caption->text == "A");
    TASK_CHECK(DecodeBodies(cxx_b, {{kSS3, 0x21}}, 3000, r) == DecodeStatus::kNoCaption);
    return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"definitions", "define_vs_immediate_execute", def_define_vs_immediate_execute},
    {"definitions", "cross_unit_and_implicit_end", def_cross_unit_and_implicit_end},
    {"definitions", "empty_overrides_default", def_empty_overrides_default},
    {"definitions", "invalid_definition_errors", def_invalid_definition_errors},
    {"execution", "invoke_via_gl_gr_ss3", exe_invoke_via_gl_gr_ss3},
    {"execution", "codeset_state_at_invocation", exe_codeset_state_at_invocation},
    {"execution", "state_changes_leak_to_outer", exe_state_changes_leak_to_outer},
    {"execution", "nested_and_sequential_reuse", exe_nested_and_sequential_reuse},
    {"execution", "cycle_detection_error", exe_cycle_detection_error},
    {"execution", "trailing_incomplete_char", exe_trailing_incomplete_char},
    {"execution", "position_accounting", exe_position_accounting},
    {"lifecycle", "persist_across_decodes", life_persist_across_decodes},
    {"lifecycle", "cs_vs_flush", life_cs_vs_flush},
    {"lifecycle", "disable_and_reinitialize", life_disable_and_reinitialize},
    {"lifecycle", "encoding_change_clears", life_encoding_change_clears},
    {"lifecycle", "c_api_and_instance_isolation", life_c_api_and_instance_isolation},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
