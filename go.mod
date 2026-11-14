module github.com/raff/godet

go 1.23

require (
 github.com/gobs/httpclient v0.0.0-20251203215645-6c30f8bfb6e7
 github.com/gobs/pretty v0.0.0-20180724170744-09732c25a95b
 github.com/gobs/simplejson v0.0.0-20181106204727-c70e6bd5e26b
 github.com/gorilla/websocket v1.5.3
)
replace github.com/gobs/httpclient => ./deps/httpclient
replace github.com/gobs/pretty => ./deps/pretty
replace github.com/gobs/simplejson => ./deps/simplejson
replace github.com/gorilla/websocket => ./deps/websocket
