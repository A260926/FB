/*
    libpe - the PE library

    Copyright (C) 2010 - 2017 libpe authors

    This file is part of libpe.

    libpe is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libpe is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with libpe.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <libpe/pe.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
bool pe_can_read(const pe_ctx_t *ctx, const void *ptr, size_t size) {
	const uintptr_t start = (uintptr_t)ptr;
	const uintptr_t end = start + size;
	return start >= (uintptr_t)ctx->map_addr && end <= (uintptr_t)ctx->map_end;
}
pe_err_e pe_parse(pe_ctx_t *ctx) {
	ctx->pe.dos_hdr = ctx->map_addr;
	if (ctx->pe.dos_hdr->e_magic == MAGIC_MZ) {
	    const uint32_t *signature_ptr = LIBPE_PTR_ADD(ctx->pe.dos_hdr,
	    	ctx->pe.dos_hdr->e_lfanew);
	    if (!pe_can_read(ctx, signature_ptr, LIBPE_SIZEOF_MEMBER(pe_file_t, signature)))
	    	return LIBPE_E_INVALID_LFANEW;

	    // NT signature (PE\0\0)
	    ctx->pe.signature = *signature_ptr;

	    switch (ctx->pe.signature) {
	    	default:
	    		//fprintf(stderr, "Invalid signature: %x\n", ctx->pe.signature);
	    		return LIBPE_E_INVALID_SIGNATURE;
	    	case SIGNATURE_PE:
	    	case SIGNATURE_PL:
	    	case SIGNATURE_PX:
	    		break;
	    }

	    ctx->pe.coff_hdr = LIBPE_PTR_ADD(signature_ptr,
	    	LIBPE_SIZEOF_MEMBER(pe_file_t, signature));

	} else if (pe_machine_type_name(ctx->pe.dos_hdr->e_magic) != NULL) {
		ctx->pe.coff_hdr = (void *)ctx->pe.dos_hdr;
		ctx->pe.dos_hdr = NULL;
	} else {
		return LIBPE_E_NOT_A_PE_FILE;
	}

	if (!pe_can_read(ctx, ctx->pe.coff_hdr, sizeof(IMAGE_COFF_HEADER)))
		return LIBPE_E_MISSING_COFF_HEADER;

	ctx->pe.num_sections = ctx->pe.coff_hdr->NumberOfSections;

	if (ctx->pe.coff_hdr->SizeOfOptionalHeader > 0) {

	// Optional header points right after the COFF header.
	ctx->pe.optional_hdr_ptr = LIBPE_PTR_ADD(ctx->pe.coff_hdr,
		sizeof(IMAGE_COFF_HEADER));

	// Figure out whether it's a PE32 or PE32+.
	uint16_t *opt_type_ptr = ctx->pe.optional_hdr_ptr;
	if (!pe_can_read(ctx, opt_type_ptr,
		LIBPE_SIZEOF_MEMBER(IMAGE_OPTIONAL_HEADER, type)))
		return LIBPE_E_MISSING_OPTIONAL_HEADER;

	ctx->pe.optional_hdr.type = *opt_type_ptr;

	switch (ctx->pe.optional_hdr.type) {
		default:
			return LIBPE_E_UNSUPPORTED_IMAGE;
		case MAGIC_ROM:
			if (ctx->pe.coff_hdr->SizeOfOptionalHeader !=
			    sizeof(IMAGE_ROM_OPTIONAL_HEADER))
				return LIBPE_E_UNSUPPORTED_IMAGE;
			if (!pe_can_read(ctx, ctx->pe.optional_hdr_ptr,
				sizeof(IMAGE_ROM_OPTIONAL_HEADER)))
				return LIBPE_E_MISSING_OPTIONAL_HEADER;
			ctx->pe.optional_hdr._rom = ctx->pe.optional_hdr_ptr;
			ctx->pe.optional_hdr.length = sizeof(IMAGE_ROM_OPTIONAL_HEADER);
			ctx->pe.entrypoint = ctx->pe.optional_hdr._rom->AddressOfEntryPoint;
			break;
		case MAGIC_PE32_0:
		case MAGIC_PE32:
			if (ctx->pe.coff_hdr->SizeOfOptionalHeader <
			    sizeof(IMAGE_OPTIONAL_HEADER_32))
				return LIBPE_E_UNSUPPORTED_IMAGE;
			if (!pe_can_read(ctx, ctx->pe.optional_hdr_ptr,
				sizeof(IMAGE_OPTIONAL_HEADER_32)))
				return LIBPE_E_MISSING_OPTIONAL_HEADER;
			ctx->pe.optional_hdr._32 = ctx->pe.optional_hdr_ptr;
			ctx->pe.optional_hdr.length = sizeof(IMAGE_OPTIONAL_HEADER_32);
			ctx->pe.num_directories =
				ctx->pe.optional_hdr._32->NumberOfRvaAndSizes;
			ctx->pe.entrypoint = ctx->pe.optional_hdr._32->AddressOfEntryPoint;
			ctx->pe.imagebase = ctx->pe.optional_hdr._32->ImageBase;
			break;
		case MAGIC_PE64:
			if (ctx->pe.coff_hdr->SizeOfOptionalHeader <
			    sizeof(IMAGE_OPTIONAL_HEADER_64))
				return LIBPE_E_UNSUPPORTED_IMAGE;
			if (!pe_can_read(ctx, ctx->pe.optional_hdr_ptr,
				sizeof(IMAGE_OPTIONAL_HEADER_64)))
				return LIBPE_E_MISSING_OPTIONAL_HEADER;
			ctx->pe.optional_hdr._64 = ctx->pe.optional_hdr_ptr;
			ctx->pe.optional_hdr.length = sizeof(IMAGE_OPTIONAL_HEADER_64);
			ctx->pe.num_directories =
				ctx->pe.optional_hdr._64->NumberOfRvaAndSizes;
			ctx->pe.entrypoint = ctx->pe.optional_hdr._64->AddressOfEntryPoint;
			ctx->pe.imagebase = ctx->pe.optional_hdr._64->ImageBase;
			break;
	}

	}

	if (ctx->pe.num_directories > MAX_DIRECTORIES) {
		//fprintf(stderr, "Too many directories (%u)\n", ctx->pe.num_directories);
		return LIBPE_E_TOO_MANY_DIRECTORIES;
	}

	if (ctx->pe.num_sections > MAX_SECTIONS) {
		//fprintf(stderr, "Too many sections (%u)\n", ctx->pe.num_sections);
		return LIBPE_E_TOO_MANY_SECTIONS;
	}

	if (ctx->pe.optional_hdr_ptr)
		ctx->pe.directories_ptr = LIBPE_PTR_ADD(ctx->pe.optional_hdr_ptr,
			ctx->pe.optional_hdr.length);

	uint32_t sections_offset = sizeof(IMAGE_FILE_HEADER)
		+ (uint32_t)ctx->pe.coff_hdr->SizeOfOptionalHeader;
	ctx->pe.sections_ptr = LIBPE_PTR_ADD(ctx->pe.coff_hdr, sections_offset);

	if (ctx->pe.num_directories > 0) {
		ctx->pe.directories = malloc(ctx->pe.num_directories
			* sizeof(IMAGE_DATA_DIRECTORY *));
		if (ctx->pe.directories == NULL)
			return LIBPE_E_ALLOCATION_FAILURE;
		for (uint32_t i = 0; i < ctx->pe.num_directories; i++) {
			ctx->pe.directories[i] = LIBPE_PTR_ADD(ctx->pe.directories_ptr,
				i * sizeof(IMAGE_DATA_DIRECTORY));
		}
	} else {
		ctx->pe.directories_ptr = NULL;
	}

	if (ctx->pe.num_sections > 0) {
		ctx->pe.sections = malloc(ctx->pe.num_sections
			* sizeof(IMAGE_SECTION_HEADER *));
		if (ctx->pe.sections == NULL)
			return LIBPE_E_ALLOCATION_FAILURE;
		for (uint32_t i = 0; i < ctx->pe.num_sections; i++) {
			ctx->pe.sections[i] = LIBPE_PTR_ADD(ctx->pe.sections_ptr,
				i * sizeof(IMAGE_SECTION_HEADER));
		}
	} else {
		ctx->pe.sections_ptr = NULL;
	}

	if (ctx->pe.coff_hdr->PointerToSymbolTable != 0) {
		uint32_t symbols_offset = ctx->pe.coff_hdr->PointerToSymbolTable;
		if (symbols_offset < ctx->map_size) {
			ctx->pe.symbols_ptr = LIBPE_PTR_ADD(ctx->map_addr, symbols_offset);
			ctx->pe.num_symbols = ctx->pe.coff_hdr->NumberOfSymbols;
			if (symbols_offset + ctx->pe.num_symbols * 18 < ctx->map_size) {
				ctx->pe.strings_ptr = LIBPE_PTR_ADD(ctx->pe.symbols_ptr, ctx->pe.num_symbols * 18);
				ctx->pe.strings_size = *(uint32_t *)ctx->pe.strings_ptr;
				if (ctx->pe.strings_size < 4 ||
				    symbols_offset + ctx->pe.num_symbols * 18 + ctx->pe.strings_size > ctx->map_size) {
					ctx->pe.strings_ptr = NULL;
					ctx->pe.strings_size = 0;
				}
			}
			if (ctx->pe.num_symbols == 0)
				ctx->pe.symbols_ptr = NULL;
		}
	}

	return LIBPE_E_OK;
}

uint64_t pe_rva2ofs(const pe_ctx_t *ctx, uint64_t rva) {
	if (rva == 0)
		return 0;

	if (ctx->pe.sections == NULL)
		return rva;

	// Find out which section the given RVA belongs
	for (uint32_t i=0; i < ctx->pe.num_sections; i++) {
		if (ctx->pe.sections[i] == NULL)
			return 0;

		// Use SizeOfRawData if VirtualSize == 0
		size_t section_size = ctx->pe.sections[i]->Misc.VirtualSize;
		if (section_size == 0)
			section_size = ctx->pe.sections[i]->SizeOfRawData;

		if (ctx->pe.sections[i]->VirtualAddress <= rva) {
			if ((ctx->pe.sections[i]->VirtualAddress + section_size) > rva) {
			 	rva -= ctx->pe.sections[i]->VirtualAddress;
				rva += ctx->pe.sections[i]->PointerToRawData;
				return rva;
			}
		}
	}

	// Handle PE with a single section
	if (ctx->pe.num_sections == 1) {
		rva -= ctx->pe.sections[0]->VirtualAddress;
		rva += ctx->pe.sections[0]->PointerToRawData;
		return rva;
	}

	return rva; // PE with no sections, return RVA
}

// Returns the RVA for a given offset
IMAGE_DATA_DIRECTORY *pe_directory_by_entry(pe_ctx_t *ctx, ImageDirectoryEntry entry) {
	if (ctx->pe.directories == NULL || entry > ctx->pe.num_directories - 1)
		return NULL;

	return ctx->pe.directories[entry];
}

#define LIBPE_ENTRY(v)	{ v, # v }

const char *pe_machine_type_name(MachineType type) {
	typedef struct {
		MachineType type;
		const char * const name;
	} MachineEntry;

	static const MachineEntry names[] = {
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_UNKNOWN),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_ALPHA_OLD),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_ALPHA),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_ALPHA64),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_AM33),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_AMD64),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_ARM),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_ARMV7),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_ARM64),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_ARM64EC),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_ARM64X),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_CEE),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_CEF),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_CHPE_X86),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_EBC),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_I386),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_I860),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_IA64),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_LOONGARCH32),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_LOONGARCH64),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_M32R),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_M68K),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_MIPS16),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_MIPSFPU),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_MIPSFPU16),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_MPPC_601),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_OMNI),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_PARISC),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_POWERPC),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_POWERPCFP),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_POWERPCBE),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_R3000),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_R3000_BE),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_R4000),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_R10000),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_RISCV32),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_RISCV64),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_RISCV128),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_SH3),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_SH3DSP),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_SH3E),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_SH4),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_SH5),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_TRICORE),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_TAHOE),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_THUMB),
		LIBPE_ENTRY(IMAGE_FILE_MACHINE_WCEMIPSV2)
	};

	for (unsigned int i=0; i < LIBPE_SIZEOF_ARRAY(names); i++) {
		if (type == names[i].type)
			return names[i].name;
	}
	return NULL;
}

