package parser

import (
	"errors"
	"fmt"
	"io"
)

type VHDXFile struct {
	// Reader to the underlying file
	reader  io.ReaderAt
	profile *VHDXProfile
	header  *FileType

	regions  []*RegionEntry
	Metadata *VHDXMetadata

	// Expose this reader to our callers: Reassemble the reader from
	// the BAT
	bat_reader *BatReader

	// The header copy selected as currently effective (valid CRC32C
	// and unique highest sequence number).
	currentHeader *Header

	// Parent disk, only set by NewVHDXFileWithParent.
	parent *VHDXFile
}

func (self *VHDXFile) ReadAt(buff []byte, off int64) (int, error) {
	if self.parent != nil {
		return readChainAt(self, buff, off)
	}
	return self.bat_reader.ReadAt(buff, off)
}

// NewVHDXFile opens a standalone VHDX image. Differencing disks
// (HasParent set) are rejected here: they must be opened together with
// an explicit parent through NewVHDXFileWithParent rather than being
// silently zero filled.
func NewVHDXFile(reader io.ReaderAt) (*VHDXFile, error) {
	self, err := newVHDXFile(reader)
	if err != nil {
		return nil, err
	}

	if self.Metadata.HasParent {
		return nil, errors.New("differencing VHDX requires an explicit parent: " +
			"open it with NewVHDXFileWithParent")
	}

	return self, nil
}

// NewVHDXFileWithParent opens a differencing VHDX image combined with
// its parent. The parent itself may be another differencing disk opened
// the same way. The parent is supplied explicitly: parent locator path
// strings are treated as metadata only and never cause a file to be
// opened.
func NewVHDXFileWithParent(reader io.ReaderAt, parent *VHDXFile) (*VHDXFile, error) {
	if parent == nil {
		return nil, errors.New("NewVHDXFileWithParent requires a non-nil parent")
	}

	self, err := newVHDXFile(reader)
	if err != nil {
		return nil, err
	}

	if !self.Metadata.HasParent {
		return nil, errors.New("NewVHDXFileWithParent requires a differencing disk: " +
			"metadata does not declare a parent")
	}

	err = validateChain(self, parent)
	if err != nil {
		return nil, err
	}

	self.parent = parent
	return self, nil
}

func newVHDXFile(reader io.ReaderAt) (*VHDXFile, error) {
	profile := NewVHDXProfile()
	self := &VHDXFile{
		reader:  reader,
		profile: profile,
		header:  profile.FileType(reader, 0),
		bat_reader: &BatReader{
			Reader: reader,
		},
	}

	err := self.Validate()
	if err != nil {
		return nil, err
	}

	region := self.header.Region1()
	self.regions = ParseArray_RegionEntry(profile, reader,
		region.Offset+int64(region.Size()), int(region.EntryCount()))

	var bat *Bat

	for _, r := range self.regions {
		switch r.GUID() {
		case BAT_GUID:
			bat = &Bat{
				StartOffset:          r.FileOffset(),
				EntrySize:            8,
				TotalNumberOfEntries: uint64(r.Length() / 8),
				profile:              profile,
				reader:               reader,
			}

		case Metadata_GUID:
			metadata := profile.Metadata(reader, int64(r.FileOffset()))
			if metadata.Signature() != MetadataSignature {
				continue
			}

			self.Metadata = metadata.ParseMetadata()
		}
	}

	if bat == nil {
		return nil, errors.New("No BAT found!")
	}

	if self.Metadata == nil {
		return nil, errors.New("Unable to parse file metadata")
	}

	if self.Metadata.BlockSize <= 0 {
		return nil, fmt.Errorf("BlockSize invalid: %v", self.Metadata.BlockSize)
	}

	if self.Metadata.VirtualDiskSize <= 0 {
		return nil, fmt.Errorf("VirtualDiskSize invalid: %v",
			self.Metadata.VirtualDiskSize)
	}

	if self.Metadata.LogicalSectorSize <= 0 {
		return nil, fmt.Errorf("LogicalSectorSize invalid: %v",
			self.Metadata.LogicalSectorSize)
	}

	self.bat_reader.bat = bat
	self.bat_reader.BlockSize = self.Metadata.BlockSize
	self.bat_reader.Size = self.Metadata.VirtualDiskSize

	bat.EntriesPerChunk = (1 << 23) * self.Metadata.LogicalSectorSize / self.Metadata.BlockSize
	if bat.EntriesPerChunk == 0 {
		return nil, fmt.Errorf("EntriesPerChunk invalid: %v", bat.EntriesPerChunk)
	}

	self.currentHeader, err = self.selectCurrentHeader()
	if err != nil {
		return nil, err
	}

	return self, nil
}

func (self *VHDXFile) DebugString() string {
	result := self.header.DebugString()
	for _, r := range self.regions {
		result += r.DebugString()
		result += fmt.Sprintf("GUID %02x\n", r.GUID())
	}

	result += self.bat_reader.bat.DebugString()

	if self.Metadata != nil {
		result += fmt.Sprintf("Metadata: %#v\n", self.Metadata)
	}

	return result
}

func (self *VHDXFile) Validate() error {
	if self.header.Signature() != VHDXFileSignature {
		return errors.New("File should have the vhdxfile signature")
	}

	if self.header.Header1().Signature() != HeaderSignature ||
		self.header.Header2().Signature() != HeaderSignature {
		return errors.New("File header should have the head signature")
	}

	if self.header.Region1().Signature() != RegionSignature ||
		self.header.Region1().Signature() != RegionSignature {
		return errors.New("File region should have the regi signature")
	}

	return nil
}
