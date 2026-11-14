// Command collect demonstrates the device-interface collection flow:
// it reads several CoAP resources in parallel over a single
// coap.Session, parses the successful responses into structured
// records and writes them to a JSON Lines file in a temporary
// directory.
//
// The peer is a fixed local UDP device simulator (no real device
// needed) offering four behaviours:
//
//	reading/voltage  immediate piggybacked ACK with the measurement
//	reading/current  empty ACK, then a separate confirmable response
//	reading/power    empty ACK, then a duplicated confirmable response
//	reading/legacy   4.04 Not Found (lands in the failure list)
package main

import (
	"context"
	"fmt"
	"log"
	"net"
	"os"
	"path/filepath"
	"sync/atomic"
	"time"

	coap "github.com/dustin/go-coap"
	"github.com/dustin/go-coap/examples/collect"
)

var resources = []string{
	"reading/voltage",
	"reading/current",
	"reading/power",
	"reading/legacy",
}

func main() {
	dev, err := startDevice()
	if err != nil {
		log.Fatalf("device: %v", err)
	}
	defer dev.Close()

	pc, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		log.Fatalf("listen: %v", err)
	}
	sess, err := coap.NewSession(pc, dev.Addr(), coap.SessionConfig{
		AckTimeout:       200 * time.Millisecond,
		MaxRetransmit:    3,
		ExchangeLifetime: 10 * time.Second,
	})
	if err != nil {
		log.Fatalf("session: %v", err)
	}
	defer sess.Close()

	var midCounter uint32 = 1000
	querier := collect.QuerierFunc(func(ctx context.Context, resource string) (collect.Response, error) {
		mid := uint16(atomic.AddUint32(&midCounter, 1))
		req := coap.Message{
			Type:      coap.Confirmable,
			Code:      coap.GET,
			MessageID: mid,
			Token:     []byte(fmt.Sprintf("t%04d", mid)),
		}
		req.SetPathString(resource)
		resp, err := sess.Request(ctx, req)
		if err != nil {
			return collect.Response{}, err
		}
		return collect.Response{Code: int(resp.Code), Payload: resp.Payload}, nil
	})

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	result := collect.Collect(ctx, querier, resources)

	dir, err := os.MkdirTemp("", "coap-collect")
	if err != nil {
		log.Fatalf("tempdir: %v", err)
	}
	path := filepath.Join(dir, "readings.jsonl")
	if err := collect.WriteJSONL(path, result.Records); err != nil {
		log.Fatalf("write: %v", err)
	}

	fmt.Printf("records written to %s\n", path)
	for _, r := range result.Records {
		fmt.Printf("  record  resource=%s sample_id=%s value=%v\n", r.Resource, r.SampleID, r.Value)
	}
	for _, f := range result.Failures {
		fmt.Printf("  failure resource=%s reason=%s\n", f.Resource, f.Reason)
	}
	fmt.Printf("  duplicates dropped: %d\n", result.Duplicates)
}

// device is a fixed local UDP device simulator.
type device struct {
	conn    *net.UDPConn
	nextMID uint32
}

func startDevice() (*device, error) {
	c, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		return nil, err
	}
	d := &device{conn: c, nextMID: 5000}
	go d.loop()
	return d, nil
}

func (d *device) Addr() net.Addr { return d.conn.LocalAddr() }

func (d *device) Close() error { return d.conn.Close() }

func (d *device) loop() {
	buf := make([]byte, 1500)
	for {
		n, addr, err := d.conn.ReadFromUDP(buf)
		if err != nil {
			return
		}
		data := make([]byte, n)
		copy(data, buf[:n])
		msg, err := coap.ParseMessage(data)
		if err != nil {
			continue
		}
		switch msg.Type {
		case coap.Acknowledgement, coap.Reset:
			// ACKs for our separate responses; nothing to do.
		case coap.Confirmable:
			go d.handle(addr, msg)
		}
	}
}

func (d *device) handle(addr *net.UDPAddr, req coap.Message) {
	payload := func(s string) []byte { return []byte(s) }
	switch req.PathString() {
	case "reading/voltage":
		// Immediate response piggybacked on the ACK.
		d.send(addr, coap.Message{
			Type:      coap.Acknowledgement,
			Code:      coap.Content,
			MessageID: req.MessageID,
			Token:     req.Token,
			Payload:   payload("sample_id=volt-001;value=230.4"),
		})
	case "reading/current":
		// Empty ACK first, separate confirmable response after.
		d.send(addr, coap.Message{Type: coap.Acknowledgement, MessageID: req.MessageID})
		time.Sleep(50 * time.Millisecond)
		d.send(addr, coap.Message{
			Type:      coap.Confirmable,
			Code:      coap.Content,
			MessageID: d.mid(),
			Token:     req.Token,
			Payload:   payload("sample_id=curr-001;value=4.2"),
		})
	case "reading/power":
		// Empty ACK, then the same confirmable response twice
		// (retransmission); the session acknowledges both but
		// delivers only one.
		d.send(addr, coap.Message{Type: coap.Acknowledgement, MessageID: req.MessageID})
		time.Sleep(50 * time.Millisecond)
		m := coap.Message{
			Type:      coap.Confirmable,
			Code:      coap.Content,
			MessageID: d.mid(),
			Token:     req.Token,
			Payload:   payload("sample_id=pwr-001;value=964.0"),
		}
		d.send(addr, m)
		d.send(addr, m)
	case "reading/legacy":
		// Failing resource: 4.04 Not Found.
		d.send(addr, coap.Message{
			Type:      coap.Acknowledgement,
			Code:      coap.NotFound,
			MessageID: req.MessageID,
			Token:     req.Token,
		})
	}
}

func (d *device) mid() uint16 {
	return uint16(atomic.AddUint32(&d.nextMID, 1))
}

func (d *device) send(addr *net.UDPAddr, m coap.Message) {
	if err := coap.Transmit(d.conn, addr, m); err != nil {
		log.Printf("device send: %v", err)
	}
}
