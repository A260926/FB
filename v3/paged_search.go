package ldap

import (
	"context"
	"errors"
	"fmt"
	"math"

	ber "github.com/go-asn1-ber/asn1-ber"
)

// maxPagedSearchPageSize is the RFC 2696 size bound: the paging control
// carries an LDAP INTEGER (signed), so sizes above maxInt cannot be encoded.
const maxPagedSearchPageSize uint32 = math.MaxInt32

// pagedSearchState is the lifecycle state of a PagedSearch. Transitions only
// move forward; the methods of one PagedSearch are called sequentially from a
// single goroutine, so the state needs no additional locking.
type pagedSearchState int

const (
	// pagedSearchNew means no SearchRequest has been sent yet.
	pagedSearchNew pagedSearchState = iota
	// pagedSearchRunning means at least one full page was published and the
	// last successful response carried a non-empty continuation cookie, so the
	// server may still hold a result set.
	pagedSearchRunning
	// pagedSearchFinal means the last published page carried an empty cookie.
	pagedSearchFinal
	// pagedSearchFailed means iteration terminated with an error or a
	// cancelled context.
	pagedSearchFailed
	// pagedSearchClosed means Close ran (or ran to completion).
	pagedSearchClosed
)

// pagedSearchTemplate is the immutable, wire-ready snapshot of the query made
// when SearchPages is called. Later mutations of the caller's SearchRequest,
// its controls or the pages already returned cannot affect subsequent
// requests: scalar fields are copied, the filter is compiled once, the
// attribute list is copied, and every non-paging control is encoded once.
type pagedSearchTemplate struct {
	baseDN           string
	scope            int
	derefAliases     int
	sizeLimit        int
	timeLimit        int
	typesOnly        bool
	filterPacket     *ber.Packet
	attributes       []string
	controlPackets   []*ber.Packet
	enforceSizeLimit bool
	pageSize         uint32
}

// appendTo encodes one paged SearchRequest into the given LDAP envelope,
// appending the frozen controls followed by the paging control built for this
// request. It mirrors SearchRequest.appendTo for the fixed fields.
func (t *pagedSearchTemplate) appendTo(envelope *ber.Packet, paging *ControlPaging) error {
	pkt := ber.Encode(ber.ClassApplication, ber.TypeConstructed, ApplicationSearchRequest, nil, "Search Request")
	pkt.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, t.baseDN, "Base DN"))
	pkt.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimitive, ber.TagEnumerated, uint64(t.scope), "Scope"))
	pkt.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimitive, ber.TagEnumerated, uint64(t.derefAliases), "Deref Aliases"))
	pkt.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimitive, ber.TagInteger, uint64(t.sizeLimit), "Size Limit"))
	pkt.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimitive, ber.TagInteger, uint64(t.timeLimit), "Time Limit"))
	pkt.AppendChild(ber.NewBoolean(ber.ClassUniversal, ber.TypePrimitive, ber.TagBoolean, t.typesOnly, "Types Only"))
	pkt.AppendChild(t.filterPacket)

	attributesPacket := ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "Attributes")
	for _, attribute := range t.attributes {
		attributesPacket.AppendChild(ber.NewString(ber.ClassUniversal, ber.TypePrimitive, ber.TagOctetString, attribute, "Attribute"))
	}
	pkt.AppendChild(attributesPacket)

	envelope.AppendChild(pkt)

	controls := ber.Encode(ber.ClassContext, ber.TypeConstructed, 0, nil, "Controls")
	for _, controlPacket := range t.controlPackets {
		controls.AppendChild(controlPacket)
	}
	controls.AppendChild(paging.Encode())
	envelope.AppendChild(controls)

	return nil
}

type pagedSearchRequest struct {
	template *pagedSearchTemplate
	paging   *ControlPaging
}

func (r *pagedSearchRequest) appendTo(envelope *ber.Packet) error {
	return r.template.appendTo(envelope, r.paging)
}

// PagedSearch iterates an RFC 2696 paged search one page at a time. Each call
// to Next sends exactly one paged SearchRequest and publishes at most one
// Page after the complete page, including its SearchResultDone, has been
// validated. Pages are never pre-read or accumulated internally.
//
// A PagedSearch is not safe for concurrent method calls: its Next, Page, Err
// and Close methods are used sequentially by a single goroutine. Distinct
// PagedSearch values may share one Conn. The underlying Conn is never closed
// by a PagedSearch, and its timeout/deadline settings are never changed.
type PagedSearch struct {
	conn   *Conn
	tmpl   *pagedSearchTemplate
	state  pagedSearchState
	cookie []byte
	page   *SearchResult
	err    error
}

