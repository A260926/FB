/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DE265_CHECKPOINT_H
#define DE265_CHECKPOINT_H

/* Internal checkpoint implementation.
 *
 * The public entry points (de265_checkpoint_size/write/read) are declared in
 * de265.h. This header exposes the internal serializer so that it can be a
 * friend of decoder_context, de265_image and decoded_picture_buffer and read
 * their private state without widening the public API. The on-disk layout is
 * documented in doc/checkpoint-format.md.
 */

#include "libde265/de265.h"

#include <vector>

class decoder_context;
struct de265_image;
class slice_segment_header;
class byte_writer;
class byte_reader;

class checkpoint_state
{
 public:
  static constexpr uint16_t format_version = 1;

  /* True if 'ctx' is currently at a checkpointable state. When 'why' is
     non-null and the answer is false, a short reason string is stored there. */
  static bool is_available(const decoder_context* ctx, const char** why);

  /* Serialize the state into 'out'. Returns false if the state is not
     serializable. Does not modify 'ctx'. */
  static bool serialize(const decoder_context* ctx, std::vector<uint8_t>& out);

  /* Build a new, independent decoder from checkpoint bytes. Returns nullptr
     on failure and stores the error in '*err' when 'err' is non-null. */
  static decoder_context* deserialize(const void* buffer, size_t buffer_size,
                                      de265_error* err);

 private:
  static void write_parameter_sets(const decoder_context*, byte_writer&);
  static void write_picture(const decoder_context*, const de265_image*, uint32_t canonical_id, byte_writer&);
  static void write_slice_header(const slice_segment_header*, byte_writer&);

  static bool read_parameter_sets(decoder_context*, byte_reader&);
  static bool parse_parameter_set(decoder_context*, uint8_t type, uint8_t id,
                                  const std::vector<uint8_t>& payload);
  static bool read_dpb(decoder_context*, byte_reader&);
  static bool read_queues(decoder_context*, byte_reader&, uint32_t num_slots);
  static bool read_picture(decoder_context*, byte_reader&, uint32_t num_slots,
                           de265_image*& out);
  static bool read_slice_header(decoder_context*, byte_reader&, uint32_t num_slots,
                                de265_image*);
};

#endif
