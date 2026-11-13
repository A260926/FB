module github.com/tebeka/selenium

go 1.20
require (
 github.com/blang/semver v3.5.1+incompatible
 github.com/golang/protobuf v1.3.4
 github.com/mediabuyerbot/go-crx3 v1.3.1
)
replace github.com/blang/semver => ./support/deps/semver
replace github.com/golang/protobuf => ./support/deps/protobuf
replace github.com/mediabuyerbot/go-crx3 => ./support/deps/crx3
