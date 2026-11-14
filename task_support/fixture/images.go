// Package fixture creates legal synthetic Mach-O bytes. It never executes them.
package fixture

import (
	"bytes"
	"encoding/binary"
)

var le = binary.LittleEndian

func put(b []byte, off int, values ...uint32) {
	for i, v := range values {
		le.PutUint32(b[off+4*i:], v)
	}
}
func put64(b []byte, off int, v uint64) { le.PutUint64(b[off:], v) }
func text(b []byte, off int, s string)  { copy(b[off:off+16], s) }
func header(b []byte, off int, typ, count, size uint32) {
	put(b, off, 0xfeedfacf, 0x01000007, 3, typ, count, size, 0, 0)
}
func entry(id string, vm, off uint64) []byte {
	size := (32 + len(id) + 1 + 7) &^ 7
	b := make([]byte, size)
	put(b, 0, 0x80000035, uint32(size))
	put64(b, 8, vm)
	put64(b, 16, off)
	put(b, 24, 32, 0)
	copy(b[32:], id)
	return b
}
func group(b []byte, off int, ids []string, offsets []uint64) {
	var commands []byte
	for i, id := range ids {
		commands = append(commands, entry(id, 0x100000000, offsets[i])...)
	}
	header(b, off, 0xc, uint32(len(ids)), uint32(len(commands)))
	copy(b[off+32:], commands)
}
func section(b []byte, off int, name string, addr, size uint64, data, rel, nrel, flags uint32) {
	text(b, off, name)
	text(b, off+16, "__TEXT")
	put64(b, off+32, addr)
	put64(b, off+40, size)
	put(b, off+48, data, 0, rel, nrel, flags, 0, 0, 0)
}
func leaf(b []byte, off int, label string) {
	const pc = uint64(0x100000400)
	data := off + 0x400
	// CU with DW_AT_name/string, DWARF4, 64-bit addresses.
	abbrev := []byte{1, 0x11, 0, 3, 8, 0, 0, 0}
	info := make([]byte, 12+len(label)+1)
	put(info, 0, uint32(len(info)-4))
	le.PutUint16(info[4:], 4)
	put(info, 6, 0)
	info[10] = 8
	info[11] = 1
	copy(info[12:], label)
	ncmd := uint32(3)
	cmdsz := uint32(72 + 3*80 + 72 + 80 + 24)
	header(b, off, 2, ncmd, cmdsz)
	c := off + 32
	put(b, c, 0x19, 72+3*80)
	text(b, c+8, "__TEXT")
	put64(b, c+24, 0x100000000)
	put64(b, c+32, 0x600)
	put64(b, c+40, uint64(off))
	put64(b, c+48, 0x600)
	put(b, c+56, 7, 5, 3, 0)
	section(b, c+72, "__text", pc, 16, uint32(data), uint32(off+0x480), 1, 0x80000400)
	section(b, c+152, "__debug_abbrev", pc+0x20, uint64(len(abbrev)), uint32(data+0x20), 0, 0, 0x2000000)
	section(b, c+232, "__debug_info", pc+0x40, uint64(len(info)), uint32(data+0x40), 0, 0, 0x2000000)
	c += 72 + 3*80
	put(b, c, 0x19, 72+80)
	text(b, c+8, "__DATA")
	put64(b, c+24, 0x100001000)
	put64(b, c+32, 16)
	put64(b, c+40, 0)
	put64(b, c+48, 0)
	put(b, c+56, 3, 3, 1, 0)
	section(b, c+72, "__bss", 0x100001000, 16, 0, 0, 0, 1)
	text(b, c+72+16, "__DATA")
	c += 72 + 80
	names := []string{label + "_a", label + "_alias", label + "_later", label + "_absolute"}
	str := []byte{0}
	for i, name := range names {
		s := off + 0x500 + i*16
		put(b, s, uint32(len(str)))
		b[s+4] = 0xf
		b[s+5] = 1
		value := pc + 2
		if i == 2 {
			value = pc + 6
		}
		if i == 3 {
			b[s+4] = 3
			b[s+5] = 0
			value = pc + 4
		}
		put64(b, s+8, value)
		str = append(str, []byte(name)...)
		str = append(str, 0)
	}
	put(b, c, 2, 24, uint32(off+0x500), 4, uint32(off+0x580), uint32(len(str)))
	copy(b[off+0x580:], str)
	copy(b[data:], []byte{0x90, 0x90, 0, 0, 0, 0, 0, 0, 0, 0, 0xc3, 0x90, 0x90, 0x90, 0x90, 0x90})
	copy(b[data+0x20:], abbrev)
	copy(b[data+0x40:], info)
	put(b, off+0x480, 2, 0x0e000000) // extern x86_64 UNSIGNED relocation, length=8, symbol 0.
}

// Standalone has ordinary zero-origin Mach-O offsets.
func Standalone() []byte { b := make([]byte, 0x600); leaf(b, 0, "solo"); return b }

// Fileset has two nested groups; each has a child named leaf, with shared PCs.
func Fileset() []byte {
	b := make([]byte, 0x3000)
	group(b, 0, []string{"left", "right"}, []uint64{0x400, 0x800})
	group(b, 0x400, []string{"leaf"}, []uint64{0x1000})
	group(b, 0x800, []string{"leaf"}, []uint64{0x2000})
	leaf(b, 0x1000, "left")
	leaf(b, 0x2000, "right")
	return b
}

// Unchanged compares the bytes after a read-only inspection.
func Unchanged(a, b []byte) bool { return bytes.Equal(a, b) }
