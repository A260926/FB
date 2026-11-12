/*
 * capture_values.c - tests for the resolved-style capture entry point.
 *
 * Each scenario below is one independent assertion group and increments the
 * scenario counter once. On success the program prints:
 *
 *     CAPTURE_VALUES_OK cases=N
 */

#include "capture_values.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_cases = 0;
static int g_failures = 0;

#define CHECK(cond, message)                                             \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL: %s (line %d)\n", (message), __LINE__);         \
            g_failures++;                                                \
        }                                                                \
    } while (0)

#define BEGIN_CASE() (g_cases++)

static KatanaResolvedStyle* resolve(const char* css,
                                    const KatanaResolvedStyle* parent) {
    return katana_resolve_style(css, strlen(css), parent);
}

static int url_is(const KatanaArray* urls, unsigned index,
                  const char* expected) {
    return urls != NULL && index < urls->length &&
           strcmp((const char*)urls->data[index], expected) == 0;
}

static int urls_count(const KatanaArray* urls, unsigned expected) {
    return urls != NULL && urls->length == expected;
}

/* 1. Cascade priority: !important, last-wins, name case rules. */
static void test_priority(void) {
    BEGIN_CASE();
    KatanaResolvedStyle* s = resolve(
        "color: red; color: blue !important; color: green;"
        "MARGIN: 1px; margin: 2px;"
        "--Hero: url(upper.png); --hero: url(lower.png);"
        "background: var(--Hero, url(fb.png));",
        NULL);

    CHECK(strcmp(katana_resolved_value(s, "color"), "blue") == 0,
          "important declaration beats later normal ones");
    CHECK(strcmp(katana_resolved_value(s, "MARGIN"), "2px") == 0,
          "normal property names are ASCII case-insensitive, last wins");
    const KatanaArray* bg = katana_resolved_urls(s, "background");
    CHECK(urls_count(bg, 1) && url_is(bg, 0, "upper.png"),
          "custom property names are case-sensitive");

    /* An !important custom property wins the cascade but is invalid at
     * computed-value time, so the earlier normal declaration is not used. */
    KatanaResolvedStyle* imp = resolve(
        "--v: url(earlier.png);"
        "--v: url(later.png) !important;"
        "p: var(--v, url(fb.png));",
        NULL);
    const KatanaArray* pu = katana_resolved_urls(imp, "p");
    CHECK(urls_count(pu, 1) && url_is(pu, 0, "fb.png"),
          "!important custom property is computed-invalid, fallback used");
    katana_resolved_style_destroy(imp);
    katana_resolved_style_destroy(s);
}

/* 2. Dependency cycles, fallback references and surviving side branches. */
static void test_cycles(void) {
    BEGIN_CASE();
    KatanaResolvedStyle* s = resolve(
        "--a: var(--b, url(x.png));"
        "--b: var(--a);"
        "--c: url(c.png);"
        "background: var(--a, url(ok.png));"
        "side: var(--c);"
        "cycled: var(--a, url(f.png));",
        NULL);

    const KatanaArray* bg = katana_resolved_urls(s, "background");
    CHECK(urls_count(bg, 1) && url_is(bg, 0, "ok.png"),
          "cyclic variables fall back, spec example yields ok.png");
    const KatanaArray* side = katana_resolved_urls(s, "side");
    CHECK(urls_count(side, 1) && url_is(side, 0, "c.png"),
          "a side branch outside the cycle still resolves");
    const KatanaArray* cy = katana_resolved_urls(s, "cycled");
    CHECK(urls_count(cy, 1) && url_is(cy, 0, "f.png"),
          "every variable on the cycle is invalid");

    /* Fallback references participate in cycle detection. */
    KatanaResolvedStyle* t = resolve(
        "--p: var(--q);"
        "--q: var(--p, url(z.png));"
        "color: var(--p, red);",
        NULL);
    CHECK(strcmp(katana_resolved_value(t, "color"), "red") == 0,
          "a reference inside a fallback still counts for the cycle");
    katana_resolved_style_destroy(t);

    /* A cycle hidden one level deeper, inside a nested fallback. */
    KatanaResolvedStyle* u2 = resolve(
        "--a: var(--missing, var(--b, url(na.png)));"
        "--b: var(--a);"
        "color: var(--a, green);",
        NULL);
    CHECK(strcmp(katana_resolved_value(u2, "color"), "green") == 0,
          "references nested in fallbacks still participate in the cycle");

    /* A malformed var() (junk after the name) is computed-invalid. */
    KatanaResolvedStyle* u3 = resolve("v: var(--x 12px, url(no.png));", NULL);
    CHECK(katana_resolved_value(u3, "v") == NULL &&
              katana_resolved_urls(u3, "v") == NULL,
          "junk after the var() name makes it invalid at computed time");
    katana_resolved_style_destroy(u3);
    katana_resolved_style_destroy(u2);
    katana_resolved_style_destroy(s);
}

