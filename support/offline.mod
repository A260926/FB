module github.com/chromedp/chromedp

go 1.25

require (
 github.com/chromedp/cdproto v0.159.0
 github.com/go-json-experiment/json v0.0.0-20260213210345-44df1a37e875
)
replace github.com/chromedp/cdproto => ./support/deps/github.com/chromedp/cdproto
replace github.com/go-json-experiment/json => ./support/deps/github.com/go-json-experiment/json

