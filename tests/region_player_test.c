/**
 * Copyright (c) 2013 Mozilla Foundation and Contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "task_support/check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webvtt/cue.h>
#include <webvtt/parser.h>
#include <webvtt/region.h>

/**
 * Tests for REGION definition parsing, cue binding, region lifetime and the
 * scroll-up region player.
 *
 * All expected row contents are spelled out literally here, rather than
 * being compared against a second run of the same implementation.
 */

#define ARRAY_LENGTH( a ) ( sizeof( a ) / sizeof( ( a )[ 0 ] ) )

typedef struct collect_t {
  webvtt_cue *cues[ 32 ];
  size_t count;
  webvtt_region *regions[ 8 ];
  size_t region_count;
} collect;

static void
collect_cue( void *userdata, webvtt_cue *cue )
{
  collect *c = ( collect * )userdata;
  if( c->count < ARRAY_LENGTH( c->cues ) ) {
    c->cues[ c->count++ ] = cue;
  } else {
    webvtt_release_cue( &cue );
  }
}

static void
collect_region( void *userdata, webvtt_region *region )
{
  collect *c = ( collect * )userdata;
  /* Borrowed reference: retain so we can inspect it later. */
  webvtt_ref_region( region );
  if( c->region_count < ARRAY_LENGTH( c->regions ) ) {
    c->regions[ c->region_count++ ] = region;
  } else {
    webvtt_release_region( &region );
  }
}

static int
collect_error( void *userdata, webvtt_uint line, webvtt_uint column,
               webvtt_error error )
{
  ( void )userdata;
  ( void )line;
  ( void )column;
  ( void )error;
  return 0;
}

static void
collect_free( collect *c )
{
  size_t i;
  for( i = 0; i < c->count; ++i ) {
    webvtt_release_cue( &c->cues[ i ] );
  }
  for( i = 0; i < c->region_count; ++i ) {
    webvtt_release_region( &c->regions[ i ] );
  }
  c->count = 0;
  c->region_count = 0;
}

/**
 * Parse 'text' as one or many chunks. When 'chunk' is 0 the whole input is
 * handed over in a single chunk; otherwise the input is split into chunks of
 * that many bytes. Always calls finish_parsing.
 */
static webvtt_status
parse_text( collect *c, const char *text, size_t chunk )
{
  webvtt_parser parser = 0;
  webvtt_status status;
  size_t length = strlen( text );
  size_t offset = 0;

  memset( c, 0, sizeof( *c ) );
  if( WEBVTT_FAILED( status = webvtt_create_parser( collect_cue, collect_error,
                                                   c, &parser ) ) ) {
    return status;
  }
  if( WEBVTT_FAILED( status = webvtt_parser_set_region_callback(
                         parser, collect_region ) ) ) {
    webvtt_delete_parser( parser );
    return status;
  }

  while( offset < length ) {
    size_t n = chunk ? chunk : length;
    if( n > length - offset ) {
      n = length - offset;
    }
    status = webvtt_parse_chunk( parser, text + offset, ( webvtt_uint )n );
    if( WEBVTT_FAILED( status ) ) {
      webvtt_delete_parser( parser );
      return status;
    }
    offset += n;
  }

  status = webvtt_finish_parsing( parser );
  webvtt_delete_parser( parser );
  return status;
}

/* ------------------------------------------------------------------ */
/* region_stream                                                       */
/* ------------------------------------------------------------------ */

static int
case_region_defaults_and_fields( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\n"
      "id:r1\n"
      "width:40%\n"
      "lines:4\n"
      "regionanchor:10%,90%\n"
      "viewportanchor:25%,75%\n"
      "scroll:up\n"
      "\n"
      "00:00:01.000 --> 00:00:03.000 region:r1\n"
      "Hello\n\n";
  collect c;
  webvtt_region *region;
  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, 0 ) ) );
  TASK_CHECK( c.region_count == 1 );
  region = c.regions[ 0 ];
  TASK_CHECK( webvtt_string_is_equal( &region->id, "r1", -1 ) );
  TASK_CHECK( region->width == 40.0 );
  TASK_CHECK( region->lines == 4 );
  TASK_CHECK( region->region_anchor_x == 10.0 );
  TASK_CHECK( region->region_anchor_y == 90.0 );
  TASK_CHECK( region->viewport_anchor_x == 25.0 );
  TASK_CHECK( region->viewport_anchor_y == 75.0 );
  TASK_CHECK( region->scroll_up );
  TASK_CHECK( c.count == 1 );
  TASK_CHECK( c.cues[ 0 ]->region == region );
  collect_free( &c );
  return 0;
}

static int
case_region_defaults( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\n"
      "id:plain\n"
      "\n"
      "00:00:01.000 --> 00:00:02.000 region:plain\n"
      "X\n\n";
  collect c;
  webvtt_region *region;
  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, 0 ) ) );
  TASK_CHECK( c.region_count == 1 );
  region = c.regions[ 0 ];
  TASK_CHECK( region->width == 100.0 );
  TASK_CHECK( region->lines == 3 );
  TASK_CHECK( region->region_anchor_x == 0.0 );
  TASK_CHECK( region->region_anchor_y == 100.0 );
  TASK_CHECK( region->viewport_anchor_x == 0.0 );
  TASK_CHECK( region->viewport_anchor_y == 100.0 );
  TASK_CHECK( !region->scroll_up );
  collect_free( &c );
  return 0;
}

