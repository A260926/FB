# Katana - A pure-C CSS parser.

Katana is an implementation of the [CSS][] \(Cascading Style Sheet\) parsing algorithm implemented as a pure C library with no outside dependencies. It's designed to serve as a building block for other tools and libraries such as linters, validators, templating languages, and refactoring and analysis tools.

Katana is inspired by [Gumbo][], so it has some goals and features same as Gumbo.

Goals & features:

* Simple API that can be easily wrapped by other languages.
* Relatively lightweight, with no outside dependencies.
* Support for fragment parsing.

Non-goals:

* Mutability. Katana is intentionally designed to turn a style sheet into a parse tree, and free that parse tree all at once. It's not designed to persistently store nodes or subtrees outside of the parse tree, or to perform arbitrary style mutations within your program. If you need this functionality, we recommend translating the Katana parse tree into a mutable style representation more suited for the particular needs of your program before operating on it.

Wishlist:

* Fully conformant with the [CSS-syntax][].
* Hackable dump or print.
* Robust and resilient to bad input.
* Full-featured error reporting.
* Additional performance improvements.
* Tested on [Official W3C Test Suites][].

Installation
============

To build and install the library, issue the standard UNIX incantation from the root of the distribution:

```bash
$ ./autogen.sh
$ ./configure CFLAGS="-std=c99"
$ make
$ sudo make install
```

Katana comes with full pkg-config support, so you can use the pkg-config to print the flags needed to link your program against it:

```bash
$ pkg-config --cflags katana         # print compiler flags
$ pkg-config --libs katana           # print linker flags
$ pkg-config --cflags --libs katana  # print both
```

For example:

```bash
$ gcc examples/dump_stylesheet.c `pkg-config --cflags --libs katana` -o dump
```

If package katana was **not found in the pkg-config** search path, perhaps you should add the directory containing `katana.pc` to the `PKG_CONFIG_PATH` environment variable as following:

```bash
$ export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
```

See the pkg-config man page for more info.

Basic Usage
===========

Within your program, you need to include "katana.h" and then issue a call to `katana_parse`:

```C
#include "katana.h"

int main() {
  const char* css = "selector { property: value }";
  KatanaOutput* output = katana_parse(css, strlen(css), KatanaParserModeStylesheet);
  // Do stuff with output, eg. print the input style
  katana_dump_output(output);
  katana_destroy_output(output);
}
```

See the API documentation and sample programs for more details.

Resolved styles and resource capture
====================================

`katana_parse` produces a declaration tree, but it does not compute custom
properties or resolve `var()`. When archiving a page offline you therefore
need a second step that turns the declarations matched against one element
into a resolved style. Include `capture_values.h`:

```C
#include "capture_values.h"

/* Declarations already matched against the element, concatenated in
 * cascade order: lowest priority first. */
const char* css =
    "--hero: url(a.png);"
    "background-image: var(--hero, url(b.png));";

KatanaResolvedStyle* style = katana_resolve_style(css, strlen(css), NULL);

const KatanaArray* urls = katana_resolved_urls(style, "background-image");
/* urls->length == 1, urls->data[0] == "a.png": the fallback is never used. */

katana_resolved_style_destroy(style);
```

Selector matching, network downloads, rendering, base-URL completion and
per-property value grammars are deliberately outside this entry point. It
tokenizes at CSS component-token boundaries (so comments, strings,
escapes, functions and nested parentheses are handled correctly rather
than split on raw `;` or `,`), applies the cascade, computes custom
properties following [css-variables][], and answers two queries:

* `katana_resolved_value(style, property)` — the normalized token text of
  a resolved normal property.
* `katana_resolved_urls(style, property)` — the decoded URL strings of the
  real URL tokens / `url()` functions in that resolved value, in order,
  duplicates kept. A `url(...)` sequence inside a string token is not a
  resource and is ignored.

A parent style may be passed for inheritance. Only its already-computed
custom properties are inherited (frozen by value); normal properties are
never inherited, and a variable redefined in the child cannot re-expand an
expression the parent already computed.

Ownership of the returned data
------------------------------

* Every `KatanaResolvedStyle` owns all of its memory. The strings returned
  by `katana_resolved_value` and the `KatanaArray` (and its strings)
  returned by `katana_resolved_urls` are borrowed from the style and must
  not be freed individually; they become invalid when
  `katana_resolved_style_destroy` is called.
* A child style deep-copies the custom properties it inherits, so it keeps
  working after the parent has been destroyed.
* The input CSS text and the parent style are only read: the input buffer
  may be reused or freed as soon as `katana_resolve_style` returns.

Return-value conventions:

* Value query: the empty string means a legal empty value; `NULL` means the
  property is absent or its winning declaration is computed-invalid.
* URL query: a non-`NULL` (possibly empty) array means the property
  resolved; `NULL` means it is absent or computed-invalid.

"Computed invalid" vs. "syntax discard"
---------------------------------------

These are distinct failure modes and they interact with the cascade
differently:

* **Syntax discard** — the text is not a well-formed declaration (for
  example a stray token where a property name is expected). It is dropped
  while parsing, exactly as the CSS parser would drop it, and an earlier
  valid declaration of the same property may still win.
* **Computed invalid** — the declaration is well-formed and wins the
  cascade, but becomes invalid at computed-value time when `var()` is
  substituted: a referenced variable is missing without a usable fallback,
  participates in a dependency cycle (references inside fallbacks count),
  or is itself invalid, or a custom property is explicitly set to
  `initial`. Such a declaration still wins, so an earlier declaration of
  the same property is **not** revisited, and both queries return `NULL`.

For example, `background-image: url(old.png); background-image: var(--missing)`
resolves to an invalid property: `old.png` must not be downloaded.

`examples/capture_values.c` is a small command-line demonstration, and
`tests/capture_values.c` exercises priority, cycles and side branches,
frozen inheritance, legal empty values, escaped URLs, comments, nested
separators and plain `url()` controls.

Contributing
===========
Bug reports are very much welcome.  Please use GitHub's issue-tracking feature, as it makes it easier to keep track of bugs and makes it possible for other project watchers to view the existing issues.

Patches and pull requests are also welcome.

If you're unwilling to do this, it would be most helpful if you could file bug reports that include detailed prose about where in the code the error is and how to fix it, but leave out exact source code.



[CSS]: http://www.w3.org/Style/CSS/current-work
[CSS-syntax]: http://www.w3.org/TR/css3-syntax
[css-variables]: https://www.w3.org/TR/css-variables-1/
[Gumbo]: https://github.com/google/gumbo-parser
[reworkcss/css]: https://github.com/reworkcss/css
[Official W3C Test Suites]: http://www.w3.org/Style/CSS/Test/
[semantic versioning]: http://semver.org/
