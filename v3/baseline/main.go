package main

import (
	"bytes"
	"fmt"
	"net"
	"time"

	ber "github.com/go-asn1-ber/asn1-ber"
	ldap "github.com/go-ldap/ldap/v3"
)

func check(ok bool, message string) {
	if !ok {
		panic(message)
	}
}

func envelope(id int64, op *ber.Packet) *ber.Packet {
	p := ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "")
	p.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimitive, ber.TagInteger, id, ""))
	p.AppendChild(op)
	return p
}

func paging(p *ber.Packet) *ldap.ControlPaging {
	check(len(p.Children) == 3, "expected request controls")
	var found *ldap.ControlPaging
	for _, raw := range p.Children[2].Children {
		c, err := ldap.DecodeControl(raw)
		check(err == nil, "request control decode failed")
		if c.GetControlType() == ldap.ControlTypePaging {
			found = c.(*ldap.ControlPaging)
		}
	}
	check(found != nil, "missing paging control")
	return found
}

func respond(server net.Conn, id int64, dn string, cookie []byte) error {
	op := ber.Encode(ber.ClassApplication, ber.TypeConstructed, ldap.ApplicationSearchResultEntry, nil, "")
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, dn, ""))
	op.AppendChild(ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, ""))
	if _, err := server.Write(envelope(id, op).Bytes()); err != nil {
		return err
	}
	op = ber.Encode(ber.ClassApplication, ber.TypeConstructed, ldap.ApplicationSearchResultDone, nil, "")
	op.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimitive, ber.TagEnumerated, 0, ""))
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, "", ""))
	op.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, "", ""))
	p := envelope(id, op)
	controls := ber.Encode(ber.ClassContext, ber.TypeConstructed, 0, nil, "")
	c := ldap.NewControlPaging(2)
	c.SetCookie(cookie)
	controls.AppendChild(c.Encode())
	p.AppendChild(controls)
	_, err := server.Write(p.Bytes())
	return err
}

func main() {
	client, server := net.Pipe()
	defer server.Close()
	// Only a hang guard; the dialogue itself is synchronized by wire reads.
	check(server.SetDeadline(time.Now().Add(10*time.Second)) == nil, "deadline")
	c := ldap.NewConn(client, false)
	c.Start()
	defer c.Close()
	c.SetTimeout(5 * time.Second)
	cookie := []byte{0, 255, 1}
	finished := make(chan error, 1)
	go func() {
		for i := 0; i < 2; i++ {
			p, err := ber.ReadPacket(server)
			if err != nil {
				finished <- err
				return
			}
			check(p.Children[1].Tag == ldap.ApplicationSearchRequest, "expected search")
			page := paging(p)
			check(page.PagingSize == 1, "page size")
			if i == 0 {
				check(len(page.Cookie) == 0, "initial cookie")
			} else {
				check(bytes.Equal(page.Cookie, cookie), "opaque continuation changed")
			}
			next := cookie
			if i == 1 {
				next = nil
			}
			if err := respond(server, p.Children[0].Value.(int64), fmt.Sprintf("cn=person%d,dc=example", i), next); err != nil {
				finished <- err
				return
			}
		}
		finished <- nil
	}()
	control := ldap.NewControlPaging(1)
	req := ldap.NewSearchRequest("dc=example", ldap.ScopeWholeSubtree, ldap.NeverDerefAliases,
		0, 0, false, "(objectClass=person)", []string{"cn"}, []ldap.Control{control})
	result, err := c.SearchWithPaging(req, 1)
	check(err == nil, fmt.Sprintf("search error: %v", err))
	check(<-finished == nil, "wire dialogue failed")
	check(len(result.Entries) == 2, "expected both pages before return")
	check(result.Entries[0].DN == "cn=person0,dc=example" && result.Entries[1].DN == "cn=person1,dc=example", "entry order")
	check(bytes.Equal(control.Cookie, cookie), "caller paging control no longer demonstrates mutation")
	check(len(result.Controls) == 2, "expected controls from both pages")
	check(bytes.Equal(result.Controls[0].(*ldap.ControlPaging).Cookie, cookie), "first page cookie")
	check(len(result.Controls[1].(*ldap.ControlPaging).Cookie) == 0, "last page cookie")
	fmt.Println("LDAP_PAGING_BASELINE_OK pages=2 entries=2 opaque_cookie=preserved caller_cookie=changed")
}
