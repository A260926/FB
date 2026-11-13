package cookiejar

import (
	"encoding/json"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"
)

// partitionCases returns the offline replay cases that make up the
// partition contract. They use only locally constructed URLs, Set-Cookie
// values and temporary files: no network access and no browser cookie
// import.
func partitionCases() []partitionCase {
	return []partitionCase{
		{Name: "site_subdomains_and_ports", Group: "identity", Run: partitionSiteSubdomainsAndPorts},
		{Name: "scheme_separation", Group: "identity", Run: partitionSchemeSeparation},
		{Name: "ip_addresses", Group: "identity", Run: partitionIPAddresses},
		{Name: "same_name_coexistence", Group: "identity", Run: partitionSameNameCoexistence},
		{Name: "path_ordering", Group: "identity", Run: partitionPathOrdering},
		{Name: "invalid_top_level", Group: "identity", Run: partitionInvalidTopLevel},
		{Name: "two_jar_disk_merge", Group: "persistence", Run: partitionTwoJarDiskMerge},
		{Name: "invalid_stored_entries", Group: "persistence", Run: partitionInvalidStoredEntries},
		{Name: "old_entries_without_partition", Group: "persistence", Run: partitionOldEntries},
		{Name: "legacy_apis_unpartitioned_only", Group: "legacy", Run: partitionLegacyAPIs},
		{Name: "admin_cleanup_covers_partitions", Group: "legacy", Run: partitionAdminCleanup},
	}
}

func partitionURL(t *testing.T, raw string) *url.URL {
	t.Helper()
	u, err := url.Parse(raw)
	if err != nil {
		t.Fatalf("bad test URL %q: %v", raw, err)
	}
	return u
}

func partitionNewJar(t *testing.T) *Jar {
	t.Helper()
	j, err := New(&Options{PublicSuffixList: testPSL{}, NoPersist: true})
	if err != nil {
		t.Fatalf("cannot create jar: %v", err)
	}
	return j
}

func partitionNewFileJar(t *testing.T, file string) *Jar {
	t.Helper()
	j, err := New(&Options{PublicSuffixList: testPSL{}, Filename: file})
	if err != nil {
		t.Fatalf("cannot create jar at %s: %v", file, err)
	}
	return j
}

func partitionExpires(t *testing.T, d time.Duration) time.Time {
	t.Helper()
	return time.Now().Add(d)
}

// partitionCookie renders a cookie list as "name=value" pairs joined by
// spaces, preserving order.
func partitionCookie(cs []*http.Cookie) string {
	var parts []string
	for _, c := range cs {
		parts = append(parts, c.Name+"="+c.Value)
	}
	return strings.Join(parts, " ")
}

func partitionCookieNames(cs []*http.Cookie) []string {
	var names []string
	for _, c := range cs {
		names = append(names, c.Name+"="+c.Value)
	}
	sort.Strings(names)
	return names
}

// partitionSiteSubdomainsAndPorts checks that a partition follows the
// top-level site's registrable domain, ignoring subdomains, ports, path,
// query and fragment, and that the request URL's own site does not
// define the partition.
func partitionSiteSubdomainsAndPorts(t *testing.T) {
	j := partitionNewJar(t)
	now := tNow
	api := partitionURL(t, "https://api.thirdparty.com/collect")
	topA := partitionURL(t, "https://a.example.com:8443/one/page?x=1#frag")
	topB := partitionURL(t, "https://b.example.com:9443/other")
	topOther := partitionURL(t, "https://example.net/landing")

	exp := now.Add(time.Hour)
	j.setCookiesWithPartition(api, topA, []*http.Cookie{{
		Name:        "sid",
		Value:       "site-example",
		Path:        "/",
		Secure:      true,
		Partitioned: true,
		Expires:     exp,
	}}, now)

	gotA, err := j.cookiesWithPartition(api, topA, now)
	partitionCheck(t, err == nil && partitionCookie(gotA) == "sid=site-example")
	gotB, err := j.cookiesWithPartition(api, topB, now)
	partitionCheck(t, err == nil && partitionCookie(gotB) == "sid=site-example")
	gotOther, err := j.cookiesWithPartition(api, topOther, now)
	partitionCheck(t, err == nil && partitionCookie(gotOther) == "")

	// Only one partition key should exist, and it is derived from the
	// top-level site rather than from the request host.
	partitionCheck(t, len(j.partitioned) == 1)
	partitionCheck(t, j.partitioned["https://example.com"] != nil)
	partitionCheck(t, j.partitioned["https://thirdparty.com"] == nil)

	// Host canonicalization (case, trailing dot, IDN) applies to the
	// top-level host too, and the scheme is lower-cased.
	topUpper := partitionURL(t, "HTTPS://WWW.EXAMPLE.COM.:443/any")
	gotUpper, err := j.cookiesWithPartition(api, topUpper, now)
	partitionCheck(t, err == nil && partitionCookie(gotUpper) == "sid=site-example")
	topIDN := partitionURL(t, "https://www.bücher.example/")
	j.setCookiesWithPartition(api, topIDN, []*http.Cookie{{
		Name:        "idn",
		Value:       "ascii-key",
		Path:        "/",
		Secure:      true,
		Partitioned: true,
		Expires:     exp,
	}}, now)
	partitionCheck(t, j.partitioned["https://xn--bcher-kva.example"] != nil)
	gotIDN, err := j.cookiesWithPartition(api, partitionURL(t, "https://shop.bücher.example/"), now)
	partitionCheck(t, err == nil && partitionCookie(gotIDN) == "idn=ascii-key")
}

