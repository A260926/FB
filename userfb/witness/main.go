package main

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"time"

	"github.com/chromedp/chromedp"
)

func must(err error) {
	if err != nil {
		panic(err)
	}
}

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, `<html><head><title>root</title></head><body><button id="advance" onclick="document.querySelector('iframe').contentWindow.location.hash='step2'">advance child</button><iframe src="/child"></iframe></body></html>`)
	})
	mux.HandleFunc("/child", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, `<html><body>child</body></html>`)
	})
	server := httptest.NewServer(mux)
	defer server.Close()
	opts := append(chromedp.DefaultExecAllocatorOptions[:], chromedp.ExecPath(os.Getenv("CHROMEDP_TEST_RUNNER")), chromedp.NoSandbox, chromedp.DisableGPU)
	alloc, stop := chromedp.NewExecAllocator(context.Background(), opts...)
	defer stop()
	ctx, closeTab := chromedp.NewContext(alloc)
	defer closeTab()
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	response, err := chromedp.RunResponse(ctx, chromedp.Navigate(server.URL+"/"))
	must(err)
	if response == nil || response.Status != 200 || response.URL != server.URL+"/" {
		panic("normal navigation did not return its HTML response")
	}
	fmt.Println("NORMAL root_document status=200 response_url_matches=true")
	wait, waitCancel := context.WithTimeout(ctx, 2*time.Second)
	defer waitCancel()
	response, err = chromedp.RunResponse(wait, chromedp.Click("#advance", chromedp.ByQuery))
	if err != nil || response != nil {
		panic(fmt.Sprintf("unexpected starting behavior: response=%v err=%v", response, err))
	}
	var rootURL, childHash string
	must(chromedp.Run(ctx, chromedp.Location(&rootURL), chromedp.Evaluate(`document.querySelector('iframe').contentWindow.location.hash`, &childHash)))
	if rootURL != server.URL+"/" || childHash != "#step2" {
		panic("click did not change only the child fragment")
	}
	fmt.Println("WITNESS root_wait=returned_nil_on_child_fragment main_url_unchanged=true child_hash=#step2")
}
