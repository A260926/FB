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

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webvtt/region.h>
#include <webvtt/node.h>

/**
 * Scroll-up region player.
 *
 * The player owns references to its cues and to every region it displays.
 * It never writes to the cues or to their node trees: visible text is copied
 * out of the tree when a cue arrives.
 *
 * Roll-up history is modelled by processing every arriving cue in order,
 * even when a single advance jumps across the cue's entire lifetime. Rows
 * that overflow a region are evicted for good; expiring some other cue later
 * does not bring them back. seek() rebuilds the state by replaying the same
 * arrival history from the beginning.
 */

typedef struct p_row_t {
  size_t cue_index;
  webvtt_uint line_index;
  char *text;
  webvtt_timestamp until;
} p_row;

typedef struct p_region_t {
  webvtt_region *region;
  p_row *rows;
  size_t count;
  size_t alloc;
} p_region;

typedef struct p_event_t {
  webvtt_cue *cue;
  size_t index;
  webvtt_timestamp from;
  webvtt_timestamp until;
  webvtt_region *region;
  size_t region_index;
  int displayable;
} p_event;

struct webvtt_region_player_t {
  p_event *events;
  size_t event_count;
  p_region *regions;
  size_t region_count;
  webvtt_region_row *snapshot;
  size_t snapshot_count;
  size_t snapshot_alloc;
  size_t cursor;
  webvtt_timestamp last_time;
  int has_advanced;
};

static void
row_free( p_row *row )
{
  if( row->text ) {
    webvtt_free( row->text );
    row->text = 0;
  }
}

static void
region_rows_clear( p_region *pr )
{
  size_t i;
  for( i = 0; i < pr->count; ++i ) {
    row_free( &pr->rows[ i ] );
  }
  pr->count = 0;
}

static void
player_clear_rows( webvtt_region_player *player )
{
  size_t i;
  for( i = 0; i < player->region_count; ++i ) {
    region_rows_clear( &player->regions[ i ] );
  }
}

static void
player_free_snapshot( webvtt_region_player *player )
{
  if( player->snapshot ) {
    webvtt_free( player->snapshot );
    player->snapshot = 0;
  }
  player->snapshot_count = 0;
  player->snapshot_alloc = 0;
}

/**
 * Collect visible cue text from the parsed node tree in document order.
 * TEXT nodes contribute their bytes, TIME_STAMP nodes are skipped and the
 * whole RUBY_TEXT subtree is skipped. Newlines inside TEXT are preserved.
 */
static webvtt_status
collect_text( const webvtt_node *node, webvtt_string *out )
{
  webvtt_uint i;

  if( !node ) {
    return WEBVTT_SUCCESS;
  }

  if( node->kind == WEBVTT_TEXT ) {
    return webvtt_string_append_string( out, &node->data.text );
  }
  if( node->kind == WEBVTT_TIME_STAMP ) {
    return WEBVTT_SUCCESS;
  }
  if( WEBVTT_IS_LEAF( node->kind ) ) {
    return WEBVTT_SUCCESS;
  }
  if( node->kind == WEBVTT_RUBY_TEXT ) {
    return WEBVTT_SUCCESS;
  }
  if( !node->data.internal_data ) {
    return WEBVTT_SUCCESS;
  }

  for( i = 0; i < node->data.internal_data->length; ++i ) {
    webvtt_status status = collect_text(
        node->data.internal_data->children[ i ], out );
    if( WEBVTT_FAILED( status ) ) {
      return status;
    }
  }
  return WEBVTT_SUCCESS;
}

/**
 * Split 'text' on '\n', keeping empty lines (including a trailing one).
 * Returns success with *out == NULL and *count == 0 for empty input.
 */