/* 3. Inheritance: only custom properties, frozen by value, self-owned. */
static void test_inheritance(void) {
    BEGIN_CASE();
    KatanaResolvedStyle* parent = resolve(
        "--b: url(b.png);"
        "--a: var(--b);"
        "color: red;",
        NULL);
    KatanaResolvedStyle* child = resolve(
        "--b: url(c.png);"
        "background-image: var(--a);"
        "background: var(--b);",
        parent);

    const KatanaArray* img =
        katana_resolved_urls(child, "background-image");
    CHECK(urls_count(img, 1) && url_is(img, 0, "b.png"),
          "inherited --a stays frozen at the parent's expansion");
    const KatanaArray* bg = katana_resolved_urls(child, "background");
    CHECK(urls_count(bg, 1) && url_is(bg, 0, "c.png"),
          "the child's own variable shadows the inherited one");
    CHECK(katana_resolved_value(child, "color") == NULL,
          "normal properties are not inherited");

    /* The child owns its memory: destroying the parent first is safe. */
    katana_resolved_style_destroy(parent);
    img = katana_resolved_urls(child, "background-image");
    CHECK(urls_count(img, 1) && url_is(img, 0, "b.png"),
          "child survives parent destruction");

    /* Inheritance is transitive: a grandchild sees the same frozen value,
     * and a new variable in the middle level cannot unfreeze it. */
    KatanaResolvedStyle* grandparent = resolve(
        "--b: url(g.png); --a: var(--b);", NULL);
    KatanaResolvedStyle* middle = resolve("--b: url(m.png);", grandparent);
    KatanaResolvedStyle* grandchild = resolve("g: var(--a);", middle);
    const KatanaArray* gu = katana_resolved_urls(grandchild, "g");
    CHECK(urls_count(gu, 1) && url_is(gu, 0, "g.png"),
          "frozen expansion propagates through a grandchild");
    katana_resolved_style_destroy(grandparent);
    gu = katana_resolved_urls(grandchild, "g");
    CHECK(urls_count(gu, 1) && url_is(gu, 0, "g.png"),
          "grandchild outlives every ancestor");
    katana_resolved_style_destroy(grandchild);
    katana_resolved_style_destroy(middle);
    katana_resolved_style_destroy(child);

    /* inherit / unset take the parent's frozen value; initial is invalid. */
    KatanaResolvedStyle* p2 = resolve("--x: url(x.png);", NULL);
    KatanaResolvedStyle* kid2 = resolve(
        "--x: inherit;"
        "--y: initial;"
        "--z: unset;"
        "a: var(--x);"
        "b: var(--y, url(q.png));"
        "c: var(--z, url(z.png));",
        p2);
    const KatanaArray* a = katana_resolved_urls(kid2, "a");
    CHECK(urls_count(a, 1) && url_is(a, 0, "x.png"),
          "inherit takes the parent's frozen value");
    const KatanaArray* b = katana_resolved_urls(kid2, "b");
    CHECK(urls_count(b, 1) && url_is(b, 0, "q.png"),
          "initial on a custom property is guaranteed-invalid, fallback used");
    const KatanaArray* c = katana_resolved_urls(kid2, "c");
    CHECK(urls_count(c, 1) && url_is(c, 0, "z.png"),
          "unset with no parent value falls back");
    katana_resolved_style_destroy(kid2);
    katana_resolved_style_destroy(p2);
}

/* 4. Legal empty values: empty custom, empty fallback, empty property. */
static void test_empty_values(void) {
    BEGIN_CASE();
    KatanaResolvedStyle* s = resolve(
        "--e: ;"
        "color: var(--e, green);"
        "gap: var(--missing,);"
        "blank: ;"
        "width: var(--e);",
        NULL);

    const char* color = katana_resolved_value(s, "color");
    CHECK(color != NULL && color[0] == '\0',
          "an existing empty variable wins over its fallback, giving ''");
    const char* gap = katana_resolved_value(s, "gap");
    CHECK(gap != NULL && gap[0] == '\0',
          "an empty fallback is a legal value");
    const char* blank = katana_resolved_value(s, "blank");
    CHECK(blank != NULL && blank[0] == '\0',
          "a declaration with an empty value serializes to ''");
    const KatanaArray* blank_urls = katana_resolved_urls(s, "blank");
    CHECK(blank_urls != NULL && blank_urls->length == 0,
          "valid empty value gives a non-NULL empty URL array");
    const char* width = katana_resolved_value(s, "width");
    CHECK(width != NULL && width[0] == '\0',
          "substitution of an empty variable is valid and empty");
    CHECK(katana_resolved_value(s, "absent") == NULL,
          "a missing property returns NULL, not the empty string");
    CHECK(katana_resolved_urls(s, "absent") == NULL,
          "urls for a missing property are NULL");
    katana_resolved_style_destroy(s);
}