// partitionSchemeSeparation checks that http and https top-level sites
// are distinct partitions even for the same registrable domain.
func partitionSchemeSeparation(t *testing.T) {
	j := partitionNewJar(t)
	now := tNow
	api := partitionURL(t, "https://api.thirdparty.com/collect")
	topHTTP := partitionURL(t, "http://example.com/page")
	topHTTPS := partitionURL(t, "https://example.com/page")

	exp := now.Add(time.Hour)
	j.setCookiesWithPartition(api, topHTTP, []*http.Cookie{{
		Name: "sid", Value: "http-partition", Path: "/",
		Secure: true, Partitioned: true, Expires: exp,
	}}, now)
	j.setCookiesWithPartition(api, topHTTPS, []*http.Cookie{{
		Name: "sid", Value: "https-partition", Path: "/",
		Secure: true, Partitioned: true, Expires: exp,
	}}, now)

	gotHTTP, err := j.cookiesWithPartition(api, topHTTP, now)
	partitionCheck(t, err == nil && partitionCookie(gotHTTP) == "sid=http-partition")
	gotHTTPS, err := j.cookiesWithPartition(api, topHTTPS, now)
	partitionCheck(t, err == nil && partitionCookie(gotHTTPS) == "sid=https-partition")
	partitionCheck(t, j.partitioned["http://example.com"] != nil)
	partitionCheck(t, j.partitioned["https://example.com"] != nil)
}

// partitionIPAddresses checks that IP top-level sites use the full host
// name, that ports do not split an IP partition, and that a bracketed
// IPv6 literal with and without a port is the same partition.
func partitionIPAddresses(t *testing.T) {
	j := partitionNewJar(t)
	now := tNow
	api := partitionURL(t, "https://api.thirdparty.com/collect")
	topV4 := partitionURL(t, "http://192.168.0.5:8000/page")
	topV4Same := partitionURL(t, "http://192.168.0.5:9000/elsewhere")
	topV4Other := partitionURL(t, "http://192.168.0.6/")
	topV6 := partitionURL(t, "https://[2001:db8::1]:8443/page")
	topV6NoPort := partitionURL(t, "https://[2001:db8::1]/root")

	exp := now.Add(time.Hour)
	j.setCookiesWithPartition(api, topV4, []*http.Cookie{{
		Name: "v4", Value: "yes", Path: "/",
		Secure: true, Partitioned: true, Expires: exp,
	}}, now)
	j.setCookiesWithPartition(api, topV6, []*http.Cookie{{
		Name: "v6", Value: "yes", Path: "/",
		Secure: true, Partitioned: true, Expires: exp,
	}}, now)

	gotSame, err := j.cookiesWithPartition(api, topV4Same, now)
	partitionCheck(t, err == nil && partitionCookie(gotSame) == "v4=yes")
	gotOther, err := j.cookiesWithPartition(api, topV4Other, now)
	partitionCheck(t, err == nil && partitionCookie(gotOther) == "")
	gotV6, err := j.cookiesWithPartition(api, topV6NoPort, now)
	partitionCheck(t, err == nil && partitionCookie(gotV6) == "v6=yes")

	partitionCheck(t, j.partitioned["http://192.168.0.5"] != nil)
	partitionCheck(t, j.partitioned["https://2001:db8::1"] != nil)
	partitionCheck(t, len(j.partitioned) == 2)
}

