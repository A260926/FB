package ldap

import (
	"bytes"
	"context"
	"errors"
	"net"
	"strings"
	"testing"
	"time"

	ber "github.com/go-asn1-ber/asn1-ber"
)

// wrongPagingControl advertises the paging OID without being a *ControlPaging;
// SearchPages must reject it before touching the network.
type wrongPagingControl struct{}

func (wrongPagingControl) GetControlType() string { return ControlTypePaging }
func (wrongPagingControl) Encode() *ber.Packet    { return nil }
func (wrongPagingControl) String() string         { return "wrong paging type" }

// pagedPipeServer is the BER-decoding protocol peer used by the paged search
// tests. It runs on one end of a net.Pipe, decodes every request packet the
// client sends and hands them to the test through a channel. Tests drive the
// reply order explicitly; the timers below are hang guards only, never the
// mechanism that establishes ordering.
type pagedPipeServer struct {
	t        *testing.T
	conn     net.Conn
	requests chan *ber.Packet
}

func newPagedPipeServer(t *testing.T) (*pagedPipeServer, net.Conn) {
	t.Helper()
	client, server := net.Pipe()
	s := &pagedPipeServer{
		t:        t,
		conn:     server,
		requests: make(chan *ber.Packet, 64),
	}
	go func() {
		for {
			packet, err := ber.ReadPacket(server)
			if err != nil {
				return
			}
			s.requests <- packet
		}
	}()
	return s, client
}

func (s *pagedPipeServer) nextRequest() *ber.Packet {
	s.t.Helper()
	select {
	case packet := <-s.requests:
		return packet
	case <-time.After(5 * time.Second):
		s.t.Fatal("timed out waiting for the client to send a request")
		return nil
	}
}

func (s *pagedPipeServer) assertNoRequest() {
	s.t.Helper()
	select {
	case packet := <-s.requests:
		s.t.Fatalf("unexpected request sent: op tag %d", packet.Children[1].Tag)
	default:
	}
}

func (s *pagedPipeServer) send(packet *ber.Packet) {
	s.t.Helper()
	if _, err := s.conn.Write(packet.Bytes()); err != nil {
		s.t.Fatalf("server write failed: %v", err)
	}
}

func (s *pagedPipeServer) close() { _ = s.conn.Close() }

// --- wire packet builders (server side) ---

func pagedEnvelope(messageID int64, op *ber.Packet) *ber.Packet {
	packet := ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "LDAP Response")
	packet.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimitive, ber.TagInteger, messageID, "MessageID"))
	packet.AppendChild(op)
	return packet
}

func pagedEntryPacket(messageID int64, dn string) *ber.Packet {
	op := ber.Encode(ber.ClassApplication, ber.TypeConstructed, ApplicationSearchResultEntry, nil, "Search Result Entry")
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, dn, "Object Name"))
	op.AppendChild(ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "Attributes"))
	return pagedEnvelope(messageID, op)
}

func pagedReferencePacket(messageID int64, uri string) *ber.Packet {
	op := ber.Encode(ber.ClassApplication, ber.TypeConstructed, ApplicationSearchResultReference, nil, "Search Result Reference")
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, uri, "URI"))
	return pagedEnvelope(messageID, op)
}

func pagedDonePacket(messageID int64, resultCode uint16, diagnostic string, controls ...*ber.Packet) *ber.Packet {
	op := ber.Encode(ber.ClassApplication, ber.TypeConstructed, ApplicationSearchResultDone, nil, "Search Result Done")
	op.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimitive, ber.TagEnumerated, int64(resultCode), "Result Code"))
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, "", "Matched DN"))
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, diagnostic, "Diagnostic Message"))
	packet := pagedEnvelope(messageID, op)
	if len(controls) > 0 {
		controlsPacket := ber.Encode(ber.ClassContext, ber.TypeConstructed, 0, nil, "Controls")
		for _, control := range controls {
			controlsPacket.AppendChild(control)
		}
		packet.AppendChild(controlsPacket)
	}
	return packet
}

func pagingControlPacket(size uint32, cookie []byte) *ber.Packet {
	control := NewControlPaging(size)
	control.SetCookie(cookie)
	return control.Encode()
}

// malformedPagingControlPacket builds a paging response control whose value
// sequence has no children, which DecodeControl must reject.
func malformedPagingControlPacket() *ber.Packet {
	control := ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "Control")
	control.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, ControlTypePaging, "Control Type"))
	value := ber.Encode(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, nil, "Control Value (Paging)")
	value.AppendChild(ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "Search Control Value"))
	control.AppendChild(value)
	return control
}

// --- request inspection helpers (server side) ---

func requestMessageID(packet *ber.Packet) int64 {
	return packet.Children[0].Value.(int64)
}