static webvtt_status
split_lines( const char *text, char ***out, size_t *count )
{
  size_t n = 0, alloc = 0, start = 0, i, len;
  char **lines = 0;

  *out = 0;
  *count = 0;
  if( !text || !*text ) {
    return WEBVTT_SUCCESS;
  }

  len = strlen( text );
  for( i = 0; i <= len; ++i ) {
    if( i == len || text[ i ] == '\n' ) {
      size_t part_len = i - start;
      char *copy;
      if( n + 1 > alloc ) {
        size_t new_alloc = alloc ? alloc * 2 : 4;
        char **grown = ( char ** )webvtt_alloc(
            sizeof( char * ) * new_alloc );
        if( !grown ) {
          goto oom;
        }
        if( lines ) {
          memcpy( grown, lines, sizeof( char * ) * n );
          webvtt_free( lines );
        }
        lines = grown;
        alloc = new_alloc;
      }
      copy = ( char * )webvtt_alloc( part_len + 1 );
      if( !copy ) {
        goto oom;
      }
      if( part_len ) {
        memcpy( copy, text + start, part_len );
      }
      copy[ part_len ] = 0;
      lines[ n++ ] = copy;
      start = i + 1;
    }
  }

  *out = lines;
  *count = n;
  return WEBVTT_SUCCESS;

oom:
  for( i = 0; i < n; ++i ) {
    webvtt_free( lines[ i ] );
  }
  if( lines ) {
    webvtt_free( lines );
  }
  return WEBVTT_OUT_OF_MEMORY;
}

static webvtt_status
region_push_row( p_region *pr, size_t cue_index, webvtt_uint line_index,
                 char *text, webvtt_timestamp until )
{
  if( pr->count + 1 > pr->alloc ) {
    size_t new_alloc = pr->alloc ? pr->alloc * 2 : 8;
    p_row *rows = ( p_row * )webvtt_alloc( sizeof( p_row ) * new_alloc );
    if( !rows ) {
      return WEBVTT_OUT_OF_MEMORY;
    }
    if( pr->rows ) {
      memcpy( rows, pr->rows, sizeof( p_row ) * pr->count );
      webvtt_free( pr->rows );
    }
    pr->rows = rows;
    pr->alloc = new_alloc;
  }
  pr->rows[ pr->count ].cue_index = cue_index;
  pr->rows[ pr->count ].line_index = line_index;
  pr->rows[ pr->count ].text = text;
  pr->rows[ pr->count ].until = until;
  pr->count++;
  return WEBVTT_SUCCESS;
}

/**
 * Evict the oldest rows until the region holds at most 'lines' rows.
 */
static void
region_evict( p_region *pr )
{
  size_t limit = pr->region->lines;
  while( pr->count > limit ) {
    row_free( &pr->rows[ 0 ] );
    memmove( pr->rows, pr->rows + 1, sizeof( p_row ) * ( pr->count - 1 ) );
    pr->count--;
  }
}

static webvtt_status
region_add_cue( p_region *pr, const p_event *event )
{
  webvtt_string text;
  webvtt_status status;
  char **lines = 0;
  size_t count = 0, i;

  webvtt_init_string( &text );
  status = collect_text( event->cue->node_head, &text );
  if( WEBVTT_FAILED( status ) ) {
    webvtt_release_string( &text );
    return status;
  }

  status = split_lines( webvtt_string_text( &text ), &lines, &count );
  webvtt_release_string( &text );
  if( WEBVTT_FAILED( status ) ) {
    return status;
  }

  for( i = 0; i < count; ++i ) {
    status = region_push_row( pr, event->index, ( webvtt_uint )i,
                              lines[ i ], event->until );
    if( WEBVTT_FAILED( status ) ) {
      size_t j;
      for( j = i; j < count; ++j ) {
        webvtt_free( lines[ j ] );
      }
      webvtt_free( lines );
      return status;
    }
  }
  if( lines ) {
    webvtt_free( lines );
  }

  region_evict( pr );
  return WEBVTT_SUCCESS;
}

static void
player_expire( webvtt_region_player *player, webvtt_timestamp when )
{
  size_t r, i, w;
  for( r = 0; r < player->region_count; ++r ) {
    p_region *pr = &player->regions[ r ];
    w = 0;
    for( i = 0; i < pr->count; ++i ) {
      if( pr->rows[ i ].until <= when ) {
        row_free( &pr->rows[ i ] );
      } else {
        if( w != i ) {
          pr->rows[ w ] = pr->rows[ i ];
        }
        ++w;
      }
    }
    pr->count = w;
  }
}

