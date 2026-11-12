package parser

import "fmt"

// RawBytes returns the on-disk 16 bytes of the GUID. Identity checks
// (such as parent linkage) compare these bytes directly rather than
// the pretty printed form.
func (self GUID) RawBytes() []byte {
	data := make([]byte, 0, 16)
	binary := ParseUint32(self.Reader, self.Profile.Off_GUID_Data1+self.Offset)
	data = append(data,
		byte(binary), byte(binary>>8), byte(binary>>16), byte(binary>>24))
	binary16 := ParseUint16(self.Reader, self.Profile.Off_GUID_Data2+self.Offset)
	data = append(data, byte(binary16), byte(binary16>>8))
	binary16 = ParseUint16(self.Reader, self.Profile.Off_GUID_Data3+self.Offset)
	data = append(data, byte(binary16), byte(binary16>>8))
	return append(data, self.Data4()...)
}

// IsZero reports whether all 16 bytes of the GUID are zero.
func (self GUID) IsZero() bool {
	for _, b := range self.RawBytes() {
		if b != 0 {
			return false
		}
	}
	return true
}

func (self GUID) AsString() string {
	data4 := self.Data4()
	return fmt.Sprintf(
		"{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", self.Data1(),
		self.Data2(), self.Data3(),
		data4[0], data4[1], data4[2], data4[3],
		data4[4], data4[5], data4[6], data4[7])
}
