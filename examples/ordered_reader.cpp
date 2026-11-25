/****************************************************************************
** libmatroska : parse Matroska files, see http://www.matroska.org/
**
** Example: dump the ordered-chapter event list of a program.
**
** Usage:
**   ordered_reader <manifest-path> <rootSource> <editionUID> <trackUID>
**                  <input> [<input> ...]
**
** Each line is either:
**   C chapter source track source_ns program_ns size
**   F chapter source track source_ns program_ns duration_ns \
**     visible_begin_ns visible_duration_ns size
**
** This file is part of libmatroska.
**
**********************************************************************/

#include <matroska/KaxOrderedReader.h>
#include <ebml/StdIOCallback.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using libmatroska::KaxOrderedReader;

static bool ParseUnsigned(const char* text, uint64_t& value)
{
  if (text == nullptr || *text == '\0')
    return false;
  char* end = nullptr;
  errno = 0;
  unsigned long long parsed = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0')
    return false;
  value = static_cast<uint64_t>(parsed);
  return true;
}

int main(int argc, char** argv)
{
  if (argc < 6) {
    std::fprintf(stderr,
                 "usage: ordered_reader <manifest> <rootSource> <editionUID> "
                 "<trackUID> <input> [<input> ...]\n");
    return 2;
  }

  uint64_t root_source = 0;
  uint64_t edition_uid = 0;
  uint64_t track_uid = 0;
  if (!ParseUnsigned(argv[2], root_source) ||
      !ParseUnsigned(argv[3], edition_uid) ||
      !ParseUnsigned(argv[4], track_uid)) {
    std::fprintf(stderr, "invalid numeric argument\n");
    return 2;
  }

  std::vector<std::shared_ptr<libebml::IOCallback>> sources;
  for (int i = 5; i < argc; ++i) {
    try {
      sources.push_back(std::make_shared<libebml::StdIOCallback>(argv[i], MODE_READ));
    } catch (const std::exception& e) {
      std::fprintf(stderr, "cannot open %s: %s\n", argv[i], e.what());
      return 3;
    }
  }

  std::unique_ptr<KaxOrderedReader> reader;
  if (!KaxOrderedReader::Open(sources, static_cast<size_t>(root_source),
                              edition_uid, track_uid, reader)) {
    std::fprintf(stderr, "Open failed\n");
    return 4;
  }

  std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "cannot write %s\n", argv[1]);
    return 5;
  }

  KaxOrderedReader::Packet packet;
  for (;;) {
    const auto status = reader->ReadNext(packet);
    if (status == KaxOrderedReader::Status::End)
      break;
    if (status == KaxOrderedReader::Status::Error) {
      std::fprintf(stderr, "ReadNext failed\n");
      return 6;
    }

    if (packet.kind == KaxOrderedReader::Kind::Config) {
      out << "C " << packet.chapter_index << ' ' << packet.source_index << ' '
          << packet.track_number << ' ' << packet.source_ns << ' '
          << packet.program_ns << ' ' << packet.data.size() << '\n';
    } else {
      out << "F " << packet.chapter_index << ' ' << packet.source_index << ' '
          << packet.track_number << ' ' << packet.source_ns << ' '
          << packet.program_ns << ' ' << packet.duration_ns << ' '
          << packet.visible_begin_ns << ' ' << packet.visible_duration_ns << ' '
          << packet.data.size() << '\n';
    }
    if (!out) {
      std::fprintf(stderr, "write failed\n");
      return 7;
    }
  }

  out.close();
  if (!out) {
    std::fprintf(stderr, "close failed\n");
    return 7;
  }
  return 0;
}