// partitionSameNameCoexistence checks that an unpartitioned cookie and
// partitioned cookies with the same domain/path/name can coexist, and
// that updates, expiry and MaxAge deletion only affect their own
// identity/partition.
func partitionSameNameCoexistence(t *testing.T) {
	j := partitionNewJar(t)
	now := tNow
	api := partitionURL(t, "https://api.thirdparty.com/collect")
	topA := partitionURL(t, "https://alpha.example.com/")
	topB := partitionURL(t, "https://beta.example.net/")
	exp := now.Add(time.Hour)

	j.setCookiesWithPartition(api, topA, []*http.Cookie{{
		Name: "sid", Value: "plain", Path: "/", Expires: exp,
	}}, now)
	j.setCookiesWithPartition(api, topA, []*http.Cookie{{
		Name: "sid", Value: "partition-a", Path: "/",
		Secure: true, Partitioned: true, Expires: exp,
	}}, now)
	j.setCookiesWithPartition(api, topB, []*http.Cookie{{
		Name: "sid", Value: "partition-b", Path: "/",
		Secure: true, Partitioned: true, Expires: exp,
	}}, now)

	gotA, err := j.cookiesWithPartition(api, topA, now)
	partitionCheck(t, err == nil && partitionCookie(gotA) == "sid=plain sid=partition-a")
	gotB, err := j.cookiesWithPartition(api, topB, now)
	partitionCheck(t, err == nil && partitionCookie(gotB) == "sid=plain sid=partition-b")
	partitionCheck(t, len(j.entries[jarKey("api.thirdparty.com", testPSL{})]) == 1)
	partitionCheck(t, j.entries["thirdparty.com"][id("api.thirdparty.com", "/", "sid")].Value == "plain")
	partitionCheck(t, j.partitioned["https://example.com"][id("api.thirdparty.com", "/", "sid")].Value == "partition-a")
	partitionCheck(t, j.partitioned["https://example.net"][id("api.thirdparty.com", "/", "sid")].Value == "partition-b")

	// Updating one partitioned identity leaves the other partition and
	// the unpartitioned cookie alone.
	later := now.Add(time.Minute)
	j.setCookiesWithPartition(api, topA, []*http.Cookie{{
		Name: "sid", Value: "partition-a2", Path: "/",
		Secure: true, Partitioned: true, Expires: exp,
	}}, later)
	gotA, _ = j.cookiesWithPartition(api, topA, later)
	gotB, _ = j.cookiesWithPartition(api, topB, later)
	partitionCheck(t, partitionCookie(gotA) == "sid=plain sid=partition-a2")
	partitionCheck(t, partitionCookie(gotB) == "sid=plain sid=partition-b")

	// MaxAge < 0 removes only the addressed identity.
	j.setCookiesWithPartition(api, topA, []*http.Cookie{{
		Name: "sid", MaxAge: -1, Path: "/",
		Secure: true, Partitioned: true,
	}}, later)
	gotA, _ = j.cookiesWithPartition(api, topA, later)
	gotB, _ = j.cookiesWithPartition(api, topB, later)
	partitionCheck(t, partitionCookie(gotA) == "sid=plain")
	partitionCheck(t, partitionCookie(gotB) == "sid=plain sid=partition-b")
	partitionCheck(t, j.entries["thirdparty.com"][id("api.thirdparty.com", "/", "sid")].Value == "plain")
}

