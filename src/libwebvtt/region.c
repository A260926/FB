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

#include <string.h>
#include "parser_internal.h"
#include "cue_internal.h"

/**
 * REGION definition blocks.
 *
 * A block is the token "REGION" on a line of its own, followed by key:value
 * lines and terminated by an empty line. The block is consumed by the same
 * streaming parser that consumes cues; definitions are published only once
 * the terminating empty line has been seen. Publishing swaps the definition
 * into this parser's lookup table (replacing an earlier version with the
 * same id) without touching the earlier object, which may already have been
 * handed to the application.
 */

static webvtt_region *
region_create( void )
{
  webvtt_region *region = ( webvtt_region * )webvtt_alloc0( sizeof( *region ) );
  if( !region ) {
    return 0;
  }
  region->refs.value = 1;
  webvtt_init_string( &region->id );
  region->width = 100;
  region->lines = 3;
  region->region_anchor_x = 0;
  region->region_anchor_y = 100;
  region->viewport_anchor_x = 0;
  region->viewport_anchor_y = 100;
  region->scroll_up = 0;
  return region;
}

WEBVTT_EXPORT void
webvtt_ref_region( webvtt_region *region )
{
  if( region ) {
    webvtt_ref( &region->refs );
  }
}

WEBVTT_EXPORT void
webvtt_release_region( webvtt_region **pregion )
{
  if( pregion && *pregion ) {
    webvtt_region *region = *pregion;
    *pregion = 0;
    if( webvtt_deref( &region->refs ) == 0 ) {
      webvtt_release_string( &region->id );
      webvtt_free( region );
    }
  }
}

WEBVTT_EXPORT webvtt_status
webvtt_parser_set_region_callback( webvtt_parser self,
                                   webvtt_region_fn on_region )
{
  if( !self ) {
    return WEBVTT_INVALID_PARAM;
  }
  self->on_region = on_region;
  return WEBVTT_SUCCESS;
}

/**
 * Report a recoverable region error. Returns the parser status; a negative
 * value from the error callback aborts parsing, matching the rest of the
 * library.
 */
static webvtt_status
region_error( webvtt_parser self, webvtt_uint line, webvtt_error error )
{
  if( self->error
      && self->error( self->userdata, line, 1, error ) < 0 ) {
    return WEBVTT_PARSE_ERROR;
  }
  return WEBVTT_SUCCESS;
}

/**
 * Parse a percentage in [0,100] requiring a trailing '%'. The plain
 * non-negative decimal form is accepted, e.g. "50", "12.5%".
 */
static int
region_parse_percentage( const char *text, double *out )
{
  const char *c = text;
  double value = 0.0;
  double fraction = 0.1;
  int have_digit = 0;
  int in_fraction = 0;

  if( !text || !*text ) {
    return 0;
  }

  for( ; *c; ++c ) {
    char ch = *c;
    if( ch >= '0' && ch <= '9' ) {
      have_digit = 1;
      if( in_fraction ) {
        value += ( double )( ch - '0' ) * fraction;
        fraction *= 0.1;
      } else {
        value = value * 10.0 + ( double )( ch - '0' );
      }
    } else if( ch == '.' && !in_fraction ) {
      in_fraction = 1;
    } else if( ch == '%' && c[1] == '\0' ) {
      if( !have_digit || value < 0.0 || value > 100.0 ) {
        return 0;
      }
      *out = value;
      return 1;
    } else {
      return 0;
    }
  }
  return 0;
}

/**
 * Parse a comma separated percentage pair, e.g. "0%,100%".
 */
static int
region_parse_anchor( const char *text, double *x, double *y )
{
  const char *comma;
  webvtt_string first;
  int ok;

  if( !text || !( comma = strchr( text, ',' ) ) ) {
    return 0;
  }
  if( strchr( comma + 1, ',' ) ) {
    return 0;
  }

  webvtt_init_string( &first );
  if( WEBVTT_FAILED( webvtt_create_string_with_text(
          &first, text, ( int )( comma - text ) ) ) ) {
    webvtt_release_string( &first );
    return 0;
  }

  ok = region_parse_percentage( webvtt_string_text( &first ), x ) &&
       region_parse_percentage( comma + 1, y );
  webvtt_release_string( &first );
  return ok;
}

/**
 * Parse a positive decimal integer for 'lines'.
 */
