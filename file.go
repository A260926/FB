// Copyright 2018 Saferwall. All rights reserved.
// Use of this source code is governed by Apache v2 license
// license that can be found in the LICENSE file.

package pe

import (
	"errors"
	"os"

	"github.com/edsrzf/mmap-go"

	"github.com/saferwall/pe/log"
)

// A File represents an open PE file.
type File struct {
	DOSHeader    ImageDOSHeader              `json:"dos_header,omitempty"`
	RichHeader   RichHeader                  `json:"rich_header,omitempty"`
	NtHeader     ImageNtHeader               `json:"nt_header,omitempty"`
	COFF         COFF                        `json:"coff,omitempty"`
	Sections     []Section                   `json:"sections,omitempty"`
	Imports      []Import                    `json:"imports,omitempty"`
	Export       Export                      `json:"export,omitempty"`
	Debugs       []DebugEntry                `json:"debugs,omitempty"`
	Relocations  []Relocation                `json:"relocations,omitempty"`
	Resources    ResourceDirectory           `json:"resources,omitempty"`
	TLS          TLSDirectory                `json:"tls,omitempty"`
	LoadConfig   LoadConfig                  `json:"load_config,omitempty"`
	Exceptions   []Exception                 `json:"exceptions,omitempty"`
	Certificates CertificateSection          `json:"certificates,omitempty"`
	DelayImports []DelayImport               `json:"delay_imports,omitempty"`
	BoundImports []BoundImportDescriptorData `json:"bound_imports,omitempty"`
	GlobalPtr    uint32                      `json:"global_ptr,omitempty"`
	CLR          CLRData                     `json:"clr,omitempty"`
	IAT          []IATEntry                  `json:"iat,omitempty"`
	Anomalies    []string                    `json:"anomalies,omitempty"`
	Header       []byte
	data         mmap.MMap
	// rawData always references the unmodified input bytes. When the
	// ARM64XAlternate option is enabled, data is swapped to a private,
	// corrected copy while rawData keeps pointing at the original image.
	rawData      mmap.MMap
	arm64xActive bool
	FileInfo
	size          uint32
	OverlayOffset int64
	f             *os.File
	opts          *Options
	logger        *log.Helper
}

// Options that influence the PE parsing behaviour.
type Options struct {
	// Parse only the PE header and do not parse data directories, by default (false).
	Fast bool

	// Includes section entropy, by default (false).
	SectionEntropy bool

	// Maximum COFF symbols to parse, by default (MaxDefaultCOFFSymbolsCount).
	MaxCOFFSymbolsCount uint32

	// Maximum relocations to parse, by default (MaxDefaultRelocEntriesCount).
	MaxRelocEntriesCount uint32

	// Disable certificate validation, by default (false).
	DisableCertValidation bool

	// Disable signature validation, by default (false).
	DisableSignatureValidation bool

	// A custom logger.
	Logger log.Logger

	// OmitExportDirectory determines if export directory parsing is skipped, by default (false).
	OmitExportDirectory bool

	// OmitImportDirectory determines if import directory parsing is skipped, by default (false).
	OmitImportDirectory bool

	// OmitExceptionDirectory determines if exception directory parsing is skipped, by default (false).
	OmitExceptionDirectory bool

	// OmitResourceDirectory determines if resource directory parsing is skipped, by default (false).
	OmitResourceDirectory bool

	// OmitSecurityDirectory determines if security directory parsing is skipped, by default (false).
	OmitSecurityDirectory bool

	// OmitRelocDirectory determines if relocation directory parsing is skipped, by default (false).
	OmitRelocDirectory bool

	// OmitDebugDirectory determines if debug directory parsing is skipped, by default (false).
	OmitDebugDirectory bool

	// OmitArchitectureDirectory determines if architecture directory parsing is skipped, by default (false).
	OmitArchitectureDirectory bool

	// OmitGlobalPtrDirectory determines if global pointer directory parsing is skipped, by default (false).
	OmitGlobalPtrDirectory bool

	// OmitTLSDirectory determines if TLS directory parsing is skipped, by default (false).
	OmitTLSDirectory bool

	// OmitLoadConfigDirectory determines if load config directory parsing is skipped, by default (false).
	OmitLoadConfigDirectory bool

	// OmitBoundImportDirectory determines if bound import directory parsing is skipped, by default (false).
	OmitBoundImportDirectory bool

	// OmitIATDirectory determines if IAT directory parsing is skipped, by default (false).
	OmitIATDirectory bool

	// OmitDelayImportDirectory determines if delay import directory parsing is skipped, by default (false).
	OmitDelayImportDirectory bool

	// OmitCLRHeaderDirectory determines if CLR header directory parsing is skipped, by default (false).
	OmitCLRHeaderDirectory bool

	// OmitCLRMetadata determines if CLR metadata parsing is skipped, by default (false).
	OmitCLRMetadata bool

	// ARM64XAlternate selects the x64-compatible hybrid view of an ARM64X
	// image instead of its native ARM64 view. When enabled, the dynamic value
	// relocation table's symbol-6 fixups are applied to a private, in-memory
	// copy of the input before data directories are parsed, so NtHeader,
	// Export, GetData and the RVA helpers all reflect the alternate layout.
	// The original bytes are never modified and nothing is written to disk;
	// images without symbol-6 records simply keep their native view.
	ARM64XAlternate bool
}

