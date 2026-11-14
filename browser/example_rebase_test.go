package browser_test

import (
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"

	"github.com/headzoo/surf"
)

// Example shows the multi-step application flow built around Rebase. The site
// reopens the same form with a fresh hidden token, so the existing form is
// rebased against the current document: user edits migrate onto the fresh
// snapshot, while the new token, buttons and action come from the new page.
// Rebase performs no network request itself; only the explicit Open and the
// final Click hit the network.
func Example() {
	// The test server rotates the hidden token and submit button every time
	// the entry page is opened.
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodPost {
			r.ParseForm()
			fmt.Printf("token=%s person=%s\n", r.Form.Get("token"), r.Form.Get("person"))
			io.WriteString(w, "<title>receipt</title>")
			return
		}
		token := "token-1"
		button := `<input type="submit" name="save" value="go">`
		if r.URL.Path == "/fresh" {
			token = "token-2"
			button = `<input type="submit" name="finish" value="done">`
		}
		fmt.Fprintf(w, `<html><body>
<form id="apply" method="post" action="/submit">
  <input type="hidden" name="token" value="%s">
  <input type="text" name="person" value="">
  %s
</form></body></html>`, token, button)
	}))
	defer ts.Close()

	bow := surf.NewBrowser()
	if err := bow.Open(ts.URL + "/apply"); err != nil {
		fmt.Println("open:", err)
		return
	}
	form, err := bow.Form("#apply")
	if err != nil {
		fmt.Println("form:", err)
		return
	}

	// The user fills in the form while the workflow is still on step one.
	if err := form.Input("person", "Ada"); err != nil {
		fmt.Println("input:", err)
		return
	}

	// The flow reopens the same site and hands out a new hidden token. Only
	// this Open fetches the page; Rebase itself sends nothing.
	if err := bow.Open(ts.URL + "/fresh"); err != nil {
		fmt.Println("reopen:", err)
		return
	}
	if err := form.Rebase("#apply"); err != nil {
		fmt.Println("rebase:", err)
		return
	}

	// The person edit survived, the token was refreshed from the new snapshot,
	// and the click uses the new button set.
	if err := form.Click("finish"); err != nil {
		fmt.Println("click:", err)
		return
	}
	// Output: token=token-2 person=Ada
}