/* 5. Escaped URLs are decoded; string contents are not URLs. */
static void test_escaped_urls(void) {
    BEGIN_CASE();
    KatanaResolvedStyle* s = resolve(
        "background-image: url( a\\ b.png );"
        "cursor: url(\"c d\\.png\");"
        "content: \"url(fake.png)\";"
        "src: url(\\61 b.png);",
        NULL);

    const KatanaArray* img = katana_resolved_urls(s, "background-image");
    CHECK(urls_count(img, 1) && url_is(img, 0, "a b.png"),
          "unquoted url() escapes are decoded");
    const KatanaArray* cur = katana_resolved_urls(s, "cursor");
    CHECK(urls_count(cur, 1) && url_is(cur, 0, "c d.png"),
          "quoted url() argument escapes are decoded");
    const KatanaArray* content = katana_resolved_urls(s, "content");
    CHECK(content != NULL && content->length == 0,
          "url(...) text inside a string is not a resource URL");
    const KatanaArray* src = katana_resolved_urls(s, "src");
    CHECK(urls_count(src, 1) && url_is(src, 0, "ab.png"),
          "hex escapes in url() are decoded");
    katana_resolved_style_destroy(s);
}

/* 6. Comments, and separators nested inside parentheses. */
static void test_comments_and_nesting(void) {
    BEGIN_CASE();
    KatanaResolvedStyle* s = resolve(
        "--g: linear-gradient(rgba(0, 0, 0, .5), url(in.png)) /* x;y */;"
        "background: var(--g);"
        "list: var(--missing, url(a.png), url(b.png), url(a.png));",
        NULL);

    const KatanaArray* bg = katana_resolved_urls(s, "background");
    CHECK(urls_count(bg, 1) && url_is(bg, 0, "in.png"),
          "semicolons/commas inside parentheses do not split declarations");
    const KatanaArray* list = katana_resolved_urls(s, "list");
    CHECK(urls_count(list, 3) && url_is(list, 0, "a.png") &&
              url_is(list, 1, "b.png") && url_is(list, 2, "a.png"),
          "fallback runs past the first comma; order and duplicates kept");
    katana_resolved_style_destroy(s);
}

/* 7. Plain url() control: computed-invalid winners and syntax discards. */
static void test_plain_url_controls(void) {
    BEGIN_CASE();
    KatanaResolvedStyle* s = resolve(
        "background-image: url(old.png);"
        "background-image: var(--missing);",
        NULL);
    CHECK(katana_resolved_value(s, "background-image") == NULL,
          "a computed-invalid winner makes the property invalid");
    CHECK(katana_resolved_urls(s, "background-image") == NULL,
          "old.png must not be downloadable after the later invalid win");

    KatanaResolvedStyle* t = resolve(
        "bogus; color: red;"
        "width: var(--n)px;"
        "--n: 10;",
        NULL);
    CHECK(strcmp(katana_resolved_value(t, "color"), "red") == 0,
          "a syntactically discarded declaration does not affect later ones");
    const char* width = katana_resolved_value(t, "width");
    CHECK(width && strcmp(width, "10 px") == 0,
          "substitution stays at token level: no new dimension token");
    const KatanaArray* color_urls = katana_resolved_urls(t, "color");
    CHECK(color_urls != NULL && color_urls->length == 0,
          "a valid value without URLs gives an empty non-NULL array");
    katana_resolved_style_destroy(t);
    katana_resolved_style_destroy(s);
}

/* 8. The headline hero-variable example. */
static void test_hero_example(void) {
    BEGIN_CASE();
    KatanaResolvedStyle* s = resolve(
        "--hero: url(a.png);"
        "background-image: var(--hero, url(b.png));",
        NULL);
    const KatanaArray* urls = katana_resolved_urls(s, "background-image");
    CHECK(urls_count(urls, 1) && url_is(urls, 0, "a.png"),
          "the variable value wins, the fallback is never downloaded");
    CHECK(katana_resolved_value(s, "--hero") == NULL,
          "custom properties are not exposed through the value query");
    katana_resolved_style_destroy(s);
}

int main(void) {
    test_priority();
    test_cycles();
    test_inheritance();
    test_empty_values();
    test_escaped_urls();
    test_comments_and_nesting();
    test_plain_url_controls();
    test_hero_example();

    if (g_failures) {
        printf("CAPTURE_VALUES_FAILURES=%d cases=%d\n", g_failures, g_cases);
        return 1;
    }
    printf("CAPTURE_VALUES_OK cases=%d\n", g_cases);
    return 0;
}