// partitionPathOrdering checks the combined ordering rules: longer path
// first, then earlier creation, and for a complete tie an unpartitioned
// cookie comes before a partitioned one.
func partitionPathOrdering(t *testing.T) {
	j := partitionNewJar(t)
	now := tNow
	api := partitionURL(t, "https://api.thirdparty.com/deep/path/x")
	top := partitionURL(t, "https://example.com/")
	exp := now.Add(10 * time.Hour)

	set := func(name, value, path string, partitioned bool, at time.Time) {
		t.Helper()
		j.setCookiesWithPartition(api, top, []*http.Cookie{{
			Name: name, Value: value, Path: path,
			Partitioned: partitioned, Secure: partitioned, Expires: exp,
		}}, at)
	}
	set("c", "c", "/deep/path", true, now)
	set("b", "b", "/deep", true, now)
	set("f", "f", "/deep", true, now.Add(time.Hour))
	set("a", "a", "/", true, now)
	set("d", "d", "/", false, now)

	got, err := j.cookiesWithPartition(api, top, now.Add(2*time.Hour))
	partitionCheck(t, err == nil)
	// /deep/path (len 10) then /deep (len 5, b before f by creation)
	// then "/" (len 1): unpartitioned d before partitioned a.
	partitionCheck(t, partitionCookie(got) == "c=c b=b f=f d=d a=a")

	// The same rules hold when only the unpartitioned space is queried.
	legacy := j.cookies(api, now.Add(2*time.Hour))
	partitionCheck(t, partitionCookie(legacy) == "d=d")

	// The returned slice must not share values with the jar: mutating
	// it must not change what later reads return.
	got, _ = j.cookiesWithPartition(api, top, now.Add(2*time.Hour))
	got[0].Value = "mutated"
	fresh, _ := j.cookiesWithPartition(api, top, now.Add(2*time.Hour))
	partitionCheck(t, partitionCookie(fresh) == "c=c b=b f=f d=d a=a")
}

// partitionInvalidTopLevel checks that a nil, host-less or non-HTTP(S)
// top-level URL is rejected before any access time update or cookie write.
func partitionInvalidTopLevel(t *testing.T) {
	j := partitionNewJar(t)
	now := tNow
	api := partitionURL(t, "https://api.thirdparty.com/collect")
	good := partitionURL(t, "https://example.com/")
	exp := now.Add(time.Hour)

	j.setCookiesWithPartition(api, good, []*http.Cookie{{
		Name: "sid", Value: "stable", Path: "/",
		Secure: true, Partitioned: true, Expires: exp,
	}}, now)
	before := j.partitioned["https://example.com"][id("api.thirdparty.com", "/", "sid")].LastAccess

	for _, top := range []*url.URL{
		nil,
		{Scheme: "https"},
		{Scheme: "https", Host: ""},
		{Scheme: "ftp", Host: "example.com"},
		{Scheme: "file", Host: "example.com"},
	} {
		if _, err := j.cookiesWithPartition(api, top, now.Add(time.Hour)); err == nil {
			t.Fatal("expected error reading with invalid top-level URL")
		}
		if err := j.setCookiesWithPartition(api, top, []*http.Cookie{{
			Name: "bad", Value: "x", Path: "/", Secure: true,
			Partitioned: true, Expires: exp,
		}}, now.Add(time.Hour)); err == nil {
			t.Fatal("expected error writing with invalid top-level URL")
		}
	}

	after := j.partitioned["https://example.com"][id("api.thirdparty.com", "/", "sid")].LastAccess
	partitionCheck(t, after.Equal(before))
	partitionCheck(t, len(j.partitioned["https://example.com"]) == 1)
	got, err := j.cookiesWithPartition(api, good, now)
	partitionCheck(t, err == nil && partitionCookie(got) == "sid=stable")
}