static int
case_region_chunk_publication( void )
{
  /* A block must be published once its terminating blank line is consumed,
   * before finish_parsing and independently of how the input is chunked. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\n"
      "id:r\n"
      "scroll:up\n"
      "\n"
      "00:00:01.000 --> 00:00:02.000 region:r\n"
      "X\n\n";
  size_t chunks[] = { 1, 2, 3, 7, 13, 64 };
  size_t i;
  for( i = 0; i < ARRAY_LENGTH( chunks ); ++i ) {
    collect c;
    TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, chunks[ i ] ) ) );
    TASK_CHECK( c.region_count == 1 );
    TASK_CHECK( c.count == 1 );
    TASK_CHECK( webvtt_string_is_equal( &c.regions[ 0 ]->id, "r", -1 ) );
    TASK_CHECK( c.cues[ 0 ]->region == c.regions[ 0 ] );
    collect_free( &c );
  }
  return 0;
}

static int
case_region_publication_timing( void )
{
  /* The region callback must fire while the terminating blank line is being
   * consumed, i.e. before finish_parsing. */
  static const char header[] =
      "WEBVTT\n\n"
      "REGION\n"
      "id:r\n"
      "\n";
  collect c;
  webvtt_parser parser = 0;
  webvtt_status status;

  memset( &c, 0, sizeof( c ) );
  TASK_CHECK( !WEBVTT_FAILED( status = webvtt_create_parser(
                    collect_cue, collect_error, &c, &parser ) ) );
  TASK_CHECK( !WEBVTT_FAILED( status = webvtt_parser_set_region_callback(
                    parser, collect_region ) ) );
  TASK_CHECK( !WEBVTT_FAILED( status = webvtt_parse_chunk(
                    parser, header, ( webvtt_uint )strlen( header ) ) ) );
  /* No finish_parsing has run yet, but the block is complete and published. */
  TASK_CHECK( c.region_count == 1 );
  TASK_CHECK( webvtt_string_is_equal( &c.regions[ 0 ]->id, "r", -1 ) );
  TASK_CHECK( !WEBVTT_FAILED( status = webvtt_finish_parsing( parser ) ) );
  TASK_CHECK( c.region_count == 1 );
  collect_free( &c );
  webvtt_delete_parser( parser );
  return 0;
}

static int
case_region_eof_publication( void )
{
  /* EOF closes the last block and publishes it; a second finish is a no-op. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\n"
      "id:r\n"
      "width:30%\n"
      "lines:5\n"
      "scroll:up";
  collect c;
  webvtt_parser parser = 0;
  webvtt_status status;

  memset( &c, 0, sizeof( c ) );
  TASK_CHECK( !WEBVTT_FAILED( status = webvtt_create_parser(
                    collect_cue, collect_error, &c, &parser ) ) );
  TASK_CHECK( !WEBVTT_FAILED( status = webvtt_parser_set_region_callback(
                    parser, collect_region ) ) );
  TASK_CHECK( !WEBVTT_FAILED( status = webvtt_parse_chunk( parser, input,
                                                           ( webvtt_uint )strlen( input ) ) ) );
  TASK_CHECK( !WEBVTT_FAILED( status = webvtt_finish_parsing( parser ) ) );
  TASK_CHECK( c.region_count == 1 );
  TASK_CHECK( c.regions[ 0 ]->width == 30.0 );
  TASK_CHECK( c.regions[ 0 ]->lines == 5 );
  TASK_CHECK( !WEBVTT_FAILED( status = webvtt_finish_parsing( parser ) ) );
  TASK_CHECK( c.region_count == 1 ); /* not published twice */
  collect_free( &c );
  webvtt_delete_parser( parser );
  return 0;
}

static int
case_region_crlf_split( void )
{
  /* CR and LF may live in different chunks; the result must not change. */
  static const char input[] =
      "WEBVTT\r\n\r\n"
      "REGION\r\n"
      "id:r\r\n"
      "lines:4\r\n"
      "scroll:up\r\n"
      "\r\n"
      "00:00:01.000 --> 00:00:02.000 region:r\r\n"
      "X\r\n\r\n";
  collect c;
  size_t chunks[] = { 1, 2, 3, 5, 0 };
  size_t i;

  for( i = 0; i < ARRAY_LENGTH( chunks ); ++i ) {
    TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, chunks[ i ] ) ) );
    TASK_CHECK( c.region_count == 1 );
    TASK_CHECK( webvtt_string_is_equal( &c.regions[ 0 ]->id, "r", -1 ) );
    TASK_CHECK( c.regions[ 0 ]->lines == 4 );
    TASK_CHECK( c.count == 1 );
    TASK_CHECK( c.cues[ 0 ]->region == c.regions[ 0 ] );
    collect_free( &c );
  }
  return 0;
}

static int
case_region_variants( void )
{
  /* Block replacement keeps the old object intact; later definitions win. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\n"
      "id:r\n"
      "width:20%\n"
      "lines:2\n"
      "\n"
      "REGION\n"
      "id:r\n"
      "width:70%\n"
      "lines:5\n"
      "scroll:up\n"
      "\n"
      "00:00:01.000 --> 00:00:02.000 region:r\n"
      "X\n\n";
  collect c;
  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, 0 ) ) );
  TASK_CHECK( c.region_count == 2 );
  /* The first published definition is unchanged even after replacement. */
  TASK_CHECK( c.regions[ 0 ]->width == 20.0 );
  TASK_CHECK( c.regions[ 0 ]->lines == 2 );
  TASK_CHECK( c.regions[ 1 ]->width == 70.0 );
  TASK_CHECK( c.regions[ 1 ]->lines == 5 );
  TASK_CHECK( c.regions[ 1 ]->scroll_up );
  TASK_CHECK( c.count == 1 );
  TASK_CHECK( c.cues[ 0 ]->region == c.regions[ 1 ] );
  collect_free( &c );
  return 0;
}

