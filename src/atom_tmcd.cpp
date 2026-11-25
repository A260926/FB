/*
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is MP4v2.
 */

#include "src/impl.h"

namespace mp4v2 { namespace impl {

///////////////////////////////////////////////////////////////////////////////

// QuickTime timecode sample description.
//
// Fixed layout (26 bytes after the 8-byte atom header), as written by
// QuickTime for a tmcd sample entry:
//
//   reserved[6]
//   uint16 dataReferenceIndex
//   uint32 flags             (timecode description flags, raw bytes)
//   uint32 reserved
//   uint32 timeScale
//   uint32 frameDuration
//   uint8  numFrames
//   uint8  reserved
//
// Any trailing 'name' source-name extension is left on disk untouched; it
// is not modelled as a property so the fixed fields stay indexable.

MP4TmcdAtom::MP4TmcdAtom(MP4File &file)
        : MP4Atom(file, "tmcd")
{
    AddReserved(*this, "reserved1", 6);                              /* 0 */

    AddProperty(
        new MP4Integer16Property(*this, "dataReferenceIndex"));      /* 1 */

    AddReserved(*this, "flags", 4);                                 /* 2 */

    AddReserved(*this, "reserved2", 4);                             /* 3 */

    AddProperty(
        new MP4Integer32Property(*this, "timeScale"));              /* 4 */

    AddProperty(
        new MP4Integer32Property(*this, "frameDuration"));          /* 5 */

    AddProperty(
        new MP4Integer8Property(*this, "numFrames"));               /* 6 */

    AddReserved(*this, "reserved3", 1);                             /* 7 */
}

void MP4TmcdAtom::Generate()
{
    MP4Atom::Generate();

    ((MP4Integer16Property*)m_pProperties[1])->SetValue(1);
}

void MP4TmcdAtom::Read()
{
    // The fixed layout occupies 26 bytes. A shorter (truncated) entry must
    // not fail the whole file open: read whatever fixed fields fit and let
    // the timecode query reject the incomplete description. Anything past
    // the fixed fields, typically the optional 'name' box, is skipped over
    // and its bytes remain exactly as stored.
    static const uint8_t fieldSize[8] = { 6, 2, 4, 4, 4, 4, 1, 1 };

    uint32_t offset = 0;
    for (uint32_t i = 0; i < m_pProperties.Size(); i++) {
        if (offset + fieldSize[i] > (uint32_t)m_size) {
            break;
        }
        m_pProperties[i]->Read(m_File);
        offset += fieldSize[i];
    }

    Skip(); // to end of atom
}

///////////////////////////////////////////////////////////////////////////////

}} // namespace mp4v2::impl
