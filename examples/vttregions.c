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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webvtt/parser.h>
#include <webvtt/cue.h>
#include <webvtt/region.h>

/**
 * vttregions input.vtt width height line_step time...
 *
 * Parses the file through the ordinary streaming entry points, deletes the
 * parser, and then renders one SVG frame per requested time. Times are
 * milliseconds. Moving backwards between frames uses seek(); everything
 * else uses advance(). Each frame is preceded by a "<!-- frame TIME -->"
 * comment on stdout. Diagnostics go to stderr and any failure returns
 * non-zero.
 */

typedef struct collector_t {
  webvtt_cue **cues;
  size_t count;
  size_t alloc;
  int oom;
} collector;

static void
collector_cue( void *userdata, webvtt_cue *cue )
{
  collector *c = ( collector * )userdata;
  if( c->oom ) {
    webvtt_release_cue( &cue );
    return;
  }
  if( c->count + 1 > c->alloc ) {
    size_t new_alloc = c->alloc ? c->alloc * 2 : 16;
    webvtt_cue **grown = ( webvtt_cue ** )realloc(
        c->cues, sizeof( webvtt_cue * ) * new_alloc );
    if( !grown ) {
      c->oom = 1;
      webvtt_release_cue( &cue );
      return;
    }
    c->cues = grown;
    c->alloc = new_alloc;
  }
  c->cues[ c->count++ ] = cue; /* ownership transferred to the collector */
}

static int
report_error( void *userdata, webvtt_uint line, webvtt_uint col,
              webvtt_error error )
{
  ( void )userdata;
  ( void )col;
  fprintf( stderr, "parse error line %u: %s\n", line,
           webvtt_strerror( error ) );
  return 0; /* recoverable */
}

static int
parse_nonnegative( const char *text, unsigned long long *out )
{
  char *end = 0;
  unsigned long long value;
  if( !text || !*text ) {
    return 0;
  }
  if( *text == '-' ) {
    return 0;
  }
  value = strtoull( text, &end, 10 );
  if( !end || *end ) {
    return 0;
  }
  *out = value;
  return 1;
}

static int
parse_positive_double( const char *text, double *out )
{
  char *end = 0;
  double value;
  if( !text || !*text ) {
    return 0;
  }
  value = strtod( text, &end );
  if( !end || *end || !isfinite( value ) || value <= 0.0 ) {
    return 0;
  }
  *out = value;
  return 1;
}

int
main( int argc, char **argv )
{
  FILE *in;
  collector collected;
  webvtt_parser parser = 0;
  webvtt_region_player *player = 0;
  webvtt_status status;
  char buffer[ 4096 ];
  size_t n_read;
  double width, height, line_step;
  unsigned long long *times;
  int time_count, i;
  webvtt_timestamp previous = 0;
  int have_previous = 0;
  int result = 1;

  memset( &collected, 0, sizeof( collected ) );

  if( argc < 6 ) {
    fprintf( stderr,
             "usage: %s input.vtt width height line_step time...\n",
             argv[ 0 ] );
    return 1;
  }

  if( !parse_positive_double( argv[ 2 ], &width )
      || !parse_positive_double( argv[ 3 ], &height )
      || !parse_positive_double( argv[ 4 ], &line_step ) ) {
    fprintf( stderr, "width, height and line_step must be positive numbers\n" );
    return 1;
  }

  time_count = argc - 5;
  times = ( unsigned long long * )calloc( ( size_t )time_count,
                                          sizeof( unsigned long long ) );
  if( !times ) {
    fprintf( stderr, "out of memory\n" );
    return 1;
  }
  for( i = 0; i < time_count; ++i ) {
    if( !parse_nonnegative( argv[ 5 + i ], &times[ i ] ) ) {
      fprintf( stderr, "invalid time: %s\n", argv[ 5 + i ] );
      goto done;
    }
  }

  in = fopen( argv[ 1 ], "rb" );
  if( !in ) {
    fprintf( stderr, "cannot open %s\n", argv[ 1 ] );
    goto done;
  }

  status = webvtt_create_parser( collector_cue, report_error, &collected,
                                 &parser );
  if( WEBVTT_FAILED( status ) ) {
    fprintf( stderr, "cannot create parser\n" );
    fclose( in );
    goto done;
  }

  while( ( n_read = fread( buffer, 1, sizeof( buffer ), in ) ) > 0 ) {
    if( WEBVTT_FAILED( status = webvtt_parse_chunk( parser, buffer,
                                                   ( webvtt_uint )n_read ) ) ) {
      fprintf( stderr, "parse failed (%d)\n", status );
      fclose( in );
      goto done;
    }
  }
  if( ferror( in ) ) {
    fprintf( stderr, "cannot read %s\n", argv[ 1 ] );
    fclose( in );
    goto done;
  }
  fclose( in );

  if( WEBVTT_FAILED( status = webvtt_finish_parsing( parser ) ) ) {
    fprintf( stderr, "finish failed (%d)\n", status );
    goto done;
  }

  /* The parser is no longer needed; the player keeps its own cue refs. */
  webvtt_delete_parser( parser );
  parser = 0;

  if( collected.oom ) {
    fprintf( stderr, "out of memory\n" );
    goto done;
  }

  status = webvtt_region_player_create( collected.cues, collected.count,
                                        &player );
  if( WEBVTT_FAILED( status ) ) {
    fprintf( stderr, "cannot create region player\n" );
    goto done;
  }

  /* Release the collector's own references; the player owns what it needs. */
  for( i = 0; i < ( int )collected.count; ++i ) {
    webvtt_release_cue( &collected.cues[ i ] );
  }
  free( collected.cues );
  collected.cues = 0;
  collected.count = 0;

  for( i = 0; i < time_count; ++i ) {
    webvtt_timestamp t = ( webvtt_timestamp )times[ i ];
    if( have_previous && t < previous ) {
      status = webvtt_region_player_seek( player, t );
    } else {
      status = webvtt_region_player_advance( player, t );
    }
    if( WEBVTT_FAILED( status ) ) {
      fprintf( stderr, "cannot play time %llu (%d)\n", times[ i ], status );
      goto done;
    }
    previous = t;
    have_previous = 1;

    if( printf( "<!-- frame %llu -->\n", times[ i ] ) < 0 ) {
      fprintf( stderr, "cannot write frame\n" );
      goto done;
    }
    status = webvtt_region_player_write_svg( player, width, height,
                                             line_step, stdout );
    if( WEBVTT_FAILED( status ) ) {
      fprintf( stderr, "cannot write SVG (%d)\n", status );
      goto done;
    }
    if( fflush( stdout ) != 0 ) {
      fprintf( stderr, "cannot write frame\n" );
      goto done;
    }
  }

  result = 0;

done:
  if( player ) {
    webvtt_region_player_delete( player );
  }
  if( parser ) {
    webvtt_delete_parser( parser );
  }
  if( collected.cues ) {
    for( i = 0; i < ( int )collected.count; ++i ) {
      webvtt_release_cue( &collected.cues[ i ] );
    }
    free( collected.cues );
  }
  free( times );
  return result;
}