static int
case_region_bad_field_recovery( void )
{
  /* A bad field keeps the previous value, reports once, and the following
   * block and cues still parse. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\n"
      "id:r\n"
      "width:oops\n"
      "lines:2\n"
      "\n"
      "REGION\n"
      "id:q\n"
      "width:50%\n"
      "\n"
      "00:00:01.000 --> 00:00:02.000 region:q\n"
      "X\n\n";
  collect c;
  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, 0 ) ) );
  TASK_CHECK( c.region_count == 2 );
  TASK_CHECK( c.regions[ 0 ]->width == 100.0 ); /* kept default */
  TASK_CHECK( c.regions[ 0 ]->lines == 2 );
  TASK_CHECK( c.regions[ 1 ]->width == 50.0 );
  TASK_CHECK( c.count == 1 );
  collect_free( &c );
  return 0;
}

static int
case_region_missing_id_dropped( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\n"
      "width:50%\n"
      "\n"
      "00:00:01.000 --> 00:00:02.000 region:r\n"
      "X\n\n";
  collect c;
  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, 0 ) ) );
  TASK_CHECK( c.region_count == 0 );
  TASK_CHECK( c.count == 1 );
  TASK_CHECK( c.cues[ 0 ]->region == 0 );
  collect_free( &c );
  return 0;
}

static int
case_region_after_cue_ignored( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "00:00:01.000 --> 00:00:02.000\n"
      "X\n\n"
      "REGION\n"
      "id:late\n"
      "scroll:up\n"
      "\n"
      "00:00:03.000 --> 00:00:04.000 region:late\n"
      "Y\n\n";
  collect c;
  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, 0 ) ) );
  TASK_CHECK( c.region_count == 0 );
  TASK_CHECK( c.count == 2 );
  TASK_CHECK( c.cues[ 0 ]->region == 0 );
  TASK_CHECK( c.cues[ 1 ]->region == 0 ); /* unknown id stays unbound */
  TASK_CHECK( webvtt_string_is_equal( &c.cues[ 0 ]->body, "X", -1 ) );
  TASK_CHECK( webvtt_string_is_equal( &c.cues[ 1 ]->body, "Y", -1 ) );
  collect_free( &c );
  return 0;
}

static int
case_region_word_in_body( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "00:00:01.000 --> 00:00:02.000\n"
      "REGION\n"
      "id:not-a-region\n"
      "\n";
  collect c;
  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, 0 ) ) );
  TASK_CHECK( c.region_count == 0 );
  TASK_CHECK( c.count == 1 );
  TASK_CHECK( webvtt_string_is_equal( &c.cues[ 0 ]->body,
                                      "REGION\nid:not-a-region", -1 ) );
  collect_free( &c );
  return 0;
}

/* ------------------------------------------------------------------ */
/* binding_lifetime                                                    */
/* ------------------------------------------------------------------ */

static int
case_binding_settings_order( void )
{
  /* region before/after line, and the compatibility check after all
   * settings have been processed. */
  static const char before[] =
      "WEBVTT\n\nREGION\nid:r\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:02.000 region:r line:10%\nX\n\n";
  static const char after[] =
      "WEBVTT\n\nREGION\nid:r\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:02.000 line:10% region:r\nX\n\n";
  static const char vertical[] =
      "WEBVTT\n\nREGION\nid:r\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:02.000 region:r vertical:rl\nX\n\n";
  static const char size[] =
      "WEBVTT\n\nREGION\nid:r\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:02.000 size:50% region:r\nX\n\n";
  static const char ok[] =
      "WEBVTT\n\nREGION\nid:r\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:02.000 align:start position:25% region:r\nX\n\n";
  collect c;

  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, before, 0 ) ) );
  TASK_CHECK( c.count == 1 && c.cues[ 0 ]->region == 0 );
  collect_free( &c );

  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, after, 0 ) ) );
  TASK_CHECK( c.count == 1 && c.cues[ 0 ]->region == 0 );
  collect_free( &c );

  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, vertical, 0 ) ) );
  TASK_CHECK( c.count == 1 && c.cues[ 0 ]->region == 0 );
  collect_free( &c );

  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, size, 0 ) ) );
  TASK_CHECK( c.count == 1 && c.cues[ 0 ]->region == 0 );
  collect_free( &c );

  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, ok, 0 ) ) );
  TASK_CHECK( c.count == 1 && c.cues[ 0 ]->region != 0 );
  TASK_CHECK( c.region_count == 1 );
  collect_free( &c );
  return 0;
}

static int
case_binding_last_region_setting_wins( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:a\n\n"
      "REGION\nid:b\n\n"
      "00:00:01.000 --> 00:00:02.000 region:a region:b\nX\n\n";
  collect c;
  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, 0 ) ) );
  TASK_CHECK( c.count == 1 );
  TASK_CHECK( c.cues[ 0 ]->region != 0 );
  TASK_CHECK( webvtt_string_is_equal( &c.cues[ 0 ]->region->id, "b", -1 ) );
  collect_free( &c );
  return 0;
}

