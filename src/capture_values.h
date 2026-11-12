/**
 * capture_values.h
 *
 * Offline-archive style resolution entry point.
 *
 * The caller has already matched a set of CSS declarations against one
 * element and concatenated them, in cascade order (lowest priority first),
 * into a single declaration list. This module turns that text into a
 * resolved style: it computes custom properties (CSS variables), performs
 * var() substitution on normal properties, and answers per-property value
 * and resource-URL queries.
 *
 * Selector matching, network access, page rendering and per-property value
 * grammars are all out of scope here. In particular the URL list never
 * performs any address resolution (no base-URL joining).
 *
 * Ownership
 * ---------
 * Every KatanaResolvedStyle owns all of its memory, including the strings
 * and KatanaArrays returned by katana_resolved_value() and
 * katana_resolved_urls(). A child style deep-copies the custom properties it
 * inherits from its parent, so destroying the parent first never invalidates
 * a surviving child. Destroy a style with katana_resolved_style_destroy();
 * do not free individual returned strings or arrays yourself.
 *
 * The input CSS text and the parent style are only read; neither is ever
 * modified or retained by pointer (all needed data is copied).
 *
 * Computed-invalid vs. syntactically discarded
 * --------------------------------------------
 * A declaration can fail in two distinct ways:
 *
 *  - "Syntax discard": the declaration text is not a well-formed
 *    declaration (bad property name, stray tokens, ...). It is dropped while
 *    the input is parsed, exactly as the CSS parser would drop it, and an
 *    earlier valid declaration of the same property may still win.
 *
 *  - "Computed invalid" (guaranteed-invalid at computed-value time, per
 *    https://www.w3.org/TR/css-variables-1/): the declaration itself is
 *    well-formed but its var() substitution fails (a referenced variable is
 *    missing without a usable fallback, participates in a dependency cycle,
 *    or is itself invalid), or a custom property is explicitly set to
 *    `initial`. The declaration still wins the cascade, so an earlier
 *    declaration of the same property is NOT revisited; queries for it
 *    return NULL.
 */

#ifndef __Katana__capture_values__
#define __Katana__capture_values__

#include "katana.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque resolved style. */
typedef struct KatanaResolvedStyleInternal KatanaResolvedStyle;

/**
 * Resolve a declaration list.
 *
 * @param css    Declarations concatenated in cascade order, lowest priority
 *               first, e.g. "color: red; --hero: url(a.png);".
 * @param len    Length of css.
 * @param parent Parent style, or NULL. Only the parent's already-computed
 *               custom properties are inherited (frozen by value). Normal
 *               properties are never inherited, and variables newly defined
 *               in the child cannot re-expand expressions stored in the
 *               parent.
 *
 * Cascade rules applied within css:
 *  - normal property names are matched case-insensitively (ASCII);
 *  - custom property (--...) names are matched case-sensitively;
 *  - for equal names, an !important declaration beats a normal one, and
 *    among declarations of the same importance the last one wins.
 *
 * @return A newly allocated style (never NULL; allocation failure aborts,
 *         matching the rest of the library), to be freed with
 *         katana_resolved_style_destroy().
 */
KatanaResolvedStyle* katana_resolve_style(const char* css, size_t len,
                                          const KatanaResolvedStyle* parent);

/**
 * Serialized, normalized token text of a resolved normal property.
 *
 * @return Borrowed NUL-terminated string owned by the style. The empty
 *         string is returned for a valid empty value; NULL means the
 *         property is absent or its winning declaration became
 *         computed-invalid. Custom properties are not exposed here.
 */
const char* katana_resolved_value(const KatanaResolvedStyle* style,
                                  const char* property);

/**
 * Decoded resource URLs referenced by a resolved normal property.
 *
 * Only real URL tokens / url() functions of the resolved (substituted)
 * value are collected; a url(...) sequence inside a string token is not.
 * URLs are returned in order of appearance and duplicates are kept.
 *
 * @return A borrowed, non-NULL but possibly empty KatanaArray whose elements
 *         are borrowed NUL-terminated char* strings (decoded, never
 *         base-URL-resolved), all owned by the style. NULL means the
 *         property is absent or its winning declaration became
 *         computed-invalid (distinguish that from a valid value that simply
 *         contains no URL, which returns an empty array).
 */
const KatanaArray* katana_resolved_urls(const KatanaResolvedStyle* style,
                                        const char* property);

/**
 * Free a resolved style and every value and array it handed out.
 */
void katana_resolved_style_destroy(KatanaResolvedStyle* style);

#ifdef __cplusplus
}
#endif

#endif /* defined(__Katana__capture_values__) */