// partitionTwoJarDiskMerge opens the same temporary file from two jars,
// stores cookies with identical domain/path/name under two different
// top-level partitions, and then deletes them in interleaved saves.
// Neither jar may overwrite the other's partition, and the old
// unpartitioned data must survive.
func partitionTwoJarDiskMerge(t *testing.T) {
	file := filepath.Join(t.TempDir(), "cookies")
	api := partitionURL(t, "https://api.thirdparty.com/collect")
	topA := partitionURL(t, "https://alpha.example.com/")
	topB := partitionURL(t, "https://beta.example.net/")
	exp := partitionExpires(t, 48*time.Hour)

	open := func() *Jar { return partitionNewFileJar(t, file) }
	read := func(j *Jar, top *url.URL) string {
		t.Helper()
		cs, err := j.CookiesWithPartition(api, top)
		if err != nil {
			t.Fatalf("cannot read partitioned cookies: %v", err)
		}
		return partitionCookie(cs)
	}

	ja, jb := open(), open()
	if err := ja.SetCookiesWithPartition(api, topA, []*http.Cookie{
		{Name: "sid", Value: "alpha", Path: "/", Secure: true, Partitioned: true, Expires: exp},
		{Name: "plain", Value: "kept", Path: "/", Expires: exp},
	}); err != nil {
		t.Fatalf("cannot set alpha cookies: %v", err)
	}
	if err := jb.SetCookiesWithPartition(api, topB, []*http.Cookie{
		{Name: "sid", Value: "beta", Path: "/", Secure: true, Partitioned: true, Expires: exp},
	}); err != nil {
		t.Fatalf("cannot set beta cookies: %v", err)
	}
	if err := ja.Save(); err != nil {
		t.Fatalf("alpha save: %v", err)
	}
	if err := jb.Save(); err != nil {
		t.Fatalf("beta save: %v", err)
	}

	fresh := open()
	partitionCheck(t, read(fresh, topA) == "plain=kept sid=alpha")
	partitionCheck(t, read(fresh, topB) == "plain=kept sid=beta")
	partitionCheck(t, len(fresh.partitioned) == 2)

	// Interleaved deletion: the second jar must not resurrect what the
	// first deleted, and must not overwrite the other partition.
	delA, delB := open(), open()
	if err := delA.SetCookiesWithPartition(api, topA, []*http.Cookie{
		{Name: "sid", MaxAge: -1, Path: "/", Secure: true, Partitioned: true},
	}); err != nil {
		t.Fatalf("alpha delete: %v", err)
	}
	if err := delA.Save(); err != nil {
		t.Fatalf("alpha save after delete: %v", err)
	}
	freshA := open()
	partitionCheck(t, read(freshA, topA) == "plain=kept")
	partitionCheck(t, read(freshA, topB) == "plain=kept sid=beta")

	if err := delB.SetCookiesWithPartition(api, topB, []*http.Cookie{
		{Name: "sid", MaxAge: -1, Path: "/", Secure: true, Partitioned: true},
	}); err != nil {
		t.Fatalf("beta delete: %v", err)
	}
	if err := delB.Save(); err != nil {
		t.Fatalf("beta save after delete: %v", err)
	}
	freshB := open()
	partitionCheck(t, read(freshB, topA) == "plain=kept")
	partitionCheck(t, read(freshB, topB) == "plain=kept")

	// Another save must not resurrect either tombstone.
	if err := freshB.Save(); err != nil {
		t.Fatalf("final save: %v", err)
	}
	freshC := open()
	partitionCheck(t, read(freshC, topA) == "plain=kept")
	partitionCheck(t, read(freshC, topB) == "plain=kept")
}

