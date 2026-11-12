package ldap

import (
	"context"
	"fmt"
	"net"

	ber "github.com/go-asn1-ber/asn1-ber"
)

// ExampleConn_SearchPages shows page-by-page collection against a local
// protocol peer: the client processes one page, then Close releases the
// server-side paging cursor with an RFC 2696 size=0 search. The peer below
// decodes the real BER packets the library puts on the wire.
func ExampleConn_SearchPages() {
	clientConn, serverConn := net.Pipe()
	defer serverConn.Close()

	// Minimal in-process LDAP peer: answer the first paged search with one
	// entry and a continuation cookie, then answer the size=0 release search.
	go func() {
		// Page request.
		request, err := ber.ReadPacket(serverConn)
		if err != nil {
			return
		}
		messageID := request.Children[0].Value.(int64)

		entry := ber.Encode(ber.ClassApplication, ber.TypeConstructed, ApplicationSearchResultEntry, nil, "")
		entry.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, "cn=one,dc=example,dc=com", ""))
		entry.AppendChild(ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, ""))
		if _, err := serverConn.Write(pagedExampleEnvelope(messageID, entry).Bytes()); err != nil {
			return
		}

		done := pagedExampleDone(messageID)
		controls := ber.Encode(ber.ClassContext, ber.TypeConstructed, 0, nil, "")
		paging := NewControlPaging(1)
		paging.SetCookie([]byte("cursor-1"))
		controls.AppendChild(paging.Encode())
		done.AppendChild(controls)
		if _, err := serverConn.Write(done.Bytes()); err != nil {
			return
		}

		// Cursor release request (paging size 0): a SearchResultDone without
		// a paging control is a sufficient answer.
		release, err := ber.ReadPacket(serverConn)
		if err != nil {
			return
		}
		releaseID := release.Children[0].Value.(int64)
		if _, err := serverConn.Write(pagedExampleDone(releaseID).Bytes()); err != nil {
			return
		}
	}()

	conn := NewConn(clientConn, false)
	conn.Start()
	defer conn.Close()

	request := NewSearchRequest(
		"dc=example,dc=com",
		ScopeWholeSubtree, NeverDerefAliases, 0, 0, false,
		"(objectClass=person)",
		[]string{"cn"},
		nil,
	)

	paged, err := conn.SearchPages(request, 1)
	if err != nil {
		fmt.Println("error:", err)
		return
	}

	ctx := context.Background()
	if paged.Next(ctx) {
		fmt.Printf("page entries=%d\n", len(paged.Page().Entries))
	}
	if err := paged.Close(ctx); err != nil {
		fmt.Println("error:", err)
		return
	}
	fmt.Println("cursor released")

	// Output:
	// page entries=1
	// cursor released
}

func pagedExampleEnvelope(messageID int64, op *ber.Packet) *ber.Packet {
	envelope := ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "")
	envelope.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimitive, ber.TagInteger, messageID, ""))
	envelope.AppendChild(op)
	return envelope
}

func pagedExampleDone(messageID int64) *ber.Packet {
	op := ber.Encode(ber.ClassApplication, ber.TypeConstructed, ApplicationSearchResultDone, nil, "")
	op.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimitive, ber.TagEnumerated, int64(LDAPResultSuccess), ""))
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, "", ""))
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, "", ""))
	return pagedExampleEnvelope(messageID, op)
}