func requestSearchOp(packet *ber.Packet) *ber.Packet {
	return packet.Children[1]
}

// requestAbandonID extracts the target message ID of an AbandonRequest.
// Application-class primitive packets keep their integer in Data because the
// BER decoder only populates Value for universal-class primitives.
func requestAbandonID(t *testing.T, packet *ber.Packet) int64 {
	t.Helper()
	op := requestSearchOp(packet)
	if op.Tag != ApplicationAbandonRequest {
		t.Fatalf("expected AbandonRequest, got tag %d", op.Tag)
	}
	id, err := ber.ParseInt64(op.Data.Bytes())
	if err != nil {
		t.Fatalf("decoding abandon target ID: %v", err)
	}
	return id
}

func requestPagingControl(t *testing.T, packet *ber.Packet) *ControlPaging {
	t.Helper()
	if len(packet.Children) < 3 {
		t.Fatal("request has no controls element")
	}
	var found *ControlPaging
	count := 0
	for _, raw := range packet.Children[2].Children {
		control, err := DecodeControl(raw)
		if err != nil {
			t.Fatalf("decoding request control: %v", err)
		}
		if control.GetControlType() == ControlTypePaging {
			count++
			paging, ok := control.(*ControlPaging)
			if !ok {
				t.Fatalf("paging control has unexpected type %T", control)
			}
			found = paging
		}
	}
	if count != 1 {
		t.Fatalf("expected exactly one paging control, got %d", count)
	}
	return found
}

func requestFilterString(t *testing.T, packet *ber.Packet) string {
	t.Helper()
	filter, err := DecompileFilter(requestSearchOp(packet).Children[6])
	if err != nil {
		t.Fatalf("decompiling request filter: %v", err)
	}
	return filter
}

func requestAttributeNames(t *testing.T, packet *ber.Packet) []string {
	t.Helper()
	attributes := requestSearchOp(packet).Children[7].Children
	names := make([]string, 0, len(attributes))
	for _, child := range attributes {
		name, err := packetString(child)
		if err != nil {
			t.Fatalf("decoding attribute name: %v", err)
		}
		names = append(names, name)
	}
	return names
}

func requestControlOIDs(t *testing.T, packet *ber.Packet) []string {
	t.Helper()
	if len(packet.Children) < 3 {
		return nil
	}
	oids := make([]string, 0, len(packet.Children[2].Children))
	for _, raw := range packet.Children[2].Children {
		control, err := DecodeControl(raw)
		if err != nil {
			t.Fatalf("decoding request control: %v", err)
		}
		oids = append(oids, control.GetControlType())
	}
	return oids
}

func newPagedTestRequest(controls []Control) *SearchRequest {
	return NewSearchRequest(
		"dc=example,dc=com",
		ScopeWholeSubtree, NeverDerefAliases, 0, 0, false,
		"(objectClass=person)",
		[]string{"cn", "mail"},
		controls,
	)
}