static int
event_compare( const void *a, const void *b )
{
  const p_event *ea = ( const p_event * )a;
  const p_event *eb = ( const p_event * )b;
  if( ea->from < eb->from ) {
    return -1;
  }
  if( ea->from > eb->from ) {
    return 1;
  }
  if( ea->index < eb->index ) {
    return -1;
  }
  if( ea->index > eb->index ) {
    return 1;
  }
  return 0;
}

static int
cue_is_displayable( const webvtt_cue *cue )
{
  return cue && cue->region && cue->region->scroll_up;
}

static webvtt_status
player_rebuild_snapshot( webvtt_region_player *player )
{
  size_t total = 0, r, i, at = 0;

  for( r = 0; r < player->region_count; ++r ) {
    total += player->regions[ r ].count;
  }

  player->snapshot_count = 0;
  if( total == 0 ) {
    return WEBVTT_SUCCESS;
  }

  if( total > player->snapshot_alloc ) {
    webvtt_region_row *rows = ( webvtt_region_row * )webvtt_alloc(
        sizeof( webvtt_region_row ) * total );
    if( !rows ) {
      return WEBVTT_OUT_OF_MEMORY;
    }
    if( player->snapshot ) {
      webvtt_free( player->snapshot );
    }
    player->snapshot = rows;
    player->snapshot_alloc = total;
  }

  for( r = 0; r < player->region_count; ++r ) {
    p_region *pr = &player->regions[ r ];
    for( i = 0; i < pr->count; ++i ) {
      player->snapshot[ at ].region = pr->region;
      player->snapshot[ at ].cue_index = pr->rows[ i ].cue_index;
      player->snapshot[ at ].line_index = pr->rows[ i ].line_index;
      player->snapshot[ at ].text = pr->rows[ i ].text;
      ++at;
    }
  }
  player->snapshot_count = at;
  return WEBVTT_SUCCESS;
}

static webvtt_status
player_apply_until( webvtt_region_player *player, webvtt_timestamp target )
{
  webvtt_status status = WEBVTT_SUCCESS;

  while( player->cursor < player->event_count &&
         player->events[ player->cursor ].from <= target ) {
    p_event *event = &player->events[ player->cursor ];
    player_expire( player, event->from );
    if( event->displayable && event->from < event->until ) {
      status = region_add_cue( &player->regions[ event->region_index ],
                               event );
      if( WEBVTT_FAILED( status ) ) {
        return status;
      }
    }
    player->cursor++;
  }

  player_expire( player, target );
  return player_rebuild_snapshot( player );
}

WEBVTT_EXPORT webvtt_status
webvtt_region_player_create( webvtt_cue *const *cues, size_t count,
                             webvtt_region_player **pout )
{
  webvtt_region_player *player;
  size_t i;

  if( !pout || ( !cues && count ) ) {
    return WEBVTT_INVALID_PARAM;
  }
  *pout = 0;

  player = ( webvtt_region_player * )webvtt_alloc0( sizeof( *player ) );
  if( !player ) {
    return WEBVTT_OUT_OF_MEMORY;
  }

  if( count ) {
    player->events = ( p_event * )webvtt_alloc0( sizeof( p_event ) * count );
    if( !player->events ) {
      webvtt_free( player );
      return WEBVTT_OUT_OF_MEMORY;
    }
  }

  /* Keep a reference to every valid cue, preserving the input order. */
  for( i = 0; i < count; ++i ) {
    webvtt_cue *cue = cues[ i ];
    p_event *event;
    if( !cue || !webvtt_validate_cue( cue ) ) {
      continue;
    }
    event = &player->events[ player->event_count++ ];
    event->cue = cue;
    event->index = i;
    event->from = cue->from;
    event->until = cue->until;
    event->region = cue->region;
    event->displayable = cue_is_displayable( cue );
    webvtt_ref_cue( cue );
  }

  if( player->event_count ) {
    player->regions = ( p_region * )webvtt_alloc0(
        sizeof( p_region ) * player->event_count );
    if( !player->regions ) {
      webvtt_region_player_delete( player );
      return WEBVTT_OUT_OF_MEMORY;
    }
  }

  /* Region display order: first appearance in the input cue array. */
  for( i = 0; i < player->event_count; ++i ) {
    p_event *event = &player->events[ i ];
    size_t r;
    if( !event->displayable ) {
      continue;
    }
    for( r = 0; r < player->region_count; ++r ) {
      if( player->regions[ r ].region == event->region ) {
        break;
      }
    }
    if( r == player->region_count ) {
      player->regions[ r ].region = event->region;
      player->regions[ r ].rows = 0;
      player->regions[ r ].count = 0;
      player->regions[ r ].alloc = 0;
      webvtt_ref_region( event->region );
      player->region_count++;
    }
    event->region_index = r;
  }

  if( player->event_count > 1 ) {
    qsort( player->events, player->event_count, sizeof( p_event ),
           event_compare );
  }

  *pout = player;
  return WEBVTT_SUCCESS;
}

