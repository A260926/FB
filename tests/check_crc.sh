#!/usr/bin/env bash
#
# check_crc.sh - end-to-end verification of optional Layer III CRC frames.
#
# Everything is compiled into a throwaway directory; no files are installed
# and nothing is downloaded. The C test (tests/crc_frames.c) locates and
# parses frames itself and recomputes the checksum with its own bit-by-bit
# CRC. This driver additionally decodes the protected streams with the local
# FFmpeg using strict CRC checking and validates the decoded PCM.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root"

build=$(mktemp -d ./.crc-build.XXXXXX)
art="$build/artifacts"
mkdir -p "$art"
trap 'rm -rf "$build"' EXIT

srcs="src/lib/bitstream.c src/lib/huffman.c src/lib/l3bitstream.c \
src/lib/l3loop.c src/lib/l3mdct.c src/lib/l3subband.c src/lib/layer3.c \
src/lib/reservoir.c src/lib/tables.c"

# The existing, unprotected-path regression must keep passing unchanged.
bash devcheck/run.sh

echo "== compiling tests/crc_frames.c =="
cc -std=c99 -O2 -fwrapv -Wall -Isrc/lib \
  tests/crc_frames.c $srcs -lm -o "$build/crc_frames"

echo "== running frame/CRC scenarios =="
"$build/crc_frames" "$art" | tee "$build/crc_frames.log"

# The byte-for-byte and dual-entry comparisons must really have run.
grep -q 'BYTE_COMPARE .* identical OK' "$build/crc_frames.log"
grep -q 'DUAL_ENTRY .* planar=interleaved .* OK' "$build/crc_frames.log"
grep -q 'MUTATION side_info_bit_flip crc_detects=1 OK' "$build/crc_frames.log"
grep -q 'MUTATION main_data_bit_flip crc_unchanged=1 OK' "$build/crc_frames.log"
grep -q 'CRC_FRAMES_OK' "$build/crc_frames.log"

echo "== FFmpeg strict-CRC decode (local, offline) =="
command -v ffmpeg >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }

# FFCASE records: "<mp3 path> <encoded frames> <channels> <samples per frame>"
grep '^FFCASE ' "$build/crc_frames.log" | while read -r _tag mp3 frames channels spf; do
  test -s "$mp3"
  pcm="$build/$(basename "$mp3" .mp3).pcm"

  # explode: any CRC or framing mismatch makes FFmpeg fail this command.
  ffmpeg -hide_banner -loglevel error -err_detect crccheck+explode \
    -i "$mp3" -f s16le -y "$pcm"

  actual=$(wc -c < "$pcm" | tr -d ' ')
  expected=$((frames * spf * channels * 2))
  if [ "$actual" != "$expected" ]; then
    echo "PCM size mismatch for $mp3: $actual != $expected" >&2
    exit 1
  fi

  # Decoded audio must contain non-zero samples (not a silenced stream).
  nonzero=$(LC_ALL=C tr -d '\000' < "$pcm" | wc -c | tr -d ' ')
  if [ "$nonzero" -eq 0 ]; then
    echo "Decoded PCM for $mp3 is all zero" >&2
    exit 1
  fi

  echo "FFMPEG $(basename "$mp3") frames=$frames channels=$channels \
spf=$spf pcm_bytes=$actual nonzero_bytes=$nonzero OK"
done

echo "SHINE_CRC_OK"