// SearchPages prepares a page-by-page paged search for req. It performs no
// network I/O: the query (attributes, filter, scope and control encodings) is
// validated and snapshotted, and the first request is sent by the first Next
// call.
//
// req may carry no paging control, in which case one with pageSize is added,
// or exactly one *ControlPaging with the same pageSize and an empty cookie.
// Every other control arrangement, a nil request, an out-of-range pageSize
// (1..2147483647) and a filter that cannot be compiled are rejected before any
// packet is sent. The caller's request and controls are not modified.
func (l *Conn) SearchPages(req *SearchRequest, pageSize uint32) (*PagedSearch, error) {
	if req == nil {
		return nil, errors.New("ldap: SearchPages called with nil SearchRequest")
	}
	if pageSize < 1 || pageSize > maxPagedSearchPageSize {
		return nil, fmt.Errorf("ldap: paged search page size %d out of range 1..%d", pageSize, maxPagedSearchPageSize)
	}

	pagingControls := 0
	for _, control := range req.Controls {
		if control == nil {
			return nil, errors.New("ldap: SearchPages request contains a nil control")
		}
		if control.GetControlType() != ControlTypePaging {
			continue
		}
		pagingControls++
		paging, ok := control.(*ControlPaging)
		if !ok {
			return nil, fmt.Errorf("ldap: expected paging control to be of type *ControlPaging, got %T", control)
		}
		if paging.PagingSize != pageSize {
			return nil, fmt.Errorf("ldap: paging size given in search request (%d) conflicts with size given in SearchPages (%d)", paging.PagingSize, pageSize)
		}
		if len(paging.Cookie) > 0 {
			return nil, errors.New("ldap: SearchPages request must not carry an initial paging cookie")
		}
	}
	if pagingControls > 1 {
		return nil, fmt.Errorf("ldap: SearchPages request contains %d paging controls, expected at most one", pagingControls)
	}

	// Compile the filter now both to freeze its wire encoding and to surface
	// filter errors before the first packet is sent.
	filterPacket, err := CompileFilter(req.Filter)
	if err != nil {
		return nil, err
	}

	tmpl := &pagedSearchTemplate{
		baseDN:           req.BaseDN,
		scope:            req.Scope,
		derefAliases:     req.DerefAliases,
		sizeLimit:        req.SizeLimit,
		timeLimit:        req.TimeLimit,
		typesOnly:        req.TypesOnly,
		filterPacket:     filterPacket,
		attributes:       append([]string(nil), req.Attributes...),
		enforceSizeLimit: req.EnforceSizeLimit,
		pageSize:         pageSize,
	}
	for _, control := range req.Controls {
		if control.GetControlType() == ControlTypePaging {
			continue
		}
		// Encode once: the packet, not the caller's control object, is used
		// for every subsequent request.
		tmpl.controlPackets = append(tmpl.controlPackets, control.Encode())
	}

	return &PagedSearch{
		conn:  l,
		tmpl:  tmpl,
		state: pagedSearchNew,
	}, nil
}

// newRequest builds the single SearchRequest for the next page. The paging
// control is freshly created per request and carries the opaque cookie bytes
// from the last complete successful page.
func (p *PagedSearch) newRequest(size uint32, cookie []byte) *pagedSearchRequest {
	control := NewControlPaging(size)
	control.SetCookie(append([]byte(nil), cookie...))
	return &pagedSearchRequest{template: p.tmpl, paging: control}
}

