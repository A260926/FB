/* REGION extension API contract. Implementations are intentionally absent. */
#ifndef WEBVTT_REGION_H
#define WEBVTT_REGION_H
#include <stddef.h>
#include <stdio.h>
#include <webvtt/parser.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Immutable after publication. Fields use percentage units, not fractions. */
typedef struct webvtt_region_t {
  struct webvtt_refcount_t refs;
  webvtt_string id;
  double width, region_anchor_x, region_anchor_y;
  double viewport_anchor_x, viewport_anchor_y;
  webvtt_uint lines;
  webvtt_bool scroll_up;
} webvtt_region;
typedef void (WEBVTT_CALLBACK *webvtt_region_fn)(void *, webvtt_region *);
WEBVTT_EXPORT webvtt_status webvtt_parser_set_region_callback(webvtt_parser, webvtt_region_fn);
WEBVTT_EXPORT void webvtt_ref_region(webvtt_region *);
WEBVTT_EXPORT void webvtt_release_region(webvtt_region **);
/* Add struct webvtt_region_t *region to webvtt_cue; NULL means unbound.
 * A bound cue owns a reference; releasing the last cue reference releases it. */
typedef struct webvtt_region_player_t webvtt_region_player;
typedef struct webvtt_region_row_t {
  const webvtt_region *region;
  size_t cue_index;
  webvtt_uint line_index;
  const char *text;
} webvtt_region_row;
WEBVTT_EXPORT webvtt_status webvtt_region_player_create(webvtt_cue *const *, size_t, webvtt_region_player **);
WEBVTT_EXPORT void webvtt_region_player_delete(webvtt_region_player *);
WEBVTT_EXPORT webvtt_status webvtt_region_player_advance(webvtt_region_player *, webvtt_timestamp);
WEBVTT_EXPORT webvtt_status webvtt_region_player_seek(webvtt_region_player *, webvtt_timestamp);
/* Borrowed array/text, valid until advance, seek or delete. Empty -> count=0. */
WEBVTT_EXPORT const webvtt_region_row *webvtt_region_player_rows(const webvtt_region_player *, size_t *);
WEBVTT_EXPORT webvtt_status webvtt_region_player_write_svg(const webvtt_region_player *, double, double, double, FILE *);
#ifdef __cplusplus
}
#endif
#endif