// partitionInvalidStoredEntries checks that a partitioned entry missing
// Secure, lacking a partition key, having a malformed key or contradicting
// the flag aborts the whole load without partially merging valid entries.
func partitionInvalidStoredEntries(t *testing.T) {
	now := tNow
	exp := now.Add(24 * time.Hour)
	valid := entry{
		Name: "ok", Value: "v", Domain: "www.example.com", Path: "/",
		Expires: exp, Updated: now, Creation: now, CanonicalHost: "www.example.com",
		Persistent: true,
	}
	invalids := map[string]entry{
		"partitioned_without_secure": {
			Name: "bad", Value: "v", Domain: "api.thirdparty.com", Path: "/",
			Expires: exp, Updated: now, Creation: now, CanonicalHost: "api.thirdparty.com",
			Persistent: true, Partitioned: true, PartitionKey: "https://example.com",
		},
		"partitioned_without_key": {
			Name: "bad", Value: "v", Domain: "api.thirdparty.com", Path: "/",
			Secure: true, Expires: exp, Updated: now, Creation: now,
			CanonicalHost: "api.thirdparty.com", Persistent: true, Partitioned: true,
		},
		"non_canonical_key": {
			Name: "bad", Value: "v", Domain: "api.thirdparty.com", Path: "/",
			Secure: true, Expires: exp, Updated: now, Creation: now,
			CanonicalHost: "api.thirdparty.com", Persistent: true,
			Partitioned: true, PartitionKey: "https://Example.com",
		},
		"key_with_port": {
			Name: "bad", Value: "v", Domain: "api.thirdparty.com", Path: "/",
			Secure: true, Expires: exp, Updated: now, Creation: now,
			CanonicalHost: "api.thirdparty.com", Persistent: true,
			Partitioned: true, PartitionKey: "https://example.com:443",
		},
		"key_not_registrable": {
			Name: "bad", Value: "v", Domain: "api.thirdparty.com", Path: "/",
			Secure: true, Expires: exp, Updated: now, Creation: now,
			CanonicalHost: "api.thirdparty.com", Persistent: true,
			Partitioned: true, PartitionKey: "https://sub.example.com",
		},
		"key_with_path": {
			Name: "bad", Value: "v", Domain: "api.thirdparty.com", Path: "/",
			Secure: true, Expires: exp, Updated: now, Creation: now,
			CanonicalHost: "api.thirdparty.com", Persistent: true,
			Partitioned: true, PartitionKey: "https://example.com/landing",
		},
		"key_without_flag": {
			Name: "bad", Value: "v", Domain: "api.thirdparty.com", Path: "/",
			Secure: true, Expires: exp, Updated: now, Creation: now,
			CanonicalHost: "api.thirdparty.com", Persistent: true,
			PartitionKey: "https://example.com",
		},
	}

	for name, bad := range invalids {
		// No nested t.Run: all checks are reported against the case's
		// test so that the driver can detect an empty case.
		dir := t.TempDir()
		file := filepath.Join(dir, "cookies")
		data, err := json.Marshal([]entry{valid, bad})
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(file, data, 0600); err != nil {
			t.Fatal(err)
		}
		j, err := New(&Options{PublicSuffixList: testPSL{}, Filename: file})
		partitionCheck(t, err != nil && j == nil)

		// mergeFrom must reject the file before installing even
		// the valid entry that precedes the invalid one.
		j2 := partitionNewJar(t)
		f, err := os.Open(file)
		if err != nil {
			t.Fatal(err)
		}
		err = j2.mergeFrom(f)
		f.Close()
		partitionCheck(t, err != nil)
		partitionCheck(t, len(j2.entries) == 0)
		partitionCheck(t, len(j2.partitioned) == 0)
		if name == "" {
			t.Fatal("unnamed invalid case")
		}
	}
}

// partitionOldEntries checks that an entry file without the partition
// fields is still loaded as an ordinary cookie.
func partitionOldEntries(t *testing.T) {
	dir := t.TempDir()
	file := filepath.Join(dir, "cookies")
	now := time.Now()
	old := entry{
		Name: "legacy", Value: "v", Domain: "www.example.com", Path: "/",
		Expires:       now.Add(24 * time.Hour),
		Updated:       now.Add(-time.Hour),
		Creation:      now.Add(-time.Hour),
		CanonicalHost: "www.example.com", Persistent: true,
	}
	data, err := json.Marshal([]entry{old})
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(data), "Partition") {
		t.Fatal("old-format entry unexpectedly serialized partition fields")
	}
	if err := os.WriteFile(file, data, 0600); err != nil {
		t.Fatal(err)
	}
	j := partitionNewFileJar(t, file)
	u := partitionURL(t, "https://www.example.com/x")
	got, err := j.CookiesWithPartition(u, partitionURL(t, "https://example.com/"))
	partitionCheck(t, err == nil && partitionCookie(got) == "legacy=v")

	all := j.AllCookies()
	partitionCheck(t, len(all) == 1 && all[0].Name == "legacy" && all[0].Value == "v")
	partitionCheck(t, len(j.partitioned) == 0)
}