static int
case_binding_unknown_id( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:a\n\n"
      "00:00:01.000 --> 00:00:02.000 region:missing\nX\n\n";
  collect c;
  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, 0 ) ) );
  TASK_CHECK( c.count == 1 );
  TASK_CHECK( c.cues[ 0 ]->region == 0 );
  TASK_CHECK( webvtt_string_is_equal( &c.cues[ 0 ]->body, "X", -1 ) );
  collect_free( &c );
  return 0;
}

static int
case_binding_survives_parser_delete( void )
{
  /* A cue held by the caller stays bound after the parser is deleted. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:02.000 region:r\nX\n\n";
  webvtt_parser parser = 0;
  webvtt_cue *cue = 0;
  webvtt_status status;
  size_t offset = 0;
  size_t length = strlen( input );
  collect sink;

  memset( &sink, 0, sizeof( sink ) );
  TASK_CHECK( !WEBVTT_FAILED( status = webvtt_create_parser(
                    collect_cue, collect_error, &sink, &parser ) ) );
  while( offset < length ) {
    if( WEBVTT_FAILED( status = webvtt_parse_chunk( parser, input + offset,
                                                    ( webvtt_uint )( length - offset ) ) ) ) {
      break;
    }
    offset = length;
  }
  TASK_CHECK( !WEBVTT_FAILED( status ) );
  TASK_CHECK( !WEBVTT_FAILED( status = webvtt_finish_parsing( parser ) ) );
  /* collect_cue wrote into the sink; it is the only cue. */
  TASK_CHECK( sink.count == 1 );
  cue = sink.cues[ 0 ];
  webvtt_delete_parser( parser );
  TASK_CHECK( cue != 0 );
  TASK_CHECK( cue->region != 0 );
  TASK_CHECK( webvtt_string_is_equal( &cue->region->id, "r", -1 ) );
  TASK_CHECK( cue->region->scroll_up );
  webvtt_release_cue( &cue );
  return 0;
}

static int
case_binding_region_outlives_table( void )
{
  /* The callback's borrowed reference can be retained; it stays valid after
   * the parser is deleted and after a same-id definition replaced it. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nwidth:10%\n\n"
      "REGION\nid:r\nwidth:80%\n\n"
      "00:00:01.000 --> 00:00:02.000 region:r\nX\n\n";
  collect c;
  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, 0 ) ) );
  TASK_CHECK( c.region_count == 2 );
  TASK_CHECK( c.regions[ 0 ]->width == 10.0 );
  TASK_CHECK( c.regions[ 1 ]->width == 80.0 );
  TASK_CHECK( c.count == 1 );
  /* Cue bound to the later version. */
  TASK_CHECK( c.cues[ 0 ]->region == c.regions[ 1 ] );
  collect_free( &c );
  return 0;
}

static int
case_binding_cue_release_order( void )
{
  /* Releasing the cue releases its region reference; the callback's retained
   * reference keeps the region alive. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:2\n\n"
      "00:00:01.000 --> 00:00:02.000 region:r\nX\n\n";
  collect c;
  webvtt_region *kept;
  TASK_CHECK( !WEBVTT_FAILED( parse_text( &c, input, 0 ) ) );
  TASK_CHECK( c.region_count == 1 && c.count == 1 );
  kept = c.regions[ 0 ];
  webvtt_release_cue( &c.cues[ 0 ] );
  c.count = 0;
  /* Still usable after the cue released its reference. */
  TASK_CHECK( kept->lines == 2 );
  TASK_CHECK( webvtt_string_is_equal( &kept->id, "r", -1 ) );
  collect_free( &c );
  return 0;
}

/* ------------------------------------------------------------------ */
/* rollup_history                                                      */
/* ------------------------------------------------------------------ */

typedef struct fixture_t {
  collect c;
  webvtt_region_player *player;
} fixture;

static int
fixture_load( fixture *f, const char *text, size_t chunk )
{
  webvtt_status status;
  memset( f, 0, sizeof( *f ) );
  status = parse_text( &f->c, text, chunk );
  if( WEBVTT_FAILED( status ) ) {
    return 0;
  }
  status = webvtt_region_player_create( f->c.cues, f->c.count, &f->player );
  return !WEBVTT_FAILED( status );
}

static void
fixture_free( fixture *f )
{
  if( f->player ) {
    webvtt_region_player_delete( f->player );
    f->player = 0;
  }
  collect_free( &f->c );
}

static size_t
row_count( fixture *f )
{
  size_t n = 0;
  webvtt_region_player_rows( f->player, &n );
  return n;
}

static const char *
row_text( fixture *f, size_t index )
{
  size_t n = 0;
  const webvtt_region_row *rows = webvtt_region_player_rows( f->player, &n );
  if( index >= n || !rows ) {
    return "";
  }
  return rows[ index ].text;
}

static size_t
row_cue( fixture *f, size_t index )
{
  size_t n = 0;
  const webvtt_region_row *rows = webvtt_region_player_rows( f->player, &n );
  return index < n ? rows[ index ].cue_index : ( size_t )-1;
}

static webvtt_uint
row_line( fixture *f, size_t index )
{
  size_t n = 0;
  const webvtt_region_row *rows = webvtt_region_player_rows( f->player, &n );
  return index < n ? rows[ index ].line_index : 0xFFFFFFFFu;
}

