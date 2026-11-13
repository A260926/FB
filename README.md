# cookiejar
--
    import "github.com/juju/persistent-cookiejar"

Package cookiejar implements an in-memory RFC 6265-compliant http.CookieJar.

This implementation is a fork of net/http/cookiejar which also implements
methods for dumping the cookies to persistent storage and retrieving them.

## Partitioned cookies

The `Partitioned` attribute of `http.Cookie` is honored through two
additional methods. Both take the request URL plus a second URL describing
the **top-level site** the request is embedded in:

```go
func (j *Jar) SetCookiesWithPartition(u, topLevel *url.URL, cookies []*http.Cookie) error
func (j *Jar) CookiesWithPartition(u, topLevel *url.URL) ([]*http.Cookie, error)
```

`topLevel` identifies the partition. It is the page that embeds the
third-party interface, not the third-party request URL and not the whole
top-level URL. The key is the lower-case `http`/`https` scheme plus the
registrable domain of the canonical host, for example `https://example.com`.
Ports, paths, queries and fragments are ignored; scheme differences always
produce different partitions. IP addresses and hosts without a registrable
parent (such as `com` or `localhost`) use the full host name, with IPv6
literals normalized so a port does not change the key. Host canonicalization
(lower-casing, trailing dots, IDN/Punycode) follows the same rules as cookie
hosts.

A `Partitioned` cookie is stored only by `SetCookiesWithPartition`, only when
it is `Secure` and the request URL is HTTPS; otherwise the cookie is ignored
and never downgraded to an ordinary cookie. Ordinary cookies passed to
`SetCookiesWithPartition` still go to the unpartitioned space. The same
domain/path/name can exist once in the unpartitioned space and once per
partition, and updates, expiry and `MaxAge` deletion affect only their own
identity.

`CookiesWithPartition` merges the unpartitioned space with the partition named
by `topLevel`; cookies stored under other top-level sites are not visible.
Ordering follows the existing path-length and creation-time rules; complete
ties put unpartitioned cookies first, then order by name and domain so results
are stable. Returned cookies are fresh values, not shared with the internal
cookiejar state.

`topLevel` must be non-nil, must have a host, and must be HTTP or HTTPS; an
error is returned otherwise, before any access time is updated or any cookie
is written. If `u` itself is not an HTTP(S) URL, an empty result is returned
just like `Cookies`.

The original `Cookies`, `SetCookies`, `RemoveCookie` and `AllCookies` operate
only on the unpartitioned space. `SetCookies` ignores `Partitioned` cookies
because it has no top-level context. `RemoveAllHost` and `RemoveAll` are
administrative cleanups and cover every partition; their tombstones take part
in the disk merge, so cleared cookies do not reappear.

Partition identity is persisted in the existing JSON file via the
`Partitioned` and `PartitionKey` fields of each stored entry, so the old
on-disk format (which lacks those fields) still loads as ordinary cookies.
Entries with a malformed or contradictory partition identity cause the whole
load to fail rather than a partial merge. Save/load/merge key on the
partition as well, so two jars sharing one file can hold identically named
cookies under different top-level sites, delete them independently, and keep
the unpartitioned data intact.

## Usage

#### func  DefaultCookieFile

```go
func DefaultCookieFile() string
```
DefaultCookieFile returns the default cookie file to use for persisting cookie
data. The following names will be used in decending order of preference:

    - the value of the $GOCOOKIES environment variable.
    - $HOME/.go-cookies

#### type Jar

```go
type Jar struct {
}
```

Jar implements the http.CookieJar interface from the net/http package.

#### func  New

```go
func New(o *Options) (*Jar, error)
```
New returns a new cookie jar. A nil *Options is equivalent to a zero Options.

New will return an error if the cookies could not be loaded from the file for
any reason than if the file does not exist.

#### func (*Jar) Cookies

```go
func (j *Jar) Cookies(u *url.URL) (cookies []*http.Cookie)
```
Cookies implements the Cookies method of the http.CookieJar interface.

It returns an empty slice if the URL's scheme is not HTTP or HTTPS.

It reports only the unpartitioned cookie space; use CookiesWithPartition for
the top-level-site isolated view.

#### func (*Jar) CookiesWithPartition

```go
func (j *Jar) CookiesWithPartition(u, topLevel *url.URL) ([]*http.Cookie, error)
```
CookiesWithPartition returns the cookies for a request to u made from a page
whose top-level site is topLevel, combining the unpartitioned space with the
partition named by topLevel. See the Partitioned cookies section above.

#### func (*Jar) Save

```go
func (j *Jar) Save() error
```
Save saves the cookies to the persistent cookie file. Before the file is
written, it reads any cookies that have been stored from it and merges them into
j.

#### func (*Jar) SetCookies

```go
func (j *Jar) SetCookies(u *url.URL, cookies []*http.Cookie)
```
SetCookies implements the SetCookies method of the http.CookieJar interface.

It does nothing if the URL's scheme is not HTTP or HTTPS. It only writes the
unpartitioned space, and ignores cookies whose Partitioned field is set; use
SetCookiesWithPartition to store those under a top-level site.

#### func (*Jar) SetCookiesWithPartition

```go
func (j *Jar) SetCookiesWithPartition(u, topLevel *url.URL, cookies []*http.Cookie) error
```
SetCookiesWithPartition stores cookies received for u, attributing
Partitioned ones to the top-level site topLevel. See the Partitioned cookies
section above.

#### type Options

```go
type Options struct {
	// PublicSuffixList is the public suffix list that determines whether
	// an HTTP server can set a cookie for a domain.
	//
	// If this is nil, the public suffix list implementation in golang.org/x/net/publicsuffix
	// is used.
	PublicSuffixList PublicSuffixList

	// Filename holds the file to use for storage of the cookies.
	// If it is empty, the value of DefaultCookieFile will be used.
	Filename string
}
```

Options are the options for creating a new Jar.

#### type PublicSuffixList

```go
type PublicSuffixList interface {
	// PublicSuffix returns the public suffix of domain.
	//
	// TODO: specify which of the caller and callee is responsible for IP
	// addresses, for leading and trailing dots, for case sensitivity, and
	// for IDN/Punycode.
	PublicSuffix(domain string) string

	// String returns a description of the source of this public suffix
	// list. The description will typically contain something like a time
	// stamp or version number.
	String() string
}
```

PublicSuffixList provides the public suffix of a domain. For example:

    - the public suffix of "example.com" is "com",
    - the public suffix of "foo1.foo2.foo3.co.uk" is "co.uk", and
    - the public suffix of "bar.pvt.k12.ma.us" is "pvt.k12.ma.us".

Implementations of PublicSuffixList must be safe for concurrent use by multiple
goroutines.

An implementation that always returns "" is valid and may be useful for testing
but it is not secure: it means that the HTTP server for foo.com can set a cookie
for bar.com.

A public suffix list implementation is in the package
golang.org/x/net/publicsuffix.