// New instantiates a file instance with options given a file name.
func New(name string, opts *Options) (*File, error) {
	f, err := os.Open(name)
	if err != nil {
		return nil, err
	}

	return NewFile(f, opts)
}

// NewFile instantiates a file instance with options given a file handle.
func NewFile(f *os.File, opts *Options) (*File, error) {
	// Memory map the file instead of using read/write.
	data, err := mmap.Map(f, mmap.RDONLY, 0)
	if err != nil {
		f.Close()
		return nil, err
	}

	file := File{}
	if opts != nil {
		file.opts = opts
	} else {
		file.opts = &Options{}
	}

	if file.opts.MaxCOFFSymbolsCount == 0 {
		file.opts.MaxCOFFSymbolsCount = MaxDefaultCOFFSymbolsCount
	}
	if file.opts.MaxRelocEntriesCount == 0 {
		file.opts.MaxRelocEntriesCount = MaxDefaultRelocEntriesCount
	}

	var logger log.Logger
	if file.opts.Logger == nil {
		logger = log.NewStdLogger(os.Stdout)
		file.logger = log.NewHelper(log.NewFilter(logger,
			log.FilterLevel(log.LevelError)))
	} else {
		file.logger = log.NewHelper(file.opts.Logger)
	}

	file.data = data
	file.rawData = data
	file.size = uint32(len(file.data))
	file.f = f
	return &file, nil
}

// NewBytes instantiates a file instance with options given a memory buffer.
func NewBytes(data []byte, opts *Options) (*File, error) {

	file := File{}
	if opts != nil {
		file.opts = opts
	} else {
		file.opts = &Options{}
	}

	if file.opts.MaxCOFFSymbolsCount == 0 {
		file.opts.MaxCOFFSymbolsCount = MaxDefaultCOFFSymbolsCount
	}
	if file.opts.MaxRelocEntriesCount == 0 {
		file.opts.MaxRelocEntriesCount = MaxDefaultRelocEntriesCount
	}

	var logger log.Logger
	if file.opts.Logger == nil {
		logger = log.NewStdLogger(os.Stdout)
		file.logger = log.NewHelper(log.NewFilter(logger,
			log.FilterLevel(log.LevelError)))
	} else {
		file.logger = log.NewHelper(opts.Logger)
	}

	file.data = data
	file.rawData = data
	file.size = uint32(len(file.data))
	return &file, nil
}

func (pe *File) Close() error {
	_ = pe.Unmap()

	if pe.f != nil {
		return pe.f.Close()
	}
	return nil
}

// Close memory mapped file
func (pe *File) Unmap() error {
	// Only the original mapping can be unmapped. When the ARM64X alternate
	// view is active, data is a private byte slice rather than a mapping.
	if pe.arm64xActive {
		pe.data = nil
		if pe.rawData != nil {
			return pe.rawData.Unmap()
		}
		return nil
	}
	if pe.data != nil {
		return pe.data.Unmap()
	}

	return nil
}

