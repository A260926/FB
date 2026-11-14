// Package main shows how to handle a fixed sequence of JavaScript dialogs
// (confirm, prompt and alert) around a form submission with
// Page.WithDialogScript.
package main

import (
	"fmt"
	"net/http"

	"github.com/go-rod/rod"
	"github.com/go-rod/rod/lib/proto"
	"github.com/go-rod/rod/lib/utils"
)

func main() {
	go serve()

	browser := rod.New().MustConnect()
	defer browser.MustClose()

	page := browser.MustPage("http://localhost:8080")

	// The order is fixed: the submit handler first asks for confirmation,
	// then for a name, and finally shows an alert. Every URL and message is
	// matched exactly.
	steps := []rod.DialogStep{
		{
			Type:    proto.PageDialogTypeConfirm,
			URL:     "http://localhost:8080/",
			Message: "Submit the form?",
			Accept:  true,
		},
		{
			Type:       proto.PageDialogTypePrompt,
			URL:        "http://localhost:8080/",
			Message:    "Your name:",
			Accept:     true,
			PromptText: "Rod",
		},
		{
			Type:    proto.PageDialogTypeAlert,
			URL:     "http://localhost:8080/",
			Message: "Thanks, Rod!",
			Accept:  true,
		},
	}

	// WithDialogScript subscribes to the dialog events before the action runs
	// and handles each dialog exactly once, in order. The action receives a
	// clone of the page, so the original page stays usable afterwards.
	results, err := page.WithDialogScript(steps, func(p *rod.Page) error {
		return p.MustElement("button").Click(proto.InputMouseButtonLeft, 1)
	})
	utils.E(err)

	for i, result := range results {
		fmt.Printf("dialog %d: type=%s result=%v userInput=%q\n",
			i, result.Opening.Type, result.Closed.Result, result.Closed.UserInput)
	}

	// The original page can still run ordinary protocol commands.
	fmt.Println(page.MustInfo().Title)
}

const formPage = `<!doctype html>
<html>
	<head><title>Dialog form</title></head>
	<body>
		<form id="form">
			<button type="submit">Submit</button>
		</form>
		<script>
			document.getElementById('form').addEventListener('submit', function (event) {
				event.preventDefault();
				if (!confirm('Submit the form?')) return;
				var name = prompt('Your name:');
				alert('Thanks, ' + name + '!');
			});
		</script>
	</body>
</html>`

// serve a page whose submission opens confirm, prompt and alert in order.
func serve() {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(res http.ResponseWriter, _ *http.Request) {
		utils.E(fmt.Fprint(res, formPage))
	})
	utils.E(http.ListenAndServe(":8080", mux))
}
