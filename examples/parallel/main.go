// Command parallel issues several CoAP GET requests concurrently over
// a single coap.Session.  Message IDs and tokens are allocated by the
// caller, as Session requires.
//
// Usage:
//
//	parallel -addr localhost:5683 /reading/voltage /reading/current
package main

import (
	"context"
	"flag"
	"fmt"
	"net"
	"os"
	"sync"
	"time"

	coap "github.com/dustin/go-coap"
)

func main() {
	addr := flag.String("addr", "localhost:5683", "CoAP server address")
	flag.Parse()
	resources := flag.Args()
	if len(resources) == 0 {
		resources = []string{"/.well-known/core"}
	}

	uaddr, err := net.ResolveUDPAddr("udp", *addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "resolve: %v\n", err)
		os.Exit(1)
	}
	pc, err := net.ListenUDP("udp", nil)
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen: %v\n", err)
		os.Exit(1)
	}
	// The session takes ownership of pc; Close shuts it down.
	sess, err := coap.NewSession(pc, uaddr, coap.SessionConfig{})
	if err != nil {
		fmt.Fprintf(os.Stderr, "session: %v\n", err)
		os.Exit(1)
	}
	defer sess.Close()

	var wg sync.WaitGroup
	for i, resource := range resources {
		wg.Add(1)
		go func(i int, resource string) {
			defer wg.Done()
			req := coap.Message{
				Type:      coap.Confirmable,
				Code:      coap.GET,
				MessageID: uint16(1000 + i),
				Token:     []byte{byte(i + 1)},
			}
			req.SetPathString(resource)
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()
			resp, err := sess.Request(ctx, req)
			if err != nil {
				fmt.Printf("%s: error: %v\n", resource, err)
				return
			}
			fmt.Printf("%s: %s mid=%d payload=%q\n", resource, resp.Code, resp.MessageID, resp.Payload)
		}(i, resource)
	}
	wg.Wait()
}
