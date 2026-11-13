package main

import (
	"context"
	"encoding/json"
	"fmt"
	"github.com/go-rod/rod"
	"github.com/go-rod/rod/lib/proto"
	"github.com/go-rod/rod/task_support/cdpstub"
	"os"
	"time"
)

func main() {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	c := cdpstub.New()
	b := rod.New().Context(ctx).Client(c)
	if e := b.Connect(); e != nil {
		panic(e)
	}
	p := b.PageFromSession("page-A")
	r := p.HijackRequests()
	if e := r.Add("*", proto.NetworkResourceTypeDocument, func(h *rod.Hijack) { h.ContinueRequest(&proto.FetchContinueRequest{}) }); e != nil {
		panic(e)
	}
	go r.Run()
	if e := c.Emit("page-A", "Fetch.requestPaused", map[string]interface{}{"requestId": "normal", "resourceType": "Document", "request": map[string]interface{}{"url": "https://site.test/", "method": "GET", "headers": map[string]string{}}}); e != nil {
		panic(e)
	}
	timer := time.NewTimer(5 * time.Second)
	defer timer.Stop()
wait:
	for {
		select {
		case x := <-c.Called:
			if x.Method == "Fetch.continueRequest" {
				fmt.Println("NORMAL_ROUTE_CONTINUE=1")
				break wait
			}
		case <-timer.C:
			panic("normal route did not dispatch")
		}
	}
	if e := r.Stop(); e != nil {
		panic(e)
	}
	if len(os.Args) == 1 {
		return
	}
	root := b.HijackRequests()
	if e := root.Add("https://site.test/*", proto.NetworkResourceTypeDocument, func(h *rod.Hijack) { h.Response.SetBody("ok") }); e != nil {
		panic(e)
	}
	defer root.Stop()
	_ = b.HandleAuth("u", "p") // registration alone rewrites the active Fetch configuration
	var last json.RawMessage
	for _, x := range c.Records() {
		if x.Session == "" && x.Method == "Fetch.enable" {
			last = x.Params
		}
	}
	var cfg struct {
		HandleAuthRequests bool              `json:"handleAuthRequests"`
		Patterns           []json.RawMessage `json:"patterns"`
	}
	if e := json.Unmarshal(last, &cfg); e != nil {
		panic(e)
	}
	if !cfg.HandleAuthRequests || len(cfg.Patterns) != 0 {
		panic(string(last))
	}
	fmt.Printf("AUTH_OVERWROTE_ROUTE_PATTERNS=%d\n", len(cfg.Patterns))
	fmt.Printf("FETCH_CONFIG=%s\n", last)
}