static int
case_rollup_over_lines( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:2\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:05.000 region:r\nA1\nA2\nA3\n\n";
  fixture f;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 1000 ) ) );
  TASK_CHECK( row_count( &f ) == 2 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "A2" ) );
  TASK_CHECK( !strcmp( row_text( &f, 1 ), "A3" ) );
  TASK_CHECK( row_cue( &f, 0 ) == 0 && row_cue( &f, 1 ) == 0 );
  TASK_CHECK( row_line( &f, 0 ) == 1 && row_line( &f, 1 ) == 2 );
  fixture_free( &f );
  return 0;
}

static int
case_rollup_evicted_rows_stay_gone( void )
{
  /* A long cue fills the region; a short cue then pushes out its oldest row.
   * When the short cue expires, the pushed-out row must NOT come back: the
   * correct result is [A3], not the [A2, A3] a "filter active cues and take
   * the last N lines" implementation would produce. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:2\nscroll:up\n\n"
      "00:00:00.000 --> 00:00:10.000 region:r\nA1\nA2\nA3\n\n"
      "00:00:01.000 --> 00:00:02.000 region:r\nS\n\n";
  fixture f;
  TASK_CHECK( fixture_load( &f, input, 0 ) );

  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 1000 ) ) );
  TASK_CHECK( row_count( &f ) == 2 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "A3" ) );
  TASK_CHECK( !strcmp( row_text( &f, 1 ), "S" ) );
  TASK_CHECK( row_cue( &f, 0 ) == 0 && row_cue( &f, 1 ) == 1 );
  TASK_CHECK( row_line( &f, 0 ) == 2 && row_line( &f, 1 ) == 0 );

  /* The short cue expires; A2 stays evicted for the rest of this history. */
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 3000 ) ) );
  TASK_CHECK( row_count( &f ) == 1 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "A3" ) );
  TASK_CHECK( row_cue( &f, 0 ) == 0 );
  TASK_CHECK( row_line( &f, 0 ) == 2 );
  fixture_free( &f );
  return 0;
}

static int
case_rollup_jump_leaves_eviction_trace( void )
{
  /* A single advance that jumps from before every cue to a time after the
   * short cue ended must still process that cue's arrival and eviction. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:2\nscroll:up\n\n"
      "00:00:00.000 --> 00:00:10.000 region:r\nA1\nA2\nA3\n\n"
      "00:00:01.000 --> 00:00:02.000 region:r\nS\n\n";
  fixture f;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 3000 ) ) );
  TASK_CHECK( row_count( &f ) == 1 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "A3" ) );
  fixture_free( &f );
  return 0;
}

static int
case_rollup_partial_cue( void )
{
  /* Overflow may leave only the tail of a single multi-line cue visible. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:1\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:05.000 region:r\nA1\nA2\nA3\n\n";
  fixture f;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 1000 ) ) );
  TASK_CHECK( row_count( &f ) == 1 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "A3" ) );
  TASK_CHECK( row_cue( &f, 0 ) == 0 );
  TASK_CHECK( row_line( &f, 0 ) == 2 );
  fixture_free( &f );
  return 0;
}

static int
case_rollup_jump_processes_arrivals( void )
{
  /* One advance over several cues; every arrival and expiry in between
   * must have been processed. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:3\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:02.000 region:r\nS1\n\n"
      "00:00:02.000 --> 00:00:08.000 region:r\nM1\nM2\n\n"
      "00:00:05.000 --> 00:00:10.000 region:r\nT1\n\n";
  fixture f;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 5000 ) ) );
  TASK_CHECK( row_count( &f ) == 3 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "M1" ) );
  TASK_CHECK( !strcmp( row_text( &f, 1 ), "M2" ) );
  TASK_CHECK( !strcmp( row_text( &f, 2 ), "T1" ) );
  fixture_free( &f );
  return 0;
}

static int
case_rollup_same_time_order( void )
{
  /* At equal start times, expiry is handled before the new arrivals, and
   * arrivals are added in input order. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:3\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:02.000 region:r\nOld1\nOld2\n\n"
      "00:00:02.000 --> 00:00:05.000 region:r\nNew1\nNew2\n\n";
  fixture f;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 2000 ) ) );
  TASK_CHECK( row_count( &f ) == 2 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "New1" ) );
  TASK_CHECK( !strcmp( row_text( &f, 1 ), "New2" ) );
  TASK_CHECK( row_cue( &f, 0 ) == 1 && row_cue( &f, 1 ) == 1 );
  fixture_free( &f );
  return 0;
}

static int
case_rollup_region_order( void )
{
  /* Region groups are ordered by first appearance in the input cue array. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:late\nscroll:up\n\n"
      "REGION\nid:early\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:05.000 region:early\nE\n\n"
      "00:00:01.000 --> 00:00:05.000 region:late\nL\n\n";
  fixture f;
  size_t n = 0;
  const webvtt_region_row *rows;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 1000 ) ) );
  rows = webvtt_region_player_rows( f.player, &n );
  TASK_CHECK( n == 2 );
  TASK_CHECK( rows != 0 );
  TASK_CHECK( webvtt_string_is_equal( &rows[ 0 ].region->id, "early", -1 ) );
  TASK_CHECK( !strcmp( rows[ 0 ].text, "E" ) );
  TASK_CHECK( webvtt_string_is_equal( &rows[ 1 ].region->id, "late", -1 ) );
  TASK_CHECK( !strcmp( rows[ 1 ].text, "L" ) );
  fixture_free( &f );
  return 0;
}

/* ------------------------------------------------------------------ */
/* seek_replay                                                         */
/* ------------------------------------------------------------------ */

static int
case_seek_replays_ended_arrivals( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:3\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:02.000 region:r\nS1\nS2\n\n"
      "00:00:04.000 --> 00:00:08.000 region:r\nL1\nL2\n\n";
  fixture f;
  size_t n = 0;
  const webvtt_region_row *rows;

  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 6000 ) ) );
  TASK_CHECK( row_count( &f ) == 2 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "L1" ) );
  TASK_CHECK( !strcmp( row_text( &f, 1 ), "L2" ) );

  /* Seek back to 1500: the short cue's arrival is replayed. */
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_seek( f.player, 1500 ) ) );
  rows = webvtt_region_player_rows( f.player, &n );
  TASK_CHECK( n == 2 );
  TASK_CHECK( !strcmp( rows[ 0 ].text, "S1" ) );
  TASK_CHECK( !strcmp( rows[ 1 ].text, "S2" ) );

  /* Seek forward past the short cue's expiry, into the long cue. */
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_seek( f.player, 5000 ) ) );
  TASK_CHECK( row_count( &f ) == 2 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "L1" ) );
  TASK_CHECK( !strcmp( row_text( &f, 1 ), "L2" ) );
  fixture_free( &f );
  return 0;
}