static int
region_parse_lines( const char *text, webvtt_uint *out )
{
  const char *c = text;
  webvtt_uint64 value = 0;
  int have_digit = 0;

  if( !text || !*text ) {
    return 0;
  }
  for( ; *c; ++c ) {
    if( *c < '0' || *c > '9' ) {
      return 0;
    }
    have_digit = 1;
    value = value * 10 + ( webvtt_uint64 )( *c - '0' );
    if( value > 0xFFFFFFFFu ) {
      return 0;
    }
  }
  if( !have_digit || value == 0 ) {
    return 0;
  }
  *out = ( webvtt_uint )value;
  return 1;
}

/**
 * Return true when 'value' is a syntactically usable region id: non-empty,
 * no whitespace and no cue separator. Bytes are otherwise preserved.
 */
static int
region_valid_id( const char *value )
{
  const char *c;
  if( !value || !*value ) {
    return 0;
  }
  for( c = value; *c; ++c ) {
    if( *c == ' ' || *c == '\t' || *c == '\r' || *c == '\n' || *c == '\f' ) {
      return 0;
    }
  }
  return strstr( value, "-->" ) == 0;
}

/**
 * Apply one key:value line to the region under construction. A bad value
 * keeps the previous value and reports a recoverable error at the start of
 * the offending line. Unknown keywords are ignored.
 */
static webvtt_status
region_apply_field( webvtt_parser self, webvtt_region *region,
                    const char *line, webvtt_uint line_no )
{
  const char *colon;
  const char *value;
  webvtt_string key;
  webvtt_status status;
  int bad = 0;

  colon = strchr( line, ':' );
  if( !colon ) {
    return WEBVTT_SUCCESS;
  }

  webvtt_init_string( &key );
  if( WEBVTT_FAILED( status = webvtt_create_string_with_text(
          &key, line, ( int )( colon - line ) ) ) ) {
    return status;
  }
  value = colon + 1;

  if( webvtt_string_is_equal( &key, "id", -1 ) ) {
    if( region_valid_id( value ) ) {
      webvtt_string new_id;
      if( WEBVTT_FAILED( status = webvtt_create_string_with_text(
              &new_id, value, -1 ) ) ) {
        webvtt_release_string( &key );
        return status;
      }
      webvtt_release_string( &region->id );
      region->id = new_id;
      self->region_have_id = 1;
    } else {
      bad = 1;
    }
  } else if( webvtt_string_is_equal( &key, "width", -1 ) ) {
    double v;
    if( ( bad = !region_parse_percentage( value, &v ) ) == 0 ) {
      region->width = v;
    }
  } else if( webvtt_string_is_equal( &key, "lines", -1 ) ) {
    webvtt_uint v;
    if( ( bad = !region_parse_lines( value, &v ) ) == 0 ) {
      region->lines = v;
    }
  } else if( webvtt_string_is_equal( &key, "regionanchor", -1 ) ) {
    double x, y;
    if( ( bad = !region_parse_anchor( value, &x, &y ) ) == 0 ) {
      region->region_anchor_x = x;
      region->region_anchor_y = y;
    }
  } else if( webvtt_string_is_equal( &key, "viewportanchor", -1 ) ) {
    double x, y;
    if( ( bad = !region_parse_anchor( value, &x, &y ) ) == 0 ) {
      region->viewport_anchor_x = x;
      region->viewport_anchor_y = y;
    }
  } else if( webvtt_string_is_equal( &key, "scroll", -1 ) ) {
    /* Only "up" enables scrolling; unknown values are silently ignored. */
    if( !strcmp( value, "up" ) ) {
      region->scroll_up = 1;
    }
  }

  webvtt_release_string( &key );

  if( bad ) {
    return region_error( self, line_no, WEBVTT_REGION_BAD_VALUE );
  }
  return status;
}

/**
 * Publish a completed region: replace or append in the parser's lookup
 * table. The table holds a reference of its own; the callback receives a
 * borrowed reference. An older version with the same id is never modified,
 * only dropped from the table.
 */