func TestPagedCollection(t *testing.T) {
	// PullAndSnapshot drives a three-page dialogue including an empty page and
	// a repeated opaque cookie, and pins the snapshot semantics: later mutation
	// of the caller's request and of an already published page cannot change
	// subsequent requests or overwrite an older page.
	t.Run("PullAndSnapshot", func(t *testing.T) {
		srv, client := newPagedPipeServer(t)
		defer srv.close()
		conn := NewConn(client, false)
		conn.Start()
		defer conn.Close()

		req := newPagedTestRequest([]Control{NewControlManageDsaIT(false)})
		ps, err := conn.SearchPages(req, 1)
		if err != nil {
			t.Fatalf("SearchPages: %v", err)
		}
		// Construction sends nothing and does not touch the caller's controls.
		srv.assertNoRequest()
		if len(req.Controls) != 1 {
			t.Fatalf("caller controls mutated, got %d", len(req.Controls))
		}

		cookie := []byte{0x01, 0x02, 0xFF}
		serverErr := make(chan error, 1)
		go func() {
			// Page 1: one entry and a continuation cookie.
			first := srv.nextRequest()
			paging := requestPagingControl(t, first)
			if paging.PagingSize != 1 || len(paging.Cookie) != 0 {
				serverErr <- errors.New("first request paging control mismatch")
				return
			}
			if requestFilterString(t, first) != "(objectClass=person)" {
				serverErr <- errors.New("first request filter mismatch")
				return
			}
			if requestSearchOp(first).Children[1].Value.(int64) != int64(ScopeWholeSubtree) {
				serverErr <- errors.New("first request scope mismatch")
				return
			}
			if names := requestAttributeNames(t, first); len(names) != 2 || names[0] != "cn" || names[1] != "mail" {
				serverErr <- errors.New("first request attributes mismatch")
				return
			}
			oids := requestControlOIDs(t, first)
			if len(oids) != 2 || oids[0] != ControlTypeManageDsaIT || oids[1] != ControlTypePaging {
				serverErr <- errors.New("first request control set mismatch")
				return
			}
			id := requestMessageID(first)
			srv.send(pagedEntryPacket(id, "cn=person0,dc=example,dc=com"))
			srv.send(pagedDonePacket(id, 0, "page one", pagingControlPacket(1, cookie)))

			// Page 2: an empty-entry page carrying the SAME binary cookie.
			second := srv.nextRequest()
			secondPaging := requestPagingControl(t, second)
			if !bytes.Equal(secondPaging.Cookie, cookie) {
				serverErr <- errors.New("second request did not echo the opaque cookie")
				return
			}
			if requestFilterString(t, second) != "(objectClass=person)" {
				serverErr <- errors.New("second request filter mismatch")
				return
			}
			srv.send(pagedDonePacket(requestMessageID(second), 0, "", pagingControlPacket(1, cookie)))

			// Page 3: the request must still carry the frozen query even though
			// the caller mutated its SearchRequest, plus a referral and the
			// final empty cookie.
			third := srv.nextRequest()
			thirdPaging := requestPagingControl(t, third)
			if !bytes.Equal(thirdPaging.Cookie, cookie) {
				serverErr <- errors.New("third request lost the continuation cookie")
				return
			}
			if requestSearchOp(third).Children[0].Value.(string) != "dc=example,dc=com" {
				serverErr <- errors.New("third request base DN changed")
				return
			}
			if requestFilterString(t, third) != "(objectClass=person)" {
				serverErr <- errors.New("third request filter changed")
				return
			}
			if names := requestAttributeNames(t, third); len(names) != 2 || names[0] != "cn" {
				serverErr <- errors.New("third request attributes changed")
				return
			}
			if oids := requestControlOIDs(t, third); len(oids) != 2 {
				serverErr <- errors.New("third request controls changed")
				return
			}
			id = requestMessageID(third)
			srv.send(pagedReferencePacket(id, "ldap://ref.example/dc=example,dc=com"))
			srv.send(pagedEntryPacket(id, "cn=person2,dc=example,dc=com"))
			srv.send(pagedDonePacket(id, 0, "", pagingControlPacket(1, nil)))
			serverErr <- nil
		}()

		ctx := context.Background()
		if !ps.Next(ctx) {
			t.Fatalf("first Next: %v", ps.Err())
		}
		page1 := ps.Page()
		if len(page1.Entries) != 1 || page1.Entries[0].DN != "cn=person0,dc=example,dc=com" {
			t.Fatalf("unexpected first page: %+v", page1.Entries)
		}
		if page1.ResultCode != 0 || page1.DiagnosticMessage != "page one" {
			t.Fatalf("unexpected done fields: code=%d diag=%q", page1.ResultCode, page1.DiagnosticMessage)
		}
		firstPageCookie := page1.Controls[0].(*ControlPaging).Cookie
		if !bytes.Equal(firstPageCookie, cookie) {
			t.Fatalf("first page control cookie = %v, want %v", firstPageCookie, cookie)
		}

		if !ps.Next(ctx) {
			t.Fatalf("second Next (empty page): %v", ps.Err())
		}
		page2 := ps.Page()
		if len(page2.Entries) != 0 {
			t.Fatalf("expected empty page, got %d entries", len(page2.Entries))
		}
		if !bytes.Equal(page2.Controls[0].(*ControlPaging).Cookie, cookie) {
			t.Fatal("empty page must still publish its non-empty cookie")
		}

		// Mutate the caller's request and the previously published page.
		req.BaseDN = "dc=mutated,dc=com"
		req.Filter = "(cn=changed)"
		req.Attributes[0] = "sn"
		req.Controls = nil
		page1.Entries = append(page1.Entries, NewEntry("cn=hacker,dc=example,dc=com", nil))
		page1.ResultCode = 99

		if !ps.Next(ctx) {
			t.Fatalf("third Next: %v", ps.Err())
		}
		page3 := ps.Page()
		if len(page3.Entries) != 1 || page3.Entries[0].DN != "cn=person2,dc=example,dc=com" {
			t.Fatalf("unexpected third page entries: %+v", page3.Entries)
		}
		if len(page3.Referrals) != 1 || page3.Referrals[0] != "ldap://ref.example/dc=example,dc=com" {
			t.Fatalf("unexpected referrals: %v", page3.Referrals)
		}
		if len(page3.Controls[0].(*ControlPaging).Cookie) != 0 {
			t.Fatal("final page must carry an empty cookie")
		}
		if err := <-serverErr; err != nil {
			t.Fatal(err)
		}

		// Empty cookie: Next stops without network I/O.
		if ps.Next(ctx) {
			t.Fatal("Next after the final page must return false")
		}
		if ps.Err() != nil {
			t.Fatalf("iteration error: %v", ps.Err())
		}
		srv.assertNoRequest()

		// Already-final and repeated Close send nothing.
		if err := ps.Close(ctx); err != nil {
			t.Fatalf("Close after final page: %v", err)
		}
		if err := ps.Close(ctx); err != nil {
			t.Fatalf("repeated Close: %v", err)
		}
		srv.assertNoRequest()
		if ps.Next(ctx) {
			t.Fatal("Next after Close must return false")
		}

		// Published pages keep their identity and were never rewritten.
		if len(page1.Entries) != 2 || page1.ResultCode != 99 {
			t.Fatal("first page snapshot was touched by the library")
		}
		if page1.DiagnosticMessage != "page one" {
			t.Fatal("first page diagnostic message was lost")
		}
		if len(page2.Entries) != 0 || ps.Page() != page3 {
			t.Fatal("page snapshots were not stable")
		}
	})

	// CloseIdleCursor verifies the two termination paths: an idle cursor is
	// released with a size=0 search carrying the last cookie, while the
	// not-started and already-final cases send nothing; release failures and
	// cleanup cancellation are surfaced rather than swallowed.
	t.Run("CloseIdleCursor", func(t *testing.T) {
		t.Run("release succeeds without response control", func(t *testing.T) {
			srv, client := newPagedPipeServer(t)
			defer srv.close()
			conn := NewConn(client, false)
			conn.Start()
			defer conn.Close()

			ps, err := conn.SearchPages(newPagedTestRequest(nil), 2)
			if err != nil {
				t.Fatalf("SearchPages: %v", err)
			}
			go func() {
				first := srv.nextRequest()
				paging := requestPagingControl(t, first)
				if paging.PagingSize != 2 || len(paging.Cookie) != 0 {
					t.Errorf("first request paging mismatch: %+v", paging)
				}
				id := requestMessageID(first)
				srv.send(pagedEntryPacket(id, "cn=one,dc=example,dc=com"))
				srv.send(pagedDonePacket(id, 0, "", pagingControlPacket(2, []byte("cur"))))

				release := srv.nextRequest()
				if requestSearchOp(release).Tag != ApplicationSearchRequest {
					t.Errorf("release is not a search request, tag %d", requestSearchOp(release).Tag)
				}
				releasePaging := requestPagingControl(t, release)
				if releasePaging.PagingSize != 0 || !bytes.Equal(releasePaging.Cookie, []byte("cur")) {
					t.Errorf("release paging mismatch: %+v", releasePaging)
				}
				if requestFilterString(t, release) != "(objectClass=person)" {
					t.Error("release changed the frozen filter")
				}
				// The cleanup response is allowed to carry no paging control.
				srv.send(pagedDonePacket(requestMessageID(release), 0, "released"))
			}()

			ctx := context.Background()
			if !ps.Next(ctx) {
				t.Fatalf("Next: %v", ps.Err())
			}
			if err := ps.Close(ctx); err != nil {
				t.Fatalf("Close: %v", err)
			}
			srv.assertNoRequest()
			if err := ps.Close(ctx); err != nil {
				t.Fatalf("repeated Close: %v", err)
			}
			srv.assertNoRequest()
		})

		t.Run("release LDAP error is returned", func(t *testing.T) {
			srv, client := newPagedPipeServer(t)
			defer srv.close()
			conn := NewConn(client, false)
			conn.Start()
			defer conn.Close()

			ps, err := conn.SearchPages(newPagedTestRequest(nil), 2)
			if err != nil {
				t.Fatalf("SearchPages: %v", err)
			}
			go func() {
				first := srv.nextRequest()
				id := requestMessageID(first)
				srv.send(pagedDonePacket(id, 0, "", pagingControlPacket(2, []byte("cur"))))
				release := srv.nextRequest()
				srv.send(pagedDonePacket(requestMessageID(release), LDAPResultUnwillingToPerform, "server refuses"))
			}()

			if !ps.Next(context.Background()) {
				t.Fatalf("Next: %v", ps.Err())
			}
			err = ps.Close(context.Background())
			if err == nil {
				t.Fatal("expected release error, got nil")
			}
			if !IsErrorWithCode(err, LDAPResultUnwillingToPerform) {
				t.Fatalf("expected unwillingToPerform, got %v", err)
			}
			if ps.Next(context.Background()) {
				t.Fatal("Next after a failed Close must return false")
			}
			srv.assertNoRequest()
		})

		t.Run("release cancellation is returned", func(t *testing.T) {
			srv, client := newPagedPipeServer(t)
			defer srv.close()
			conn := NewConn(client, false)
			conn.Start()
			defer conn.Close()

			ps, err := conn.SearchPages(newPagedTestRequest(nil), 2)
			if err != nil {
				t.Fatalf("SearchPages: %v", err)
			}
			go func() {
				first := srv.nextRequest()
				id := requestMessageID(first)
				srv.send(pagedDonePacket(id, 0, "", pagingControlPacket(2, []byte("cur"))))
				// Read the release request, then never answer.
				srv.nextRequest()
			}()

			if !ps.Next(context.Background()) {
				t.Fatalf("Next: %v", ps.Err())
			}
			ctx, cancel := context.WithCancel(context.Background())
			releaseErr := make(chan error, 1)
			go func() { releaseErr <- ps.Close(ctx) }()
			cancel()
			select {
			case err := <-releaseErr:
				if !errors.Is(err, context.Canceled) {
					t.Fatalf("Close error = %v, want context.Canceled", err)
				}
			case <-time.After(5 * time.Second):
				t.Fatal("Close did not return after context cancellation")
			}
			srv.assertNoRequest()
		})

		t.Run("no release before start or after final page", func(t *testing.T) {
			srv, client := newPagedPipeServer(t)
			defer srv.close()
			conn := NewConn(client, false)
			conn.Start()
			defer conn.Close()

			idle, err := conn.SearchPages(newPagedTestRequest(nil), 2)
			if err != nil {
				t.Fatalf("SearchPages: %v", err)
			}
			if err := idle.Close(context.Background()); err != nil {
				t.Fatalf("Close before first Next: %v", err)
			}
			srv.assertNoRequest()

			finished, err := conn.SearchPages(newPagedTestRequest(nil), 2)
			if err != nil {
				t.Fatalf("SearchPages: %v", err)
			}
			go func() {
				first := srv.nextRequest()
				id := requestMessageID(first)
				srv.send(pagedEntryPacket(id, "cn=only,dc=example,dc=com"))
				srv.send(pagedDonePacket(id, 0, "", pagingControlPacket(2, nil)))
			}()
			if !finished.Next(context.Background()) {
				t.Fatalf("Next: %v", finished.Err())
			}
			if finished.Next(context.Background()) {
				t.Fatal("expected final page to end iteration")
			}
			if err := finished.Close(context.Background()); err != nil {
				t.Fatalf("Close after final page: %v", err)
			}
			srv.assertNoRequest()
		})
	})

	// CancelInFlight covers pre-send cancellation, in-flight cancellation with
	// an Abandon targeting the real message ID, and continued use of the shared
	// connection afterwards.
	t.Run("CancelInFlight", func(t *testing.T) {
		srv, client := newPagedPipeServer(t)
		defer srv.close()
		conn := NewConn(client, false)
		conn.Start()
		defer conn.Close()

		ps, err := conn.SearchPages(newPagedTestRequest(nil), 2)
		if err != nil {
			t.Fatalf("SearchPages: %v", err)
		}

		// A context cancelled before the first Next sends nothing.
		preCancelled, cancelPre := context.WithCancel(context.Background())
		cancelPre()
		if ps.Next(preCancelled) {
			t.Fatal("Next with a pre-cancelled context must return false")
		}
		if !errors.Is(ps.Err(), context.Canceled) {
			t.Fatalf("Err = %v, want context.Canceled", ps.Err())
		}
		srv.assertNoRequest()
		if err := ps.Close(context.Background()); err != nil {
			t.Fatalf("Close after pre-cancel: %v", err)
		}
		srv.assertNoRequest()

		// In-flight cancellation: the server has read the search request and is
		// holding the result set open.
		inflight, err := conn.SearchPages(newPagedTestRequest(nil), 2)
		if err != nil {
			t.Fatalf("SearchPages: %v", err)
		}
		ctx, cancel := context.WithCancel(context.Background())

		nextResult := make(chan bool, 1)
		go func() { nextResult <- inflight.Next(ctx) }()
		search := srv.nextRequest()
		if requestSearchOp(search).Tag != ApplicationSearchRequest {
			t.Fatalf("expected a search request, tag %d", requestSearchOp(search).Tag)
		}
		targetID := requestMessageID(search)

		cancel()
		if id := requestAbandonID(t, srv.nextRequest()); id != targetID {
			t.Fatalf("abandon targets %d, want in-flight message ID %d", id, targetID)
		}

		select {
		case ok := <-nextResult:
			if ok {
				t.Fatal("cancelled Next must return false")
			}
		case <-time.After(5 * time.Second):
			t.Fatal("cancelled Next did not return")
		}
		if !errors.Is(inflight.Err(), context.Canceled) {
			t.Fatalf("Err = %v, want context.Canceled", inflight.Err())
		}
		// No size=0 release with the old cookie follows an abandon.
		srv.assertNoRequest()
		if err := inflight.Close(context.Background()); err != nil {
			t.Fatalf("Close after cancel: %v", err)
		}
		srv.assertNoRequest()

		// The shared connection still serves ordinary and paged searches.
		go func() {
			plain := srv.nextRequest()
			id := requestMessageID(plain)
			srv.send(pagedEntryPacket(id, "cn=plain,dc=example,dc=com"))
			srv.send(pagedDonePacket(id, 0, ""))
		}()
		plainResult, err := conn.Search(newPagedTestRequest(nil))
		if err != nil {
			t.Fatalf("plain Search after cancel: %v", err)
		}
		if len(plainResult.Entries) != 1 || plainResult.Entries[0].DN != "cn=plain,dc=example,dc=com" {
			t.Fatalf("unexpected plain result: %+v", plainResult.Entries)
		}

		again, err := conn.SearchPages(newPagedTestRequest(nil), 2)
		if err != nil {
			t.Fatalf("SearchPages after cancel: %v", err)
		}
		go func() {
			request := srv.nextRequest()
			id := requestMessageID(request)
			srv.send(pagedEntryPacket(id, "cn=again,dc=example,dc=com"))
			srv.send(pagedDonePacket(id, 0, "", pagingControlPacket(2, nil)))
		}()
		if !again.Next(context.Background()) {
			t.Fatalf("paged Next after cancel: %v", again.Err())
		}
		if again.Page().Entries[0].DN != "cn=again,dc=example,dc=com" {
			t.Fatalf("unexpected paged result: %+v", again.Page().Entries)
		}
	})

	// PageAtomicFailure ensures a damaged or failed final response never
	// publishes a partial page: the entries already read for the failed page
	// are discarded and Page keeps the previous complete page.
	t.Run("PageAtomicFailure", func(t *testing.T) {
		cases := []struct {
			name      string
			respond   func(srv *pagedPipeServer, id int64)
			wantError string
		}{
			{
				name: "missing paging control",
				respond: func(srv *pagedPipeServer, id int64) {
					srv.send(pagedEntryPacket(id, "cn=partial,dc=example,dc=com"))
					srv.send(pagedDonePacket(id, 0, ""))
				},
				wantError: "missing a paging control",
			},
			{
				name: "duplicate paging controls",
				respond: func(srv *pagedPipeServer, id int64) {
					srv.send(pagedEntryPacket(id, "cn=partial,dc=example,dc=com"))
					srv.send(pagedDonePacket(id, 0, "",
						pagingControlPacket(2, []byte("x")),
						pagingControlPacket(2, []byte("x"))))
				},
				wantError: "2 paging controls",
			},
			{
				name: "malformed paging control",
				respond: func(srv *pagedPipeServer, id int64) {
					srv.send(pagedEntryPacket(id, "cn=partial,dc=example,dc=com"))
					srv.send(pagedDonePacket(id, 0, "", malformedPagingControlPacket()))
				},
				wantError: "child index",
			},
			{
				name: "ldap error done",
				respond: func(srv *pagedPipeServer, id int64) {
					srv.send(pagedEntryPacket(id, "cn=partial,dc=example,dc=com"))
					srv.send(pagedDonePacket(id, LDAPResultOperationsError, "boom",
						pagingControlPacket(2, []byte("x"))))
				},
				wantError: "LDAP Result Code 1",
			},
		}

		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				srv, client := newPagedPipeServer(t)
				defer srv.close()
				conn := NewConn(client, false)
				conn.Start()
				defer conn.Close()

				ps, err := conn.SearchPages(newPagedTestRequest(nil), 2)
				if err != nil {
					t.Fatalf("SearchPages: %v", err)
				}
				go func() {
					first := srv.nextRequest()
					firstID := requestMessageID(first)
					srv.send(pagedEntryPacket(firstID, "cn=good,dc=example,dc=com"))
					srv.send(pagedDonePacket(firstID, 0, "good page", pagingControlPacket(2, []byte("cur"))))

					second := srv.nextRequest()
					tc.respond(srv, requestMessageID(second))
				}()

				ctx := context.Background()
				if !ps.Next(ctx) {
					t.Fatalf("first Next: %v", ps.Err())
				}
				goodPage := ps.Page()
				if len(goodPage.Entries) != 1 || goodPage.Entries[0].DN != "cn=good,dc=example,dc=com" {
					t.Fatalf("unexpected good page: %+v", goodPage.Entries)
				}

				if ps.Next(ctx) {
					t.Fatal("failing Next must return false")
				}
				if ps.Err() == nil || !strings.Contains(ps.Err().Error(), tc.wantError) {
					t.Fatalf("Err = %v, want substring %q", ps.Err(), tc.wantError)
				}
				// The partial page was not published.
				if ps.Page() != goodPage {
					t.Fatal("Page changed after a failed page")
				}
				if len(ps.Page().Entries) != 1 || ps.Page().Entries[0].DN != "cn=good,dc=example,dc=com" {
					t.Fatal("previous successful page was overwritten")
				}
				if ps.Page().DiagnosticMessage != "good page" {
					t.Fatal("previous page diagnostic was overwritten")
				}
				if ps.Next(ctx) {
					t.Fatal("Next after termination must return false")
				}
				// A terminated iteration performs no cleanup request.
				if err := ps.Close(ctx); err != nil {
					t.Fatalf("Close after failure: %v", err)
				}
				srv.assertNoRequest()
			})
		}
	})

	// SharedConnection runs two PagedSearch values over one Conn and one shared
	// SearchRequest with interleaved pages, then cancels a third one and proves
	// its late packets cannot reach a fourth query or a plain Search.
	t.Run("SharedConnection", func(t *testing.T) {
		srv, client := newPagedPipeServer(t)
		defer srv.close()
		conn := NewConn(client, false)
		conn.Start()
		defer conn.Close()

		sharedReq := newPagedTestRequest([]Control{NewControlPaging(1)})
		psA, err := conn.SearchPages(sharedReq, 1)
		if err != nil {
			t.Fatalf("SearchPages A: %v", err)
		}
		psB, err := conn.SearchPages(sharedReq, 1)
		if err != nil {
			t.Fatalf("SearchPages B: %v", err)
		}

		// Server state machine keyed on the request cookie: two independent
		// cursors, each with an empty and a non-empty cookie stage.
		go func() {
			emptySeen := 0
			for range 4 {
				request := srv.nextRequest()
				paging := requestPagingControl(t, request)
				id := requestMessageID(request)
				switch {
				case len(paging.Cookie) == 0:
					emptySeen++
					if emptySeen == 1 {
						srv.send(pagedEntryPacket(id, "cn=A1,dc=example,dc=com"))
						srv.send(pagedDonePacket(id, 0, "", pagingControlPacket(1, []byte("a1"))))
					} else {
						srv.send(pagedEntryPacket(id, "cn=B1,dc=example,dc=com"))
						srv.send(pagedDonePacket(id, 0, "", pagingControlPacket(1, []byte("b1"))))
					}
				case bytes.Equal(paging.Cookie, []byte("a1")):
					srv.send(pagedEntryPacket(id, "cn=A2,dc=example,dc=com"))
					srv.send(pagedDonePacket(id, 0, "", pagingControlPacket(1, nil)))
				case bytes.Equal(paging.Cookie, []byte("b1")):
					srv.send(pagedEntryPacket(id, "cn=B2,dc=example,dc=com"))
					srv.send(pagedDonePacket(id, 0, "", pagingControlPacket(1, nil)))
				default:
					t.Errorf("unexpected cookie %q", paging.Cookie)
					return
				}
			}
		}()

		ctx := context.Background()
		if !psA.Next(ctx) {
			t.Fatalf("A first Next: %v", psA.Err())
		}
		if !psB.Next(ctx) {
			t.Fatalf("B first Next: %v", psB.Err())
		}
		if psA.Page().Entries[0].DN != "cn=A1,dc=example,dc=com" ||
			psB.Page().Entries[0].DN != "cn=B1,dc=example,dc=com" {
			t.Fatal("first pages were swapped between the two searches")
		}
		if !psA.Next(ctx) {
			t.Fatalf("A second Next: %v", psA.Err())
		}
		if !psB.Next(ctx) {
			t.Fatalf("B second Next: %v", psB.Err())
		}
		if psA.Page().Entries[0].DN != "cn=A2,dc=example,dc=com" ||
			psB.Page().Entries[0].DN != "cn=B2,dc=example,dc=com" {
			t.Fatal("continuation cookies were shared between cursors")
		}
		if psA.Next(ctx) || psB.Next(ctx) {
			t.Fatal("both iterations must end after the empty cookie")
		}
		if len(sharedReq.Controls) != 1 || len(sharedReq.Controls[0].(*ControlPaging).Cookie) != 0 {
			t.Fatal("the caller's shared paging control was mutated")
		}

		// Cancel a third cursor, then deliver late packets for its message ID.
		psC, err := conn.SearchPages(newPagedTestRequest(nil), 1)
		if err != nil {
			t.Fatalf("SearchPages C: %v", err)
		}
		cancelCtx, cancel := context.WithCancel(context.Background())
		cDone := make(chan bool, 1)
		go func() { cDone <- psC.Next(cancelCtx) }()
		cRequest := srv.nextRequest()
		cID := requestMessageID(cRequest)
		cancel()
		if id := requestAbandonID(t, srv.nextRequest()); id != cID {
			t.Fatalf("abandon targets %d, want in-flight message ID %d", id, cID)
		}
		<-cDone
		// Late responses for the abandoned message must not land anywhere.
		srv.send(pagedEntryPacket(cID, "cn=LATE,dc=example,dc=com"))
		srv.send(pagedDonePacket(cID, 0, "", pagingControlPacket(1, nil)))

		// A fourth paged search over the same connection sees only its own page.
		psD, err := conn.SearchPages(newPagedTestRequest(nil), 1)
		if err != nil {
			t.Fatalf("SearchPages D: %v", err)
		}
		go func() {
			request := srv.nextRequest()
			if requestMessageID(request) == cID {
				t.Error("new search reused the abandoned message ID")
			}
			id := requestMessageID(request)
			srv.send(pagedEntryPacket(id, "cn=D1,dc=example,dc=com"))
			srv.send(pagedDonePacket(id, 0, "", pagingControlPacket(1, nil)))
		}()
		if !psD.Next(context.Background()) {
			t.Fatalf("D Next: %v", psD.Err())
		}
		if len(psD.Page().Entries) != 1 || psD.Page().Entries[0].DN != "cn=D1,dc=example,dc=com" {
			t.Fatalf("late packet leaked into the new paged search: %+v", psD.Page().Entries)
		}

		go func() {
			request := srv.nextRequest()
			id := requestMessageID(request)
			srv.send(pagedEntryPacket(id, "cn=E1,dc=example,dc=com"))
			srv.send(pagedDonePacket(id, 0, ""))
		}()
		plain, err := conn.Search(newPagedTestRequest(nil))
		if err != nil {
			t.Fatalf("plain Search: %v", err)
		}
		if len(plain.Entries) != 1 || plain.Entries[0].DN != "cn=E1,dc=example,dc=com" {
			t.Fatalf("late packet leaked into the plain search: %+v", plain.Entries)
		}
	})

	// InvalidInput pins every construction-time rejection and proves none of
	// them reach the wire.
	t.Run("InvalidInput", func(t *testing.T) {
		srv, client := newPagedPipeServer(t)
		defer srv.close()
		conn := NewConn(client, false)
		conn.Start()
		defer conn.Close()

		tests := []struct {
			name     string
			req      *SearchRequest
			pageSize uint32
		}{
			{"nil request", nil, 1},
			{"zero page size", newPagedTestRequest(nil), 0},
			{"page size over RFC 2696 maxInt", newPagedTestRequest(nil), 2147483648},
			{"duplicate paging controls", newPagedTestRequest([]Control{
				NewControlPaging(1), NewControlPaging(1),
			}), 1},
			{"same OID different control type", newPagedTestRequest([]Control{
				wrongPagingControl{},
			}), 1},
			{"paging size conflict", newPagedTestRequest([]Control{
				NewControlPaging(2),
			}), 1},
			{"non-empty initial cookie", func() *SearchRequest {
				control := NewControlPaging(1)
				control.SetCookie([]byte("stale"))
				return newPagedTestRequest([]Control{control})
			}(), 1},
			{"uncompilable filter", NewSearchRequest("dc=example,dc=com", ScopeWholeSubtree,
				NeverDerefAliases, 0, 0, false, "(broken", nil, nil), 1},
			{"nil control in list", newPagedTestRequest([]Control{nil}), 1},
		}

		for _, tc := range tests {
			t.Run(tc.name, func(t *testing.T) {
				ps, err := conn.SearchPages(tc.req, tc.pageSize)
				if err == nil {
					t.Fatalf("expected construction error, got %v", ps)
				}
				if ps != nil {
					t.Fatal("failed construction must return a nil PagedSearch")
				}
			})
		}

		// Boundary-valid sizes and the allowed control arrangement construct.
		for _, size := range []uint32{1, 2147483647} {
			if _, err := conn.SearchPages(newPagedTestRequest(nil), size); err != nil {
				t.Fatalf("page size %d rejected: %v", size, err)
			}
		}
		if _, err := conn.SearchPages(newPagedTestRequest([]Control{NewControlPaging(3)}), 3); err != nil {
			t.Fatalf("matching empty-cookie paging control rejected: %v", err)
		}
		srv.assertNoRequest()
	})
}