static int
case_seek_history_matches_advance( void )
{
  /* Seeking to a time from scratch matches advancing there incrementally. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:2\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:02.000 region:r\nS\n\n"
      "00:00:03.000 --> 00:00:09.000 region:r\nL1\nL2\nL3\n\n";
  fixture f;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 6000 ) ) );
  TASK_CHECK( row_count( &f ) == 2 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "L2" ) );
  TASK_CHECK( !strcmp( row_text( &f, 1 ), "L3" ) );

  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_seek( f.player, 6000 ) ) );
  TASK_CHECK( row_count( &f ) == 2 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "L2" ) );
  TASK_CHECK( !strcmp( row_text( &f, 1 ), "L3" ) );
  TASK_CHECK( row_line( &f, 0 ) == 1 && row_line( &f, 1 ) == 2 );
  fixture_free( &f );
  return 0;
}

static int
case_seek_rejects_backwards_advance( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:2\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:05.000 region:r\nA\n\n";
  fixture f;
  webvtt_status status;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 3000 ) ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 3000 ) ) );
  TASK_CHECK( row_count( &f ) == 1 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "A" ) );
  status = webvtt_region_player_advance( f.player, 1000 );
  TASK_CHECK( WEBVTT_FAILED( status ) );
  /* The failed call did not change the frame. */
  TASK_CHECK( row_count( &f ) == 1 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "A" ) );
  fixture_free( &f );
  return 0;
}

static int
case_seek_players_independent( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:2\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:05.000 region:r\nA1\nA2\nA3\n\n";
  fixture a, b;
  TASK_CHECK( fixture_load( &a, input, 0 ) );
  TASK_CHECK( fixture_load( &b, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( a.player, 1000 ) ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_seek( b.player, 0 ) ) );
  TASK_CHECK( row_count( &a ) == 2 );
  TASK_CHECK( row_count( &b ) == 0 );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( b.player, 1000 ) ) );
  TASK_CHECK( !strcmp( row_text( &a, 0 ), "A2" ) );
  TASK_CHECK( !strcmp( row_text( &b, 0 ), "A2" ) );
  fixture_free( &a );
  fixture_free( &b );
  return 0;
}

static int
case_seek_creation_lifetime( void )
{
  /* The player keeps its own cue references and does not need the parser
   * afterwards; releasing the caller's cues leaves playback intact. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nlines:2\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:05.000 region:r\nA1\nA2\nA3\n\n";
  fixture f;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  /* Release everything the collector owns. */
  collect_free( &f.c );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 2000 ) ) );
  TASK_CHECK( row_count( &f ) == 2 );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "A2" ) );
  TASK_CHECK( !strcmp( row_text( &f, 1 ), "A3" ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_seek( f.player, 1000 ) ) );
  TASK_CHECK( !strcmp( row_text( &f, 0 ), "A2" ) );
  fixture_free( &f );
  return 0;
}

static int
case_seek_empty_inputs( void )
{
  webvtt_region_player *player = 0;
  size_t n = 99;
  const webvtt_region_row *rows;
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_create( 0, 0, &player ) ) );
  TASK_CHECK( player != 0 );
  rows = webvtt_region_player_rows( player, &n );
  TASK_CHECK( rows == 0 && n == 0 );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( player, 1000 ) ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_seek( player, 0 ) ) );
  TASK_CHECK( webvtt_region_player_rows( player, &n ) == 0 && n == 0 );
  webvtt_region_player_delete( player );
  return 0;
}

/* ------------------------------------------------------------------ */
/* svg_output                                                          */
/* ------------------------------------------------------------------ */

static int
draw_svg( webvtt_region_player *player, double width, double height,
          double step, const char *path )
{
  FILE *out = fopen( path, "wb" );
  webvtt_status status;
  if( !out ) {
    return 0;
  }
  status = webvtt_region_player_write_svg( player, width, height, step, out );
  fclose( out );
  return !WEBVTT_FAILED( status );
}

