# gos7
Implementation of Siemens S7 protocol in golang

Overview
-------------------
For years, numerous drivers/connectors, available in both commercial and open source domains, have supported the connection to S7 family PLC devices. GoS7 fills the gaps in the S7 protocol, implementing it with pure Go (also known as golang). There is a strong belief that low-level communication should be implemented with a low-level programming language that is close to binary and memory.

The minimum supported Go version is 1.21 (see `go.mod`).

Functions
-------------------
AG:
*   Read/Write Data Block (DB) (tested)
*   Read/Write Merkers(MB) (tested)
*   Read/Write IPI (EB) (tested)
*   Read/Write IPU (AB) (tested)
*   Read/Write Timer (TM)  (tested)
*   Read/Write Counter (CT) (tested)
*   Multiple Read/Write Area (tested)
*   Get Block Info (tested)

PG:
*   Hot start/Cold start / Stop PLC
*   Get CPU of PLC status (tested)
*   List available blocks in PLC (tested)
*   Set/Clear password for session
*   Get CPU protection and CPU Order code
*   Get CPU/CP Information (tested)
*   Read/Write clock for the PLC
Helpers:
*   Get/set value for a byte array for types: value(bit/int/word/dword/uint...), real, time, counter

Supported communication
-----------------
*   TCP
*   Serial (PPI, MPI) (under construction)

How to:
----------
following is a simple usage to connect with PLC via TCP
```go
const (
	tcpDevice = "127.0.0.1"
	rack      = 0
	slot      = 2
)
// TCPClient
handler := gos7.NewTCPClientHandler(tcpDevice, rack, slot)
handler.Timeout = 200 * time.Second
handler.IdleTimeout = 200 * time.Second
handler.Logger = log.New(os.Stdout, "tcp: ", log.LstdFlags)
// Connect manually so that multiple requests are handled in one connection session
handler.Connect()
defer handler.Close()
//init client
client := gos7.NewClient(handler)
address := 2710
start := 8
size := 2
buffer := make([]byte, 255)
value := 100
//AGWriteDB to address DB2710 with value 100, start from position 8 with size = 2 (for an integer)
var helper gos7.Helper
helper.SetValueAt(buffer, 0, value)  
err := client.AGWriteDB(address, start, size, buffer)
buf := make([]byte, 255)
//AGReadDB to address DB2710, start from position 8 with size = 2
err := client.AGReadDB(address, start, size, buf)
var s7 gos7.Helper
var result uint16
s7.GetValueAt(buf, 0, &result)	 
  
```
Calling it from Go
----------
The module is `github.com/robinson/gos7` and supports Go 1.21 or newer. There are no
dependencies outside the standard library:
```
go get github.com/robinson/gos7
```
```go
import "github.com/robinson/gos7"

handler := gos7.NewTCPClientHandler("192.168.0.1", 0, 1) // address, rack, slot
handler.Timeout = 10 * time.Second
if err := handler.Connect(); err != nil { // TCP connect, COTP connect, S7 PDU negotiation
	// handle error
}
defer handler.Close()
client := gos7.NewClient(handler)
buf := make([]byte, 4)
if err := client.AGReadDB(1, 0, 4, buf); err != nil { // DB1, byte offset 0, 4 bytes
	// handle error
}
```
ISO-on-TCP fragmentation
----------
The TCP transporter speaks TPKT/COTP (RFC 1006 over ISO 8073 class 0) and fragments S7
telegrams, so they also pass gateways that forward smaller COTP TPDUs. The connection
confirm is parsed by its TPKT length and COTP length indicator: reordered and unknown
parameters are accepted, and its destination reference has to echo the source reference
of the connection request. Outgoing DT telegrams are split so that no fragment exceeds
the negotiated TPDU size and only the last fragment carries the EOT flag; incoming
fragments are read TPKT by TPKT and reassembled until the EOT flag, so the S7 layer
always sees one complete telegram. A connection that shows a protocol violation is
closed and has to be reconnected explicitly.

Sizes and units
----------
*   `Timeout`, `IdleTimeout`: `time.Duration` (nanoseconds). One `Timeout` covers a
    whole request/response exchange of the transporter, all fragments included.
*   COTP TPDU size: bytes on the wire, the 3 byte COTP DT header included, the 4 byte
    TPKT header not. Negotiated per connection from the TPDU-size parameter of the
    connection confirm (2^7 = 128 to 2^10 = 1024 bytes); 1024 bytes when the confirm
    names none. It is never carried over to another connection.
*   `PDULength`: bytes of S7 protocol data per PDU, negotiated with the CPU during
    connect (the client requests 480). It limits the reassembled S7 payload of a
    telegram, not single wire fragments.
*   `AGReadDB`/`AGWriteDB` `size` and `buffer`: bytes; `start` is a byte offset inside
    the area. The item length fields inside the telegrams count bits, as the S7
    protocol does.
*   `dbnumber`, `rack`, `slot`: plain numbers without a unit.

Request/response matching
----------
Every request carries a running PDU reference and the reply has to echo it. A reply with another
reference (for example a late answer after a timeout at a slow gateway) is discarded by the TCP
transporter, which keeps waiting for the matching reply until the timeout expires. Transporters other
than the TCP one get the error `CLI : PDU reference mismatch` from the packager. A read reply whose item
does not hold the requested number of bytes is rejected with `CLI : Invalid data size received` and
leaves the caller's buffer untouched.

References
----------
- libnodave http://libnodave.sourceforge.net/
- snap7 http://snap7.sourceforge.net/ 
- tarm serial library https://github.com/tarm/serial
- Simatic Open TCP/IP Communication via Industrial Ethernet from Siemens(doku)
- SIMATIC NET FDL-Programmierschnittstelle (doku)
- Elementary Data Types from Siemens (doku)

Simatic, Simatic S5, Simatic S7, S7-200, S7-300, S7-400, S7-1200, S7-1500 are registered Trademarks of Siemens

License
----------
https://opensource.org/licenses/BSD-3-Clause

Copyright (c) 2018, robinson