// Next sends one paged SearchRequest and waits for that request's
// SearchResultDone. It returns true when a complete page is available via
// Page; an empty-entry page still returns true when it carries a continuation
// cookie. After a page with an empty cookie, Next returns false without
// further network I/O.
//
// If ctx is already cancelled when Next is entered, no packet is sent and the
// iteration terminates with ctx.Err(). If ctx is cancelled while the request
// is in flight, Next discards the unfinished page, sends an LDAP Abandon for
// the in-flight message ID and terminates with ctx.Err(); no release request
// using the previous page's cookie is sent afterwards.
func (p *PagedSearch) Next(ctx context.Context) bool {
	if ctx == nil {
		ctx = context.Background()
	}
	switch p.state {
	case pagedSearchFinal, pagedSearchFailed, pagedSearchClosed:
		return false
	}
	if err := ctx.Err(); err != nil {
		p.terminate(err)
		return false
	}

	msgCtx, err := p.conn.doRequest(p.newRequest(p.tmpl.pageSize, p.cookie))
	if err != nil {
		p.terminate(err)
		return false
	}

	result := &SearchResult{
		Entries:   make([]*Entry, 0),
		Referrals: make([]string, 0),
		Controls:  make([]Control, 0),
	}
	for {
		select {
		case <-ctx.Done():
			// The request is in flight (or queued ahead of the writer): tell
			// the server to abandon the actual message ID, drop anything still
			// buffered for it and stop. Abandon has no response, so we do not
			// wait for one and we do not reuse the old cookie for a size=0
			// release, which the server may already have invalidated.
			_ = p.conn.sendAbandon(msgCtx.id)
			p.discardResponses(msgCtx)
			p.conn.finishMessage(msgCtx)
			p.terminate(ctx.Err())
			return false
		case packetResponse, ok := <-msgCtx.responses:
			if !ok {
				p.conn.finishMessage(msgCtx)
				p.terminate(NewError(ErrorNetwork, errRespChanClosed))
				return false
			}
			packet, err := packetResponse.ReadPacket()
			if err != nil {
				p.conn.finishMessage(msgCtx)
				p.terminate(err)
				return false
			}

			done, err := p.handlePacket(result, packet)
			if err != nil {
				p.conn.finishMessage(msgCtx)
				p.terminate(err)
				return false
			}
			if !done {
				continue
			}

			paging, err := singlePagingControl(result.Controls)
			if err != nil {
				p.conn.finishMessage(msgCtx)
				p.terminate(err)
				return false
			}

			p.conn.finishMessage(msgCtx)

			// Publish only after the full page and its control have passed all
			// validation; earlier pages stay reachable via Page on failure.
			p.page = result
			p.cookie = append([]byte(nil), paging.Cookie...)
			if len(paging.Cookie) == 0 {
				p.state = pagedSearchFinal
			} else {
				p.state = pagedSearchRunning
			}
			return true
		}
	}
}

// handlePacket decodes one response packet of the in-flight search, mirroring
// the entry, referral and result handling of Conn.Search. It reports done true
// only when a successful SearchResultDone has been received and decoded.
func (p *PagedSearch) handlePacket(result *SearchResult, packet *ber.Packet) (done bool, err error) {
	protocolOp, err := packetChild(packet, 1)
	if err != nil {
		return false, err
	}

	switch protocolOp.Tag {
	case ApplicationSearchResultEntry:
		if p.tmpl.enforceSizeLimit &&
			p.tmpl.sizeLimit > 0 &&
			len(result.Entries) >= p.tmpl.sizeLimit {
			return false, ErrSizeLimitExceeded
		}
		attributesChild, err := packetChild(protocolOp, 1)
		if err != nil {
			return false, err
		}
		attributes, err := unpackAttributes(attributesChild.Children)
		if err != nil {
			return false, err
		}
		dn, err := packetStringAt(protocolOp, 0)
		if err != nil {
			return false, err
		}
		result.Entries = append(result.Entries, &Entry{DN: dn, Attributes: attributes})
		return false, nil
	case ApplicationSearchResultDone:
		rc, _, diag, parseErr := parseLDAPResult(packet)
		if parseErr != nil {
			return false, NewError(ErrorNetwork, fmt.Errorf("malformed SearchResultDone: %w", parseErr))
		}
		result.ResultCode = rc
		result.DiagnosticMessage = diag
		if err := GetLDAPError(packet); err != nil {
			return false, err
		}
		controlsChild, ok, err := packetChildIfPresent(packet, 2)
		if err != nil {
			return false, err
		}
		if ok {
			for _, child := range controlsChild.Children {
				decoded, err := DecodeControl(child)
				if err != nil {
					return false, fmt.Errorf("failed to decode child control: %w", err)
				}
				result.Controls = append(result.Controls, decoded)
			}
		}
		return true, nil
	case ApplicationSearchResultReference:
		ref, err := packetStringAt(protocolOp, 0)
		if err != nil {
			return false, err
		}
		result.Referrals = append(result.Referrals, ref)
		return false, nil
	default:
		return false, fmt.Errorf("ldap: unexpected protocol op tag %d in paged search response", protocolOp.Tag)
	}
}