WEBVTT_EXPORT void
webvtt_region_player_delete( webvtt_region_player *player )
{
  size_t i;
  if( !player ) {
    return;
  }
  for( i = 0; i < player->event_count; ++i ) {
    if( player->events[ i ].cue ) {
      webvtt_release_cue( &player->events[ i ].cue );
    }
  }
  if( player->events ) {
    webvtt_free( player->events );
  }
  for( i = 0; i < player->region_count; ++i ) {
    region_rows_clear( &player->regions[ i ] );
    if( player->regions[ i ].rows ) {
      webvtt_free( player->regions[ i ].rows );
    }
    webvtt_release_region( &player->regions[ i ].region );
  }
  if( player->regions ) {
    webvtt_free( player->regions );
  }
  player_free_snapshot( player );
  webvtt_free( player );
}

WEBVTT_EXPORT webvtt_status
webvtt_region_player_advance( webvtt_region_player *player,
                              webvtt_timestamp t )
{
  if( !player ) {
    return WEBVTT_INVALID_PARAM;
  }
  if( player->has_advanced ) {
    if( t < player->last_time ) {
      return WEBVTT_INVALID_PARAM;
    }
    if( t == player->last_time ) {
      return WEBVTT_SUCCESS;
    }
  } else {
    player->has_advanced = 1;
    player->last_time = t;
  }

  player->last_time = t;
  return player_apply_until( player, t );
}

WEBVTT_EXPORT webvtt_status
webvtt_region_player_seek( webvtt_region_player *player, webvtt_timestamp t )
{
  if( !player ) {
    return WEBVTT_INVALID_PARAM;
  }

  player_clear_rows( player );
  player_free_snapshot( player );
  player->cursor = 0;
  player->has_advanced = 1;
  player->last_time = t;

  return player_apply_until( player, t );
}

WEBVTT_EXPORT const webvtt_region_row *
webvtt_region_player_rows( const webvtt_region_player *player, size_t *count )
{
  if( count ) {
    *count = player ? player->snapshot_count : 0;
  }
  if( !player || !player->snapshot_count ) {
    return 0;
  }
  return player->snapshot;
}

/**
 * Write a number in the C locale, without exponents for ordinary values.
 */
static int
write_number( FILE *out, double value )
{
  char buffer[ 64 ];
  const char *point;
  char *c;

  if( value == 0.0 ) {
    value = 0.0; /* normalise -0 */
  }
  snprintf( buffer, sizeof( buffer ), "%.10g", value );

  /* Force the C locale decimal point even when the process locale differs. */
  point = localeconv() ? localeconv()->decimal_point : 0;
  if( point && point[ 0 ] && !( point[ 0 ] == '.' && point[ 1 ] == '\0' ) ) {
    size_t plen = strlen( point );
    c = strstr( buffer, point );
    if( c ) {
      *c = '.';
      if( plen > 1 ) {
        memmove( c + 1, c + plen, strlen( c + plen ) + 1 );
      }
    }
  }
  return fputs( buffer, out ) >= 0;
}

static int
write_escaped( FILE *out, const char *text )
{
  const unsigned char *c = ( const unsigned char * )text;
  for( ; *c; ++c ) {
    const char *replacement = 0;
    switch( *c ) {
      case '&': replacement = "&amp;"; break;
      case '<': replacement = "&lt;"; break;
      case '>': replacement = "&gt;"; break;
      case '"': replacement = "&quot;"; break;
      case '\'': replacement = "&apos;"; break;
      default: break;
    }
    if( replacement ) {
      if( fputs( replacement, out ) < 0 ) {
        return 0;
      }
    } else if( fputc( *c, out ) == EOF ) {
      return 0;
    }
  }
  return 1;
}

