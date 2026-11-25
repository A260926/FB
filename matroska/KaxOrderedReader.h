/****************************************************************************
** libmatroska : parse Matroska files, see http://www.matroska.org/
**
** Ordered-chapter reader: plays several Matroska sources as one ordered
** program without remuxing them.
**
** This file is part of libmatroska.
**
** This library is free software; you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public
** License as published by the Free Software Foundation; either
** version 2.1 of the License, or (at your option) any later version.
**
**********************************************************************/

#ifndef LIBMATROSKA_ORDERED_READER_H
#define LIBMATROSKA_ORDERED_READER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "matroska/KaxConfig.h"
#include "ebml/IOCallback.h"

namespace libmatroska {

class KaxOrderedReaderImpl;

/*!
  \brief Plays the ordered chapters of a Matroska root source, following
         ChapterSegmentUID references to other sources.

  The reader pre-scans the sources at Open() time and keeps only metadata,
  frame offsets and frame lengths.  Media payloads are re-read from the
  supplied IO callbacks for every Frame event.
*/
class MATROSKA_DLL_API KaxOrderedReader {
public:
  enum class Status {
    Ok,
    End,
    Error
  };

  enum class Kind {
    Config,
    Frame
  };

  struct Packet {
    Kind kind{Kind::Frame};
    size_t source_index{0};
    size_t chapter_index{0};
    uint64_t track_number{0};
    uint64_t source_ns{0};
    uint64_t program_ns{0};
    uint64_t duration_ns{0};
    uint64_t visible_begin_ns{0};
    uint64_t visible_duration_ns{0};
    bool keyframe{false};
    std::vector<uint8_t> data;
  };

  /*!
    \brief Open an ordered program.
    \param sources   Seekable IO callbacks, each positioned at the start of a
                     Matroska file.  The reader keeps shared ownership.
    \param rootSource Index of the source containing the Chapters element.
    \param editionUID EditionEntry UID to play (must have Ordered flag set).
    \param trackUID  TrackUID of the video track to play in every referenced
                     source.
    \param out       Receives the reader on success; reset on failure.
  */
  static bool Open(const std::vector<std::shared_ptr<libebml::IOCallback>>& sources,
                   size_t rootSource,
                   uint64_t editionUID,
                   uint64_t trackUID,
                   std::unique_ptr<KaxOrderedReader>& out);

  ~KaxOrderedReader();

  KaxOrderedReader(const KaxOrderedReader&) = delete;
  KaxOrderedReader& operator=(const KaxOrderedReader&) = delete;

  /*!
    \brief Move the logical play cursor to a program position.

    Positions are half-open: the chapter containing \p programNS is selected.
    Seek(DurationNS()) is valid and puts the reader in the End state.
    A position greater than DurationNS() fails and leaves the state untouched.
  */
  bool Seek(uint64_t programNS);

  /*!
    \brief Produce the next Config or Frame event.
    \note On Error the packet and the logical cursor are left untouched, so
          the same event can be retried after the IO problem is repaired.
  */
  Status ReadNext(Packet& out);

  /*!
    \brief Total program duration: the sum of all played chapter durations.
  */
  uint64_t DurationNS() const;

private:
  KaxOrderedReader();

  std::unique_ptr<KaxOrderedReaderImpl> impl_;
};

} // namespace libmatroska

#endif // LIBMATROSKA_ORDERED_READER_H