static webvtt_status
region_publish( webvtt_parser self, webvtt_region *region )
{
  webvtt_uint i;
  webvtt_region **slot = 0;

  for( i = 0; i < self->region_count; ++i ) {
    webvtt_region *existing = self->regions[ i ];
    if( webvtt_string_is_equal( &existing->id,
                                webvtt_string_text( &region->id ), -1 ) ) {
      slot = &self->regions[ i ];
      break;
    }
  }

  if( slot ) {
    webvtt_release_region( slot );
    webvtt_ref_region( region );
    *slot = region;
  } else {
    if( self->region_count + 1 > self->region_alloc ) {
      webvtt_uint new_alloc = self->region_alloc
                              ? ( self->region_alloc << 1 ) : 4;
      webvtt_region **regions = ( webvtt_region ** )webvtt_alloc0(
          sizeof( webvtt_region * ) * new_alloc );
      if( !regions ) {
        ERROR( WEBVTT_ALLOCATION_FAILED );
        return WEBVTT_OUT_OF_MEMORY;
      }
      if( self->regions ) {
        memcpy( regions, self->regions,
                sizeof( webvtt_region * ) * self->region_count );
        webvtt_free( self->regions );
      }
      self->regions = regions;
      self->region_alloc = new_alloc;
    }
    webvtt_ref_region( region );
    self->regions[ self->region_count++ ] = region;
  }

  if( self->on_region ) {
    self->on_region( self->userdata, region );
  }
  return WEBVTT_SUCCESS;
}

/**
 * Finish the block being parsed.
 */
static webvtt_status
region_end_block( webvtt_parser self )
{
  webvtt_status status = WEBVTT_SUCCESS;
  webvtt_region *region = self->region_building;

  self->region_building = 0;
  if( !region ) {
    return WEBVTT_SUCCESS;
  }

  if( self->region_discard ) {
    self->region_discard = 0;
    self->region_have_id = 0;
    status = region_error( self, self->region_start_line,
                           WEBVTT_REGION_AFTER_CUE );
    webvtt_release_region( &region );
    return status;
  }

  if( !self->region_have_id ) {
    self->region_have_id = 0;
    status = region_error( self, self->line, WEBVTT_REGION_MISSING_ID );
    webvtt_release_region( &region );
    return status;
  }

  self->region_have_id = 0;
  status = region_publish( self, region );
  webvtt_release_region( &region );
  return status;
}

WEBVTT_INTERN int
webvtt_region_is_keyword( const webvtt_string *line )
{
  /* The keyword occupies a line of its own, exactly as spelled. */
  return line && webvtt_string_length( line ) == 6
         && !memcmp( webvtt_string_text( line ), "REGION", 6 );
}

/**
 * Begin a REGION block. Called by the main parser once the complete
 * "REGION" line has been read and its terminating newline consumed.
 */
WEBVTT_INTERN webvtt_status
webvtt_region_begin( webvtt_parser self )
{
  if( self->region_building ) {
    /* Previous block was never terminated; finish it first. */
    webvtt_status status = region_end_block( self );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
  }

  self->region_building = region_create();
  if( !self->region_building ) {
    ERROR( WEBVTT_ALLOCATION_FAILED );
    return WEBVTT_OUT_OF_MEMORY;
  }
  self->region_discard = self->seen_cue;
  self->region_start_line = self->line;
  self->region_have_id = 0;
  self->region_line_ready = 0;
  if( !self->region_line.d ) {
    webvtt_init_string( &self->region_line );
  } else {
    webvtt_release_string( &self->region_line );
    webvtt_init_string( &self->region_line );
  }
  return WEBVTT_SUCCESS;
}

/**
 * Consume key:value lines for the current REGION block.
 *
 * Mirrors the cue-text line reader: a line may span several chunks and a
 * CRLF pair may be split across a chunk boundary. The terminating empty
 * line is consumed and the block is published immediately, without waiting
 * for finish. Returns when the current buffer is exhausted or when the
 * block ends.
 */