// singlePagingControl validates that the SearchResultDone carried exactly one
// well-formed paging control. A missing, duplicated or damaged control fails
// the whole page instead of silently ending the iteration.
func singlePagingControl(controls []Control) (*ControlPaging, error) {
	var found []*ControlPaging
	for _, control := range controls {
		if control.GetControlType() != ControlTypePaging {
			continue
		}
		paging, ok := control.(*ControlPaging)
		if !ok {
			return nil, fmt.Errorf("ldap: paged search response paging control has unexpected type %T", control)
		}
		found = append(found, paging)
	}
	switch len(found) {
	case 0:
		return nil, errors.New("ldap: paged search response is missing a paging control")
	case 1:
		return found[0], nil
	default:
		return nil, fmt.Errorf("ldap: paged search response contains %d paging controls, expected one", len(found))
	}
}

// Page returns the last complete successful page. The returned value is owned
// by the caller: later calls never mutate it, and mutating it does not affect
// the PagedSearch. It returns nil before the first successful page.
func (p *PagedSearch) Page() *SearchResult {
	return p.page
}

// Err returns the error that terminated the iteration, or nil.
func (p *PagedSearch) Err() error {
	return p.err
}

// Close releases the server-side paging result set when iteration stopped
// between complete pages: it sends one SearchRequest with paging size 0 and
// the most recent valid cookie, then reads that request's SearchResultDone.
// The cleanup response is allowed to omit a paging control; LDAP errors,
// transport errors and ctx cancellation are returned, not swallowed.
//
// No cleanup request is sent when no page was ever requested, the final page
// was reached, the iteration already failed (including cancellation) or Close
// was already called. Repeated calls do not send another packet; after Close,
// Next always returns false. The shared Conn is left open.
func (p *PagedSearch) Close(ctx context.Context) error {
	if ctx == nil {
		ctx = context.Background()
	}
	if p.state == pagedSearchClosed {
		return nil
	}

	var err error
	if p.state == pagedSearchRunning {
		err = p.releaseCursor(ctx)
	}
	p.state = pagedSearchClosed
	return err
}

// releaseCursor sends the RFC 2696 abandonment search (size 0, last cookie)
// and waits for its SearchResultDone.
func (p *PagedSearch) releaseCursor(ctx context.Context) error {
	msgCtx, err := p.conn.doRequest(p.newRequest(0, p.cookie))
	if err != nil {
		return err
	}
	defer p.conn.finishMessage(msgCtx)

	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case packetResponse, ok := <-msgCtx.responses:
			if !ok {
				return NewError(ErrorNetwork, errRespChanClosed)
			}
			packet, err := packetResponse.ReadPacket()
			if err != nil {
				return err
			}
			protocolOp, err := packetChild(packet, 1)
			if err != nil {
				return err
			}
			if protocolOp.Tag != ApplicationSearchResultDone {
				// Entries and referrals of the cleanup search are discarded.
				continue
			}
			// The cleanup response need not carry another paging control, so
			// only the LDAP result itself is validated here.
			return GetLDAPError(packet)
		}
	}
}

func (p *PagedSearch) terminate(err error) {
	p.err = err
	p.state = pagedSearchFailed
}

// discardResponses drops packets already delivered for an abandoned message
// without blocking. It never waits: Abandon has no response and the message is
// finished immediately afterwards.
func (p *PagedSearch) discardResponses(msgCtx *messageContext) {
	for {
		select {
		case _, ok := <-msgCtx.responses:
			if !ok {
				return
			}
		default:
			return
		}
	}
}

// sendAbandon writes an LDAP AbandonRequest targeting messageID. The request
// has no response (RFC 4511 section 4.11), so its message context is finished
// immediately after queueing the packet. Both packets travel through the
// connection's single message-processing goroutine, which preserves the
// ordering of the abandoned request ahead of the abandon.
func (l *Conn) sendAbandon(messageID int64) error {
	packet := ber.Encode(ber.ClassUniversal, ber.TypeConstructed, ber.TagSequence, nil, "LDAP Request")
	packet.AppendChild(ber.NewInteger(ber.ClassUniversal, ber.TypePrimitive, ber.TagInteger, l.nextMessageID(), "MessageID"))
	packet.AppendChild(ber.NewInteger(ber.ClassApplication, ber.TypePrimitive, ber.Tag(ApplicationAbandonRequest), messageID, "Abandon Request"))

	msgCtx, err := l.sendMessage(packet)
	if err != nil {
		return err
	}
	l.finishMessage(msgCtx)
	return nil
}
