package cookiejar_test

import (
	"fmt"
	"net/http"
	"net/url"
	"time"

	cookiejar "github.com/juju/persistent-cookiejar"
)

// ExampleCookiesWithPartition demonstrates an offline replay of a page
// from two different top-level sites that each embed the same third-party
// interface. Both sites set a Partitioned cookie with the same name under
// the same request host; the two sessions stay isolated because the
// top-level site is passed alongside the request URL on every call.
func ExampleJar_CookiesWithPartition() {
	jar, err := cookiejar.New(&cookiejar.Options{NoPersist: true})
	if err != nil {
		panic(err)
	}

	// The third-party request is the same for both pages.
	api := mustURL("https://api.thirdparty.example/collect")
	// Each top-level site identifies the partition: scheme plus
	// registrable domain. Ports, paths and fragments do not matter.
	siteA := mustURL("https://www.publisher-a.example:8443/article?id=1")
	siteB := mustURL("https://publisher-b.example/other")
	// A different port on the same site is the same partition.
	siteASame := mustURL("https://www.publisher-a.example:9443/section")

	expires := time.Now().Add(24 * time.Hour)
	err = jar.SetCookiesWithPartition(api, siteA, []*http.Cookie{{
		Name:   "session",
		Value:  "for-a",
		Path:   "/",
		Secure: true,
		// Partitioned cookies must be Secure and set over HTTPS.
		Partitioned: true,
		Expires:     expires,
	}})
	if err != nil {
		panic(err)
	}
	err = jar.SetCookiesWithPartition(api, siteB, []*http.Cookie{{
		Name:        "session",
		Value:       "for-b",
		Path:        "/",
		Secure:      true,
		Partitioned: true,
		Expires:     expires,
	}})
	if err != nil {
		panic(err)
	}

	// A plain (unpartitioned) cookie is visible everywhere.
	jar.SetCookies(api, []*http.Cookie{{
		Name:    "client",
		Value:   "shared",
		Path:    "/",
		Expires: expires,
	}})

	for _, top := range []*url.URL{siteA, siteB, siteASame} {
		cookies, err := jar.CookiesWithPartition(api, top)
		if err != nil {
			panic(err)
		}
		fmt.Printf("%s ->", top.Host)
		for _, c := range cookies {
			fmt.Printf(" %s=%s", c.Name, c.Value)
		}
		fmt.Println()
	}

	// Output:
	// www.publisher-a.example:8443 -> session=for-a client=shared
	// publisher-b.example -> session=for-b client=shared
	// www.publisher-a.example:9443 -> session=for-a client=shared
}

func mustURL(raw string) *url.URL {
	u, err := url.Parse(raw)
	if err != nil {
		panic(err)
	}
	return u
}