// Parse performs the file parsing for a PE binary.
func (pe *File) Parse() error {

	// check for the smallest PE size.
	if len(pe.data) < TinyPESize {
		return ErrInvalidPESize
	}

	// Parse must be repeatable: drop every result of a previous run, including
	// any private ARM64X alternate-view buffer, before starting over. The
	// original input slice backing rawData is never touched.
	pe.resetForParse()

	// Parse the DOS header.
	err := pe.ParseDOSHeader()
	if err != nil {
		return err
	}

	// Parse the Rich header.
	err = pe.ParseRichHeader()
	if err != nil {
		pe.logger.Errorf("rich header parsing failed: %v", err)
	}

	// Parse the NT header.
	err = pe.ParseNTHeader()
	if err != nil {
		return err
	}

	// Parse COFF symbol table.
	err = pe.ParseCOFFSymbolTable()
	if err != nil {
		pe.logger.Debugf("coff symbols parsing failed: %v", err)
	}

	// Parse the Section Header.
	err = pe.ParseSectionHeader()
	if err != nil {
		return err
	}

	// Build the ARM64X compatible (hybrid) view before any data directory is
	// parsed so the corrected headers drive the rest of the normal flow.
	if pe.opts.ARM64XAlternate {
		if err := pe.applyARM64XAlternateView(); err != nil {
			return err
		}
	}

	// In fast mode, do not parse data directories.
	if pe.opts.Fast {
		return nil
	}

	// Parse the Data Directory entries.
	return pe.ParseDataDirectories()
}

// resetForParse returns the file to the state right after construction so
// Parse can be called repeatedly. In alternate-view mode the active data is a
// private copy that must be discarded in favor of the original raw input;
// replaying the symbol-6 DELTA fixups on an already-corrected buffer would
// otherwise accumulate them a second time.
func (pe *File) resetForParse() {
	if pe.arm64xActive {
		pe.data = pe.rawData
		pe.arm64xActive = false
	}

	pe.DOSHeader = ImageDOSHeader{}
	pe.RichHeader = RichHeader{}
	pe.NtHeader = ImageNtHeader{}
	pe.COFF = COFF{}
	pe.Sections = nil
	pe.Imports = nil
	pe.Export = Export{}
	pe.Debugs = nil
	pe.Relocations = nil
	pe.Resources = ResourceDirectory{}
	pe.TLS = TLSDirectory{}
	pe.LoadConfig = LoadConfig{}
	pe.Exceptions = nil
	pe.Certificates = CertificateSection{}
	pe.DelayImports = nil
	pe.BoundImports = nil
	pe.GlobalPtr = 0
	pe.CLR = CLRData{}
	pe.IAT = nil
	pe.Anomalies = nil
	pe.Header = nil
	pe.FileInfo = FileInfo{}
	pe.OverlayOffset = 0
	pe.size = uint32(len(pe.data))
}

// String stringify the data directory entry.
func (entry ImageDirectoryEntry) String() string {
	dataDirMap := map[ImageDirectoryEntry]string{
		ImageDirectoryEntryExport:       "Export",
		ImageDirectoryEntryImport:       "Import",
		ImageDirectoryEntryResource:     "Resource",
		ImageDirectoryEntryException:    "Exception",
		ImageDirectoryEntryCertificate:  "Security",
		ImageDirectoryEntryBaseReloc:    "Relocation",
		ImageDirectoryEntryDebug:        "Debug",
		ImageDirectoryEntryArchitecture: "Architecture",
		ImageDirectoryEntryGlobalPtr:    "GlobalPtr",
		ImageDirectoryEntryTLS:          "TLS",
		ImageDirectoryEntryLoadConfig:   "LoadConfig",
		ImageDirectoryEntryBoundImport:  "BoundImport",
		ImageDirectoryEntryIAT:          "IAT",
		ImageDirectoryEntryDelayImport:  "DelayImport",
		ImageDirectoryEntryCLR:          "CLR",
		ImageDirectoryEntryReserved:     "Reserved",
	}

	return dataDirMap[entry]
}