WEBVTT_EXPORT webvtt_status
webvtt_region_player_write_svg( const webvtt_region_player *player,
                                double width, double height,
                                double line_step, FILE *out )
{
  size_t r, i, emitted = 0;
  int ok = 1;

  if( !player || !out || !isfinite( width ) || !isfinite( height )
      || !isfinite( line_step ) || width <= 0 || height <= 0
      || line_step <= 0 ) {
    return WEBVTT_INVALID_PARAM;
  }

  ok = fputs( "<svg xmlns=\"http://www.w3.org/2000/svg\"", out ) >= 0;
  if( ok ) {
    ok = fputs( " width=\"", out ) >= 0 && write_number( out, width );
  }
  if( ok ) {
    ok = fputs( "\" height=\"", out ) >= 0 && write_number( out, height );
  }
  if( ok ) {
    ok = fputs( "\" viewBox=\"0 0 ", out ) >= 0 && write_number( out, width )
         && fputc( ' ', out ) != EOF && write_number( out, height )
         && fputs( "\">\n", out ) >= 0;
  }
  if( !ok ) {
    return WEBVTT_UNSUCCESSFUL;
  }

  for( r = 0; r < player->region_count; ++r ) {
    const p_region *pr = &player->regions[ r ];
    const webvtt_region *region = pr->region;
    double rw, rh, x, y;
    size_t n = pr->count;

    if( n == 0 ) {
      continue;
    }

    rw = width * region->width / 100.0;
    rh = ( double )region->lines * line_step;
    x = width * region->viewport_anchor_x / 100.0
        - rw * region->region_anchor_x / 100.0;
    y = height * region->viewport_anchor_y / 100.0
        - rh * region->region_anchor_y / 100.0;

    ok = fputs( "<defs><clipPath id=\"region-clip-", out ) >= 0
         && fprintf( out, "%lu", ( unsigned long )emitted ) >= 0
         && fputs( "\"><rect x=\"", out ) >= 0 && write_number( out, x )
         && fputs( "\" y=\"", out ) >= 0 && write_number( out, y )
         && fputs( "\" width=\"", out ) >= 0 && write_number( out, rw )
         && fputs( "\" height=\"", out ) >= 0 && write_number( out, rh )
         && fputs( "\"/></clipPath></defs>\n", out ) >= 0;
    if( !ok ) {
      return WEBVTT_UNSUCCESSFUL;
    }

    ok = fputs( "<g data-region=\"", out ) >= 0
         && write_escaped( out, webvtt_string_text( &region->id ) )
         && fputs( "\" clip-path=\"url(#region-clip-", out ) >= 0
         && fprintf( out, "%lu", ( unsigned long )emitted ) >= 0
         && fputs( ")\">\n", out ) >= 0;
    if( !ok ) {
      return WEBVTT_UNSUCCESSFUL;
    }

    for( i = 0; i < n; ++i ) {
      double baseline = y + ( ( double )region->lines - ( double )n
                              + ( double )i + 1.0 ) * line_step;
      ok = fputs( "<text x=\"", out ) >= 0 && write_number( out, x )
           && fputs( "\" y=\"", out ) >= 0 && write_number( out, baseline )
           && fprintf( out, "\" data-cue-index=\"%lu\" data-line-index=\"%lu\">",
                       ( unsigned long )pr->rows[ i ].cue_index,
                       ( unsigned long )pr->rows[ i ].line_index ) >= 0
           && write_escaped( out, pr->rows[ i ].text )
           && fputs( "</text>\n", out ) >= 0;
      if( !ok ) {
        return WEBVTT_UNSUCCESSFUL;
      }
    }

    ok = fputs( "</g>\n", out ) >= 0;
    if( !ok ) {
      return WEBVTT_UNSUCCESSFUL;
    }
    ++emitted;
  }

  if( fputs( "</svg>\n", out ) < 0 ) {
    return WEBVTT_UNSUCCESSFUL;
  }
  if( ferror( out ) ) {
    return WEBVTT_UNSUCCESSFUL;
  }
  return WEBVTT_SUCCESS;
}