// partitionLegacyAPIs checks that the original Cookies, SetCookies,
// RemoveCookie and AllCookies only operate on the unpartitioned space, and
// that old SetCookies ignores Partitioned cookies rather than accepting
// them as ordinary ones.
func partitionLegacyAPIs(t *testing.T) {
	j := partitionNewJar(t)
	now := time.Now()
	api := partitionURL(t, "https://api.thirdparty.com/collect")
	top := partitionURL(t, "https://example.com/")
	exp := now.Add(time.Hour)

	// Old SetCookies: the partitioned cookie is ignored, the ordinary
	// cookie is stored.
	j.setCookies(api, []*http.Cookie{
		{Name: "part", Value: "ignored", Path: "/", Secure: true, Partitioned: true, Expires: exp},
		{Name: "plain", Value: "kept", Path: "/", Expires: exp},
	}, now)
	partitionCheck(t, len(j.partitioned) == 0)
	legacy := j.cookies(api, now)
	partitionCheck(t, partitionCookie(legacy) == "plain=kept")
	partitionCheck(t, len(j.allCookies(now)) == 1 && j.allCookies(now)[0].Name == "plain")

	// Both spaces may hold the same identity; RemoveCookie removes only
	// the unpartitioned copy.
	partitionCheck(t, len(j.partitioned) == 0)
	j.setCookiesWithPartition(api, top, []*http.Cookie{{
		Name: "plain", Value: "partitioned-copy", Path: "/",
		Secure: true, Partitioned: true, Expires: exp,
	}}, now)
	partitionCheck(t, len(j.partitioned) == 1)
	j.RemoveCookie(&http.Cookie{Domain: "api.thirdparty.com", Path: "/", Name: "plain"})
	gotPlain := j.cookies(api, now.Add(time.Second))
	partitionCheck(t, partitionCookie(gotPlain) == "")
	gotPart, err := j.cookiesWithPartition(api, top, now.Add(time.Second))
	partitionCheck(t, err == nil && partitionCookie(gotPart) == "plain=partitioned-copy")
	partitionCheck(t, len(j.allCookies(now.Add(time.Second))) == 0)
}

// partitionAdminCleanup checks that RemoveAllHost and RemoveAll clear
// every partition and that the resulting tombstones survive a later
// disk merge instead of being resurrected.
func partitionAdminCleanup(t *testing.T) {
	file := filepath.Join(t.TempDir(), "cookies")
	api := partitionURL(t, "https://api.thirdparty.com/collect")
	other := partitionURL(t, "https://cdn.example.net/asset")
	topA := partitionURL(t, "https://alpha.example.com/")
	topB := partitionURL(t, "https://beta.example.org/")
	exp := partitionExpires(t, 48*time.Hour)

	j := partitionNewFileJar(t, file)
	if err := j.SetCookiesWithPartition(api, topA, []*http.Cookie{
		{Name: "sid", Value: "alpha", Path: "/", Secure: true, Partitioned: true, Expires: exp},
		{Name: "plain", Value: "plain", Path: "/", Expires: exp},
	}); err != nil {
		t.Fatal(err)
	}
	if err := j.SetCookiesWithPartition(api, topB, []*http.Cookie{
		{Name: "sid", Value: "beta", Path: "/", Secure: true, Partitioned: true, Expires: exp},
	}); err != nil {
		t.Fatal(err)
	}
	if err := j.SetCookiesWithPartition(other, topA, []*http.Cookie{
		{Name: "asset", Value: "other-host", Path: "/", Expires: exp},
	}); err != nil {
		t.Fatal(err)
	}

	j.RemoveAllHost("api.thirdparty.com")
	j.Save()

	fresh := partitionNewFileJar(t, file)
	for _, top := range []*url.URL{topA, topB} {
		cs, err := fresh.CookiesWithPartition(api, top)
		partitionCheck(t, err == nil && partitionCookie(cs) == "")
	}
	cs, err := fresh.CookiesWithPartition(other, topA)
	partitionCheck(t, err == nil && partitionCookie(cs) == "asset=other-host")

	// A second save/load cycle must not bring the cleared cookies back.
	if err := fresh.Save(); err != nil {
		t.Fatal(err)
	}
	fresh2 := partitionNewFileJar(t, file)
	gotA, err := fresh2.CookiesWithPartition(api, topA)
	partitionCheck(t, err == nil && partitionCookie(gotA) == "")
	gotB, err := fresh2.CookiesWithPartition(api, topB)
	partitionCheck(t, err == nil && partitionCookie(gotB) == "")

	// RemoveAll clears both spaces.
	fresh2.RemoveAll()
	if err := fresh2.Save(); err != nil {
		t.Fatal(err)
	}
	fresh3 := partitionNewFileJar(t, file)
	gotAll, err := fresh3.CookiesWithPartition(other, topA)
	partitionCheck(t, err == nil && len(partitionCookieNames(gotAll)) == 0)
	gotPlain := fresh3.Cookies(other)
	partitionCheck(t, len(gotPlain) == 0)
}
