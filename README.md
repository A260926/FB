# libwebvtt
The library for interpreting and authoring conformant WebVTT content
WebVTT is a caption and subtitle format designed for use with HTML5
audio and video elements.

See: [W3C WebVTT Draft](http://dev.w3.org/html5/webvtt/)

[![Build Status](https://travis-ci.org/mozilla/webvtt.png?branch=dev)](https://travis-ci.org/mozilla/webvtt)

## Build Instructions:

In Unix-like environments, we use autotools:

```
./bootstrap.sh && ./configure && make
```

On Windows, we use a Visual Studio Project, see files in build/msvc2010

## Running Tests:

All tests are written using Google Test, and run using `make check`. You can configure the tests to run with our without valgrind, for memory checking.

Without valgrind:

```
./configure
make
make check
```

With valgrind:

```
./configure --enable-valgrind-testing
make
make check
```

When running tests with valgrind, any test that fails valgrind (even if it passes Google Test) will fail. See `test/unit/Makefile.am` for info on known test failures, and how to add/remove them.

## Routines available to application:
### Parser Object
        webvtt_status webvtt_create_parser( webvtt_cue_fn on_read, webvtt_error_fn on_error, void *userdata, webvtt_parser *ppout );
        void webvtt_delete_parser( webvtt_parser parser );
        webvtt_status webvtt_parse_chunk( webvtt_parser self, const void *buffer, webvtt_uint len );
        webvtt_status webvtt_finish_parsing( webvtt_parser self );

### WebVTT Cues
        webvtt_status webvtt_create_cue( webvtt_cue **pcue );
        void webvtt_ref_cue( webvtt_cue *cue );
        void webvtt_release_cue( webvtt_cue **pcue );
        int webvtt_validate_cue( webvtt_cue *cue );

### WebVTT Regions
        webvtt_status webvtt_parser_set_region_callback( webvtt_parser, webvtt_region_fn );
        void webvtt_ref_region( webvtt_region * );
        void webvtt_release_region( webvtt_region ** );

        webvtt_status webvtt_region_player_create( webvtt_cue *const *, size_t, webvtt_region_player ** );
        void webvtt_region_player_delete( webvtt_region_player * );
        webvtt_status webvtt_region_player_advance( webvtt_region_player *, webvtt_timestamp );
        webvtt_status webvtt_region_player_seek( webvtt_region_player *, webvtt_timestamp );
        const webvtt_region_row *webvtt_region_player_rows( const webvtt_region_player *, size_t * );
        webvtt_status webvtt_region_player_write_svg( const webvtt_region_player *, double, double, double, FILE * );

### WebVTT Nodes
        void webvtt_init_node( webvtt_node **node );
        void webvtt_ref_node( webvtt_node *node );
        void webvtt_release_node( webvtt_node **node );

### Application Callbacks
        typedef int ( WEBVTT_CALLBACK *webvtt_error_fn )( void *userdata, webvtt_uint line, webvtt_uint col, webvtt_error error );
        typedef void ( WEBVTT_CALLBACK *webvtt_cue_fn )( void *userdata, webvtt_cue *cue );
        typedef void ( WEBVTT_CALLBACK *webvtt_region_fn )( void *userdata, webvtt_region *region );

## REGION definitions and the scroll-up region player

A WebVTT file may open with `REGION` blocks between the signature and the
first cue:

```
WEBVTT

REGION
id:main
width:60%
lines:3
regionanchor:0%,100%
viewportanchor:20%,80%
scroll:up

00:00:01.000 --> 00:00:06.000 region:main
First line
Second line
```

The streaming entry points (`webvtt_parse_chunk` and
`webvtt_finish_parsing`) consume these blocks. A definition is published
only once its terminating blank line has been read, through the callback
installed with `webvtt_parser_set_region_callback`. A definition placed
after the first cue is ignored and reported.

A cue's `region:ID` setting is resolved against the parser's region table
after the entire timing/settings line has been processed. Unknown ids leave
the cue unbound, and a cue that ends up with `vertical` other than
horizontal, an explicit `line`, or a `size` other than 100% is not bound to
its region (it is still delivered through the ordinary cue callback).

### Reference lifetimes

* The region passed to `webvtt_region_fn` is **borrowed**. Call
  `webvtt_ref_region` if you want to keep it; release it later with
  `webvtt_release_region`. The parser's table holds its own reference.
* Replacing a definition with the same id only affects the lookup table for
  cues parsed later; an object already delivered to your callback is never
  modified, so retained references remain valid.
* A cue bound to a region owns a reference to it; the region is released
  when the last reference to the cue goes away. Deleting the parser does not
  invalidate cues or regions that the application already received.
* `webvtt_region_player_create` retains every valid cue and the regions it
  displays. The player never depends on the parser afterwards and never
  mutates the cues or their node trees, so the application may release its
  own cue references once the player exists.

### Playback

The player renders `scroll:up` regions as line queues. `advance(t)` processes
every cue arrival up to `t` in start-time then input order; on a tie, rows
that expire at `t` are removed before new cues are added. Lines pushed past
a region's `lines` limit are evicted from the top and never come back, even
if the cue that caused the overflow expires later. `seek(t)` clears the
display and replays the same arrival history from the beginning, so jumping
straight to a late time produces the same roll-up state as advancing there
one cue at a time. Two players do not share state.

`webvtt_region_player_rows` returns a borrowed array (valid until the next
advance, seek or delete); `webvtt_region_player_write_svg` writes one
complete SVG document with the region rectangles, clip paths and text lines.

A standalone example is provided:

```
examples/vttregions input.vtt width height line_step time...
```

It parses the file, deletes the parser, then prints one frame per requested
time (milliseconds) to stdout, each preceded by `<!-- frame TIME -->`.
Moving backwards between requested times uses `seek`; otherwise `advance` is
used. Diagnostics go to stderr and any failure returns non-zero.
        
### Strings
        void webvtt_init_string( webvtt_string *result );
        webvtt_status webvtt_create_string( webvtt_uint32 alloc, webvtt_string *result );
        webvtt_status webvtt_create_string_with_text( webvtt_string *result, const char *init_text, int len );
        void webvtt_ref_string( webvtt_string *str );
        void webvtt_release_string( webvtt_string *str );     
        webvtt_status webvtt_string_detach( webvtt_string *str );
        void webvtt_copy_string( webvtt_string *left, const webvtt_string *right );    
        webvtt_uint webvtt_string_is_empty( const webvtt_string *str );
        const char *webvtt_string_text( const webvtt_string *str );
        webvtt_uint32 webvtt_string_length( const webvtt_string *str );
        webvtt_uint32 webvtt_string_capacity( const webvtt_string *str );
        int webvtt_string_getline( webvtt_string *str, const char *buffer, webvtt_uint *pos, int len, int *truncate, webvtt_bool finish );
        webvtt_status webvtt_string_putc( webvtt_string *str, char to_append );
        webvtt_bool webvtt_string_is_equal( const webvtt_string *str, const char *to_compare, int len );
        webvtt_status webvtt_string_append( webvtt_string *str, const char *buffer, int len );
        webvtt_status webvtt_string_append_string( webvtt_string *str, const webvtt_string *other );

### UTF8 And UTF16 Conversion
        webvtt_bool webvtt_next_utf8( const char **begin, const char *end );
        webvtt_bool webvtt_skip_utf8( const char **begin, const char *end, int n_chars );
        webvtt_uint16 webvtt_utf8_to_utf16( const char *utf8, const char *end, webvtt_uint16 *high_surrogate );
        int webvtt_utf8_chcount( const char *utf8, const char *end );
        int webvtt_utf8_length( const char *utf8 );

### String List
        webvtt_status webvtt_create_stringlist( webvtt_stringlist **result );
        void webvtt_ref_stringlist( webvtt_stringlist *list );
        void webvtt_copy_stringlist( webvtt_stringlist **left, webvtt_stringlist *right );
        void webvtt_release_stringlist( webvtt_stringlist **list );
        webvtt_status webvtt_stringlist_push( webvtt_stringlist *list, webvtt_string *str );
        
### Memory Allocation
        void *webvtt_alloc( webvtt_uint nb );
        void *webvtt_alloc0( webvtt_uint nb );
        void webvtt_free( void *data );
        void webvtt_set_allocator( webvtt_alloc_fn_ptr alloc, webvtt_free_fn_ptr free, void *userdata );

### Memory Application Callbacks
        typedef void *(WEBVTT_CALLBACK *webvtt_alloc_fn_ptr)( void *userdata, webvtt_uint nbytes );
        typedef void (WEBVTT_CALLBACK *webvtt_free_fn_ptr)( void *userdata, void *pmem );

### Error handling
	const char *webvtt_strerror( webvtt_error );
  
## License
> Copyright (c) 2013 Mozilla Foundation and Contributors
> All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions are
> met:
>
> - Redistributions of source code must retain the above copyright
> notice, this list of conditions and the following disclaimer.
> - Redistributions in binary form must reproduce the above copyright
> notice, this list of conditions and the following disclaimer in the
> documentation and/or other materials provided with the distribution.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
> ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
> LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
> A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
> HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
> SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
> LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
> DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
> THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
> (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
> OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

