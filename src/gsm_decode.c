/*
 * Copyright 1992 by Jutta Degener and Carsten Bormann, Technische
 * Universitaet Berlin.  See the accompanying file "COPYRIGHT" for
 * details.  THERE IS ABSOLUTELY NO WARRANTY FOR THIS SOFTWARE.
 */

/* $Header: /tmp_amd/presto/export/kbs/jutta/src/gsm/RCS/gsm_decode.c,v 1.2 1996/07/02 09:59:05 jutta Exp $ */

#include "private.h"

#include "gsm.h"
#include "proto.h"

/*
 *  The parameter unpacking, the decode itself and the most-recent-good
 *  frame bookkeeping now live in plc_frame.c, shared with the block
 *  entry gsm_decode_plc().  This preserves the legacy semantics:
 *  a bad magic nibble still yields -1 before any state changes, and
 *  the WAV49 half-frame chaining (frame_index / frame_chain) behaves
 *  exactly as before.
 */
int gsm_decode P3((s, c, target), gsm s, gsm_byte * c, gsm_signal * target)
{
#ifdef WAV49
	if (s->wav_fmt)
		return gsm_plc_legacy_wav(s, c, target);
#endif
	return gsm_plc_legacy_normal(s, c, target);
}
