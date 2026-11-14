/*
** FAAD2 - Freeware Advanced Audio (AAC) Decoder including SBR decoding
** Copyright (C) 2003-2005 M. Bakker, Nero AG, http://www.nero.com
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
**
** Any non-GPL usage of this software or parts of this software is strictly
** forbidden.
**
** The "appropriate copyright message" mentioned in section 2c of the GPLv2
** must read: "Code from FAAD2 is copyright (c) Nero AG, www.nero.com"
**
** Commercial non-GPL licensing of this software is possible.
** For more info contact Nero AG through Mpeg4AAClicense@nero.com.
*/

#ifndef __TNS_OBSERVE_H__
#define __TNS_OBSERVE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"

/*
 * Read-only observation point for the dedicated TNS regression build.
 *
 * The hook is compiled in only when FAAD_TNS_TEST_OBSERVE is defined.  It
 * receives the spectral coefficients of one channel exactly after the real
 * tns_decode_frame() call and before the filter bank runs.  The callback must
 * treat the array as read-only.  A normal build neither defines the macro nor
 * contains the symbol or the call.
 */
#ifdef FAAD_TNS_TEST_OBSERVE
typedef void (*faad_tns_observe_cb)(uint8_t channel, uint16_t frame_len,
                                    const real_t *spectrum);
extern faad_tns_observe_cb faad_tns_observe;
#endif

#ifdef __cplusplus
}
#endif
#endif