static char *
read_file( const char *path )
{
  FILE *in = fopen( path, "rb" );
  char *data;
  long size;
  if( !in ) {
    return 0;
  }
  fseek( in, 0, SEEK_END );
  size = ftell( in );
  fseek( in, 0, SEEK_SET );
  if( size < 0 ) {
    fclose( in );
    return 0;
  }
  data = ( char * )malloc( ( size_t )size + 1 );
  if( !data ) {
    fclose( in );
    return 0;
  }
  if( fread( data, 1, ( size_t )size, in ) != ( size_t )size ) {
    free( data );
    fclose( in );
    return 0;
  }
  data[ size ] = 0;
  fclose( in );
  return data;
}

static int
case_svg_coordinates_and_escaping( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:r&<\nwidth:50%\nlines:3\n"
      "regionanchor:0%,100%\nviewportanchor:25%,75%\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:05.000 region:r&<\n"
      "A &amp; <b>B</b>\nC\n\n";
  fixture f;
  char *svg;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 1000 ) ) );
  TASK_CHECK( draw_svg( f.player, 640.0, 360.0, 20.0,
                        ".task-build/check.svg" ) );
  svg = read_file( ".task-build/check.svg" );
  TASK_CHECK( svg != 0 );
  /* x = 640*25/100 - 320*0/100 = 160; W = 320; y = 360*75/100 - 60*100/100 = 210 */
  TASK_CHECK( strstr( svg, "width=\"640\" height=\"360\"" ) != 0 );
  TASK_CHECK( strstr( svg, "<rect x=\"160\" y=\"210\" width=\"320\" height=\"60\"" ) != 0 );
  TASK_CHECK( strstr( svg, "data-region=\"r&amp;&lt;\"" ) != 0 );
  TASK_CHECK( strstr( svg, ">A &amp; B</text>" ) != 0 );
  TASK_CHECK( strstr( svg, "data-cue-index=\"0\" data-line-index=\"0\"" ) != 0 );
  TASK_CHECK( strstr( svg, "data-cue-index=\"0\" data-line-index=\"1\"" ) != 0 );
  /* Two rows: n=2, lines=3 -> baselines 210 + (3-2+0+1)*20 = 250 and 270. */
  TASK_CHECK( strstr( svg, "<text x=\"160\" y=\"250\"" ) != 0 );
  TASK_CHECK( strstr( svg, "<text x=\"160\" y=\"270\"" ) != 0 );
  free( svg );
  fixture_free( &f );
  return 0;
}

static int
case_svg_empty_and_groups( void )
{
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:empty\nlines:2\nscroll:up\n\n"
      "REGION\nid:full\nlines:2\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:05.000 region:full\nX\n\n";
  fixture f;
  char *svg;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 0 ) ) );
  TASK_CHECK( draw_svg( f.player, 320.0, 180.0, 10.0,
                        ".task-build/empty.svg" ) );
  svg = read_file( ".task-build/empty.svg" );
  TASK_CHECK( svg != 0 );
  TASK_CHECK( strstr( svg, "<svg " ) != 0 );
  TASK_CHECK( strstr( svg, "</svg>" ) != 0 );
  TASK_CHECK( strstr( svg, "data-region" ) == 0 ); /* empty frame: no group */
  free( svg );

  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 1000 ) ) );
  TASK_CHECK( draw_svg( f.player, 320.0, 180.0, 10.0,
                        ".task-build/full.svg" ) );
  svg = read_file( ".task-build/full.svg" );
  TASK_CHECK( svg != 0 );
  TASK_CHECK( strstr( svg, "<g data-region=\"full\"" ) != 0 );
  TASK_CHECK( strstr( svg, "<g data-region=\"empty\"" ) == 0 );
  free( svg );
  fixture_free( &f );
  return 0;
}

static int
case_svg_validation( void )
{
  /* Non-positive or non-finite geometry is rejected. */
  webvtt_region_player *player = 0;
  FILE *out = fopen( ".task-build/geometry.svg", "wb" );
  TASK_CHECK( out != 0 );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_create( 0, 0, &player ) ) );
  TASK_CHECK( WEBVTT_FAILED( webvtt_region_player_write_svg( player, 0.0,
                                                             100.0, 10.0, out ) ) );
  TASK_CHECK( WEBVTT_FAILED( webvtt_region_player_write_svg( player, 100.0,
                                                             0.0, 10.0, out ) ) );
  TASK_CHECK( WEBVTT_FAILED( webvtt_region_player_write_svg( player, 100.0,
                                                             100.0, -1.0, out ) ) );
  TASK_CHECK( webvtt_region_player_write_svg( player, 100.0, 100.0, 10.0, 0 )
              == WEBVTT_INVALID_PARAM );
  webvtt_region_player_delete( player );
  fclose( out );
  return 0;
}