WEBVTT_INTERN webvtt_status
webvtt_region_read( webvtt_parser self, const char *b, webvtt_uint *ppos,
                    webvtt_uint len, webvtt_bool finish )
{
  webvtt_status status = WEBVTT_SUCCESS;
  webvtt_uint pos = *ppos;

  if( !self->region_building ) {
    self->mode = M_WEBVTT;
    *ppos = pos;
    return WEBVTT_SUCCESS;
  }

  for( ;; ) {
    if( !self->region_line_ready ) {
      int v = webvtt_string_getline( &self->region_line, b, &pos, ( int )len,
                                     &self->truncate, finish );
      if( v < 0 ) {
        ERROR( WEBVTT_ALLOCATION_FAILED );
        status = WEBVTT_OUT_OF_MEMORY;
        goto _finish;
      }
      if( v == 0 ) {
        /* Partial line; wait for the next chunk. */
        goto _finish;
      }
      self->region_line_ready = 1;
    }

    {
      webvtt_token token = webvtt_lex_newline( self, b, &pos, len, finish );
      webvtt_uint line_no = self->line;
      webvtt_uint length;
      const char *text;

      if( token != NEWLINE ) {
        /*
         * A CR was the last byte of the chunk. The line text is complete;
         * only the newline sequence carries over, so keep the line ready and
         * do not read it into the buffer a second time.
         */
        goto _finish;
      }

      self->region_line_ready = 0;
      self->token_pos = 0; /* do not let newline lexing accumulate */
      length = webvtt_string_length( &self->region_line );
      text = webvtt_string_text( &self->region_line );

      if( length == 0 ) {
        /* Empty line terminates the block and publishes it right away. */
        webvtt_release_string( &self->region_line );
        webvtt_init_string( &self->region_line );
        ++self->line;
        self->column = 1;
        self->mode = M_WEBVTT;
        /*
         * The terminating empty line is the same block separator the cue
         * reader consumes itself. Mark the surrounding T_EOL frame as having
         * seen the two newlines, so the next block starts cleanly.
         */
        if( ( self->top + 1 )->state == T_EOL ) {
          ( self->top + 1 )->v.value = 2;
        }
        /* Never let the newline lexer's buffer leak into the next cue line. */
        self->token_pos = 0;
        self->popped = 0;
        status = region_end_block( self );
        goto _finish;
      }

      if( !self->region_discard ) {
        status = region_apply_field( self, self->region_building, text,
                                     line_no );
      }
      webvtt_release_string( &self->region_line );
      webvtt_init_string( &self->region_line );
      ++self->line;
      self->column = 1;

      if( WEBVTT_FAILED( status ) ) {
        if( self->region_building ) {
          webvtt_release_region( &self->region_building );
        }
        self->region_discard = 0;
        self->region_have_id = 0;
        self->token_pos = 0;
        self->mode = M_WEBVTT;
        goto _finish;
      }
    }

    if( pos >= len && !finish ) {
      break;
    }
  }

_finish:
  *ppos = pos;
  return status;
}

/**
 * Release everything owned by this parser's region machinery.
 */
WEBVTT_INTERN void
webvtt_region_cleanup( webvtt_parser self )
{
  webvtt_uint i;
  if( !self ) {
    return;
  }
  webvtt_release_region( &self->region_building );
  self->region_discard = 0;
  self->region_have_id = 0;
  self->region_line_ready = 0;
  webvtt_release_string( &self->region_line );
  for( i = 0; i < self->region_count; ++i ) {
    webvtt_release_region( &self->regions[ i ] );
  }
  if( self->regions ) {
    webvtt_free( self->regions );
    self->regions = 0;
  }
  self->region_count = 0;
  self->region_alloc = 0;
}

/**
 * Bind the cue to a region named by its settings, if the final settings are
 * compatible. Runs after the whole timing/settings line was processed, so
 * the order of the settings on the line cannot matter.
 */
WEBVTT_INTERN void
webvtt_region_bind_cue( webvtt_parser self, webvtt_cue *cue )
{
  webvtt_uint i;

  if( !self || !cue || cue->region ) {
    return;
  }
  if( !( cue->flags & CUE_HAVE_REGION ) ) {
    return;
  }
  if( webvtt_string_is_empty( &cue->region_id ) ) {
    return;
  }
  if( cue->settings.vertical != WEBVTT_HORIZONTAL ) {
    return;
  }
  if( ( webvtt_uint )cue->settings.line != WEBVTT_AUTO ) {
    return;
  }
  if( cue->settings.size != 100 ) {
    return;
  }

  for( i = 0; i < self->region_count; ++i ) {
    webvtt_region *region = self->regions[ i ];
    if( webvtt_string_is_equal( &region->id,
                                webvtt_string_text( &cue->region_id ),
                                -1 ) ) {
      webvtt_ref_region( region );
      cue->region = region;
      return;
    }
  }
}