// ParseDataDirectories parses the data directories. The DataDirectory is an
// array of 16 structures. Each array entry has a predefined meaning for what
// it refers to.
func (pe *File) ParseDataDirectories() error {

	foundErr := false
	oh32 := ImageOptionalHeader32{}
	oh64 := ImageOptionalHeader64{}

	switch pe.Is64 {
	case true:
		oh64 = pe.NtHeader.OptionalHeader.(ImageOptionalHeader64)
	case false:
		oh32 = pe.NtHeader.OptionalHeader.(ImageOptionalHeader32)
	}

	// Maps data directory index to function which parses that directory.
	funcMaps := make(map[ImageDirectoryEntry]func(uint32, uint32) error)
	if !pe.opts.OmitExportDirectory {
		funcMaps[ImageDirectoryEntryExport] = pe.parseExportDirectory
	}
	if !pe.opts.OmitImportDirectory {
		funcMaps[ImageDirectoryEntryImport] = pe.parseImportDirectory
	}
	if !pe.opts.OmitExceptionDirectory {
		funcMaps[ImageDirectoryEntryException] = pe.parseExceptionDirectory
	}
	if !pe.opts.OmitResourceDirectory {
		funcMaps[ImageDirectoryEntryResource] = pe.parseResourceDirectory
	}
	if !pe.opts.OmitSecurityDirectory {
		funcMaps[ImageDirectoryEntryCertificate] = pe.parseSecurityDirectory
	}
	if !pe.opts.OmitRelocDirectory {
		funcMaps[ImageDirectoryEntryBaseReloc] = pe.parseRelocDirectory
	}
	if !pe.opts.OmitDebugDirectory {
		funcMaps[ImageDirectoryEntryDebug] = pe.parseDebugDirectory
	}
	if !pe.opts.OmitArchitectureDirectory {
		funcMaps[ImageDirectoryEntryArchitecture] = pe.parseArchitectureDirectory
	}
	if !pe.opts.OmitGlobalPtrDirectory {
		funcMaps[ImageDirectoryEntryGlobalPtr] = pe.parseGlobalPtrDirectory
	}
	if !pe.opts.OmitTLSDirectory {
		funcMaps[ImageDirectoryEntryTLS] = pe.parseTLSDirectory
	}
	if !pe.opts.OmitLoadConfigDirectory {
		funcMaps[ImageDirectoryEntryLoadConfig] = pe.parseLoadConfigDirectory
	}
	if !pe.opts.OmitBoundImportDirectory {
		funcMaps[ImageDirectoryEntryBoundImport] = pe.parseBoundImportDirectory
	}
	if !pe.opts.OmitIATDirectory {
		funcMaps[ImageDirectoryEntryIAT] = pe.parseIATDirectory
	}
	if !pe.opts.OmitDelayImportDirectory {
		funcMaps[ImageDirectoryEntryDelayImport] = pe.parseDelayImportDirectory
	}
	if !pe.opts.OmitCLRHeaderDirectory {
		funcMaps[ImageDirectoryEntryCLR] = pe.parseCLRHeaderDirectory
	}

	// Iterate over data directories and call the appropriate function.
	for entryIndex := ImageDirectoryEntry(0); entryIndex < ImageNumberOfDirectoryEntries; entryIndex++ {

		var va, size uint32
		switch pe.Is64 {
		case true:
			dirEntry := oh64.DataDirectory[entryIndex]
			va = dirEntry.VirtualAddress
			size = dirEntry.Size
		case false:
			dirEntry := oh32.DataDirectory[entryIndex]
			va = dirEntry.VirtualAddress
			size = dirEntry.Size
		}

		if va != 0 {
			func() {
				// keep parsing data directories even though some entries fails.
				defer func() {
					if e := recover(); e != nil {
						pe.logger.Errorf("unhandled exception when parsing data directory %s, reason: %v",
							entryIndex.String(), e)
						foundErr = true
					}
				}()

				// the last entry in the data directories is reserved and must be zero.
				if entryIndex == ImageDirectoryEntryReserved {
					pe.Anomalies = append(pe.Anomalies, AnoReservedDataDirectoryEntry)
					return
				}

				parseDirectory, ok := funcMaps[entryIndex]
				if !ok {
					return
				}
				err := parseDirectory(va, size)
				if err != nil {
					pe.logger.Warnf("failed to parse data directory %s, reason: %v",
						entryIndex.String(), err)
				}
			}()
		}
	}

	if foundErr {
		return errors.New("Data directory parsing failed")
	}
	return nil
}
