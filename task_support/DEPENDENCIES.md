The source snapshot includes the production modules below. Builds use `-mod=vendor`
and do not install or download modules. Go 1.23 or newer is required; the target
JSON-event checker also uses Node.js (standard `node:fs`, no npm packages).

| Module | Version | License file in this snapshot |
| --- | --- | --- |
| github.com/PuerkitoBio/goquery | v1.10.3 | vendor/github.com/PuerkitoBio/goquery/LICENSE |
| github.com/andybalholm/cascadia | v1.3.3 | vendor/github.com/andybalholm/cascadia/LICENSE |
| golang.org/x/net | v0.39.0 | vendor/golang.org/x/net/LICENSE |

`internal/testassert` is new MIT-licensed local test support; its LICENSE is in
that directory. It uses only Go standard-library reflection and string matching.
Existing test assertion operands and test functions are preserved. Assertions
record an error on the explicit test object and continue; nil checks distinguish
a nil interface from an interface holding a typed nil pointer.
