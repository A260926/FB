/*-
 * Copyright (c) 2026 The Elftoolchain Project.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>

#include <libelf.h>

#include "_libelf.h"

ELFTC_VCSID("$Id$");

/*@ELFTC-DOWNSTREAM-VCSID@*/

/*
 * Install (or clear, with a NULL callback) the resolver used to fetch
 * the external members of a GNU thin archive.  The API is read-only
 * and specific to thin archive parent descriptors, so the call is
 * rejected for NULL, non-archive and ordinary (non-thin) descriptors.
 * Replacing or clearing the resolver does not affect members that
 * have already been opened, since each of those owns a private copy
 * of its object bytes.
 */
int
elf_set_archive_resolver(Elf *elf, Elf_Archive_Resolver resolver,
    void *context)
{

	if (elf == NULL || elf->e_kind != ELF_K_AR ||
	    (elf->e_flags & LIBELF_F_AR_THIN) == 0 ||
	    elf->e_cmd != ELF_C_READ) {
		LIBELF_SET_ERROR(ARGUMENT, 0);
		return (-1);
	}

	elf->e_u.e_ar.e_resolver = resolver;
	elf->e_u.e_ar.e_resolver_ctx = context;

	return (0);
}