static int
case_svg_utf8_and_special_text( void )
{
  /* Text and attributes are escaped independently; UTF-8 bytes pass through. */
  static const char input[] =
      "WEBVTT\n\n"
      "REGION\nid:id\"q\nlines:1\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:05.000 region:id\"q\n"
      "\xE4\xB8\xAD &lt;tag&gt; \"quote\"\n\n";
  fixture f;
  char *svg;
  TASK_CHECK( fixture_load( &f, input, 0 ) );
  TASK_CHECK( !WEBVTT_FAILED( webvtt_region_player_advance( f.player, 1000 ) ) );
  TASK_CHECK( draw_svg( f.player, 100.0, 50.0, 10.0,
                        ".task-build/utf8.svg" ) );
  svg = read_file( ".task-build/utf8.svg" );
  TASK_CHECK( svg != 0 );
  TASK_CHECK( strstr( svg, "\xE4\xB8\xAD" ) != 0 );
  TASK_CHECK( strstr( svg, "data-region=\"id&quot;q\"" ) != 0 );
  TASK_CHECK( strstr( svg, "&lt;tag&gt;" ) != 0 );
  TASK_CHECK( strstr( svg, "&quot;quote&quot;" ) != 0 );
  free( svg );
  fixture_free( &f );
  return 0;
}

static int
case_svg_example_binary( void )
{
  /* The example program must exist and produce parseable frames with the
   * expected coordinates for a known input. This exercises the real build. */
  static const char vtt[] =
      "WEBVTT\n\n"
      "REGION\nid:r\nwidth:50%\nlines:2\n"
      "regionanchor:0%,100%\nviewportanchor:0%,100%\nscroll:up\n\n"
      "00:00:01.000 --> 00:00:05.000 region:r\nHello\nWorld\n\n";
  FILE *file = fopen( ".task-build/example_check.vtt", "wb" );
  int rc;
  TASK_CHECK( file != 0 );
  TASK_CHECK( fwrite( vtt, 1, sizeof( vtt ) - 1, file ) == sizeof( vtt ) - 1 );
  fclose( file );
  rc = system( ".task-build/vttregions .task-build/example_check.vtt "
               "320 200 15 1000 > .task-build/example_check.out 2>/dev/null" );
  TASK_CHECK( rc == 0 );
  {
    char *svg = read_file( ".task-build/example_check.out" );
    TASK_CHECK( svg != 0 );
    TASK_CHECK( strstr( svg, "<!-- frame 1000 -->" ) != 0 );
    /* W=160,H=30,x=0,y=200-30=170; n=2 -> baselines 170+15 and 170+30. */
    TASK_CHECK( strstr( svg, "<rect x=\"0\" y=\"170\" width=\"160\" height=\"30\"" ) != 0 );
    TASK_CHECK( strstr( svg, "<text x=\"0\" y=\"185\"" ) != 0 );
    TASK_CHECK( strstr( svg, "<text x=\"0\" y=\"200\"" ) != 0 );
    TASK_CHECK( strstr( svg, ">Hello</text>" ) != 0 );
    TASK_CHECK( strstr( svg, ">World</text>" ) != 0 );
    free( svg );
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
  { "region_stream", "defaults_and_fields", case_region_defaults_and_fields },
  { "region_stream", "defaults", case_region_defaults },
  { "region_stream", "chunk_publication", case_region_chunk_publication },
  { "region_stream", "publication_timing", case_region_publication_timing },
  { "region_stream", "eof_publication", case_region_eof_publication },
  { "region_stream", "crlf_split", case_region_crlf_split },
  { "region_stream", "variants", case_region_variants },
  { "region_stream", "bad_field_recovery", case_region_bad_field_recovery },
  { "region_stream", "missing_id_dropped", case_region_missing_id_dropped },
  { "region_stream", "after_cue_ignored", case_region_after_cue_ignored },
  { "region_stream", "word_in_body", case_region_word_in_body },

  { "binding_lifetime", "settings_order", case_binding_settings_order },
  { "binding_lifetime", "last_region_wins",
    case_binding_last_region_setting_wins },
  { "binding_lifetime", "unknown_id", case_binding_unknown_id },
  { "binding_lifetime", "parser_delete", case_binding_survives_parser_delete },
  { "binding_lifetime", "region_outlives_table",
    case_binding_region_outlives_table },
  { "binding_lifetime", "cue_release_order", case_binding_cue_release_order },

  { "rollup_history", "over_lines", case_rollup_over_lines },
  { "rollup_history", "evicted_stay_gone",
    case_rollup_evicted_rows_stay_gone },
  { "rollup_history", "jump_leaves_eviction_trace",
    case_rollup_jump_leaves_eviction_trace },
  { "rollup_history", "partial_cue", case_rollup_partial_cue },
  { "rollup_history", "jump_processes_arrivals",
    case_rollup_jump_processes_arrivals },
  { "rollup_history", "same_time_order", case_rollup_same_time_order },
  { "rollup_history", "region_order", case_rollup_region_order },

  { "seek_replay", "ended_arrivals", case_seek_replays_ended_arrivals },
  { "seek_replay", "matches_advance", case_seek_history_matches_advance },
  { "seek_replay", "rejects_backwards_advance",
    case_seek_rejects_backwards_advance },
  { "seek_replay", "players_independent", case_seek_players_independent },
  { "seek_replay", "creation_lifetime", case_seek_creation_lifetime },
  { "seek_replay", "empty_inputs", case_seek_empty_inputs },

  { "svg_output", "coordinates_and_escaping",
    case_svg_coordinates_and_escaping },
  { "svg_output", "empty_and_groups", case_svg_empty_and_groups },
  { "svg_output", "validation", case_svg_validation },
  { "svg_output", "utf8_and_special_text", case_svg_utf8_and_special_text },
  { "svg_output", "example_binary", case_svg_example_binary },
};

const size_t task_case_count = ARRAY_LENGTH( task_cases );
