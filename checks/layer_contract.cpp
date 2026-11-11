// Contract tests for the canvas_ity layer (group-opacity) API.
//
// These exercise begin_layer()/end_layer() directly through the public
// canvas interface: group fading on transparent and opaque backgrounds,
// nested layers, the deferred parent clip (including a half-coverage edge),
// erasing inside a layer, invalid calls, and the state and path isolation
// that must be restored when a layer ends.
//
#define CANVAS_ITY_IMPLEMENTATION
#include "canvas_ity.hpp"
#include <cstdio>
#include <limits>

using namespace canvas_ity;

static int failures = 0;

static void check(
    bool condition,
    char const *message )
{
    if ( condition )
        printf( "PASS %s\n", message );
    else
    {
        printf( "FAIL %s\n", message );
        ++failures;
    }
}

static bool near_byte(
    int value,
    int expected )
{
    int delta = value - expected;
    return -1 <= delta && delta <= 1;
}

int main()
{
    // A single opacity applied once must fade a whole group uniformly:
    // two overlapping opaque rectangles keep the same ~.5 alpha in their
    // overlap as everywhere else, on a transparent background.
    {
        canvas c( 6, 2 );
        c.set_color( fill_style, 1.0f, 0.0f, 0.0f, 1.0f );
        check( c.begin_layer( 0.5f ), "begin layer returns true" );
        c.fill_rectangle( 0, 0, 4, 2 );
        c.fill_rectangle( 2, 0, 4, 2 );
        check( c.end_layer(), "end layer returns true" );
        unsigned char image[ 48 ];
        c.get_image_data( image, 6, 2, 24, 0, 0 );
        bool uniform = true;
        for ( int x = 0; x < 6; ++x )
            uniform = uniform && near_byte( image[ x * 4 + 3 ], 127 );
        check( uniform, "group fade alpha ~127/128 including overlap" );
        check( image[ 0 ] > 200 && image[ 1 ] == 0 && image[ 2 ] == 0,
               "group fade keeps the layer color" );
    }

    // The same group fade over an opaque background stays opaque and blends
    // the layer color over the background color.
    {
        canvas c( 4, 1 );
        c.set_color( fill_style, 0.0f, 0.0f, 1.0f, 1.0f );
        c.fill_rectangle( 0, 0, 4, 1 );
        check( c.begin_layer( 0.5f ), "begin layer over opaque background" );
        c.set_color( fill_style, 1.0f, 0.0f, 0.0f, 1.0f );
        c.fill_rectangle( 0, 0, 4, 1 );
        check( c.end_layer(), "end layer over opaque background" );
        unsigned char image[ 16 ];
        c.get_image_data( image, 4, 1, 16, 0, 0 );
        check( image[ 3 ] == 255 && image[ 0 ] > 100 && image[ 2 ] > 100,
               "fade blends over opaque background without transparency" );
    }

    // Nested layers multiply their opacities: .5 inside .5 fades opaque red
    // to an alpha of about .25 (~63/64).
    {
        canvas c( 4, 1 );
        c.set_color( fill_style, 1.0f, 0.0f, 0.0f, 1.0f );
        check( c.begin_layer( 0.5f ), "begin outer nested layer" );
        check( c.begin_layer( 0.5f ), "begin inner nested layer" );
        c.fill_rectangle( 0, 0, 4, 1 );
        check( c.end_layer(), "end inner nested layer" );
        check( c.end_layer(), "end outer nested layer" );
        unsigned char image[ 16 ];
        c.get_image_data( image, 4, 1, 16, 0, 0 );
        bool quarter = true;
        for ( int x = 0; x < 4; ++x )
            quarter = quarter && near_byte( image[ x * 4 + 3 ], 64 );
        check( quarter, "nested .5/.5 layers give alpha ~63/64" );
    }

    // The parent clip is deferred to the single compositing step.  On a clip
    // edge with .5 coverage, an opaque layer ended at opacity .5 yields alpha
    // .25 (~63/64): clip and opacity are each applied once, not the clip
    // twice (which would give .125).
    {
        canvas c( 8, 1 );
        c.begin_path();
        c.rectangle( 0, 0, 4.5f, 1 );
        c.clip();
        check( c.begin_layer( 0.5f ), "begin layer with parent clip" );
        c.set_color( fill_style, 1.0f, 0.0f, 0.0f, 1.0f );
        c.fill_rectangle( 0, 0, 8, 1 );
        // While the layer is active its pixels are read back unclipped.
        unsigned char inside[ 32 ];
        c.get_image_data( inside, 8, 1, 32, 0, 0 );
        check( inside[ 7 * 4 + 3 ] == 255,
               "parent clip is not applied while drawing into the layer" );
        check( c.end_layer(), "end layer with parent clip" );
        unsigned char image[ 32 ];
        c.get_image_data( image, 8, 1, 32, 0, 0 );
        check( near_byte( image[ 4 * 4 + 3 ], 64 ),
               "half-coverage clip edge faded once to alpha ~63/64" );
        check( near_byte( image[ 3 ], 127 ) &&
               image[ 5 * 4 + 3 ] == 0,
               "parent clip fully applied inside and not outside" );
    }

    // destination_out inside a layer erases only layer pixels; the parent
    // target shows through the hole and is itself untouched.
    {
        canvas c( 4, 1 );
        c.set_color( fill_style, 0.0f, 0.0f, 1.0f, 1.0f );
        c.fill_rectangle( 0, 0, 4, 1 );
        check( c.begin_layer( 1.0f ), "begin layer for erasing" );
        c.set_color( fill_style, 1.0f, 0.0f, 0.0f, 1.0f );
        c.fill_rectangle( 0, 0, 4, 1 );
        c.global_composite_operation = destination_out;
        c.fill_rectangle( 1, 0, 1, 1 );
        check( c.end_layer(), "end erased layer" );
        unsigned char image[ 16 ];
        c.get_image_data( image, 4, 1, 16, 0, 0 );
        check( image[ 0 ] == 255 && image[ 2 ] == 0,
               "layer color drawn where not erased" );
        check( image[ 1 * 4 + 0 ] == 0 &&
               image[ 1 * 4 + 2 ] == 255 &&
               image[ 1 * 4 + 3 ] == 255,
               "in-layer erase reveals the untouched parent" );
    }

    // A clip established inside the layer is local to it and does not become
    // the parent's clip after the layer ends.
    {
        canvas c( 8, 1 );
        c.set_color( fill_style, 1.0f, 0.0f, 0.0f, 1.0f );
        check( c.begin_layer( 1.0f ), "begin layer for local clip" );
        c.begin_path();
        c.rectangle( 0, 0, 2, 1 );
        c.clip();
        c.fill_rectangle( 0, 0, 8, 1 );
        check( c.end_layer(), "end locally clipped layer" );
        unsigned char image[ 32 ];
        c.get_image_data( image, 8, 1, 32, 0, 0 );
        check( image[ 1 * 4 + 3 ] == 255 &&
               image[ 2 * 4 + 3 ] == 0,
               "layer-local clip confines layer drawing" );
        c.set_color( fill_style, 0.0f, 0.0f, 1.0f, 1.0f );
        c.fill_rectangle( 0, 0, 8, 1 );
        c.get_image_data( image, 8, 1, 32, 0, 0 );
        check( image[ 7 * 4 + 3 ] == 255,
               "parent clip is restored to the full canvas" );
    }

    // Invalid opacity values are rejected without changing any state, and
    // ending without an active layer is a side-effect-free no-op.
    {
        canvas c( 2, 2 );
        float bad[] = {
            -0.01f, 1.01f,
            std::numeric_limits< float >::infinity(),
            -std::numeric_limits< float >::infinity(),
            std::numeric_limits< float >::quiet_NaN() };
        bool rejected = true;
        for ( int index = 0; index < 5; ++index )
            rejected = rejected && !c.begin_layer( bad[ index ] );
        check( rejected, "non-finite or out-of-range opacity rejected" );
        check( !c.end_layer(), "end with no active layer returns false" );
        c.set_color( fill_style, 1.0f, 0.0f, 0.0f, 1.0f );
        c.fill_rectangle( 0, 0, 2, 2 );
        unsigned char image[ 16 ];
        c.get_image_data( image, 2, 2, 8, 0, 0 );
        check( image[ 3 ] == 255 && image[ 15 ] == 255,
               "rejected begin calls leave the canvas usable" );
        // Boundary opacities are accepted and behave at their extremes.
        check( c.begin_layer( 0.0f ), "zero opacity accepted" );
        check( c.end_layer(), "zero-opacity layer ended" );
        check( c.begin_layer( 1.0f ), "unit opacity accepted" );
        check( c.end_layer(), "unit-opacity layer ended" );
    }

    // Ending a layer ignores the transform, shadows, and global alpha in
    // effect at that moment; the layer is composited with its own opacity.
    {
        canvas c( 4, 1 );
        c.set_color( fill_style, 1.0f, 0.0f, 0.0f, 1.0f );
        check( c.begin_layer( 1.0f ), "begin layer for settings test" );
        c.fill_rectangle( 0, 0, 4, 1 );
        c.set_global_alpha( 0.25f );
        c.translate( 10.0f, 10.0f );
        c.set_shadow_color( 0.0f, 0.0f, 0.0f, 1.0f );
        c.shadow_offset_x = 5.0f;
        check( c.end_layer(), "end ignores in-layer drawing settings" );
        unsigned char image[ 16 ];
        c.get_image_data( image, 4, 1, 16, 0, 0 );
        check( image[ 3 ] == 255 && image[ 15 ] == 255,
               "layer composited at full opacity in place" );
    }

    // The parent path is preserved across a layer and can be filled after
    // the layer ends, while drawing settings are restored to the parent's.
    {
        canvas c( 4, 1 );
        c.set_color( fill_style, 0.0f, 1.0f, 0.0f, 1.0f );
        c.begin_path();
        c.rectangle( 0, 0, 2, 1 );
        check( c.begin_layer( 1.0f ), "begin layer preserving parent path" );
        c.set_color( fill_style, 1.0f, 0.0f, 0.0f, 1.0f );
        c.global_composite_operation = destination_out;
        c.set_global_alpha( 0.25f );
        c.begin_path();
        c.rectangle( 0, 0, 1, 1 );
        check( c.end_layer(), "end restores parent path and state" );
        c.fill();
        unsigned char image[ 16 ];
        c.get_image_data( image, 4, 1, 16, 0, 0 );
        check( image[ 1 * 4 + 1 ] == 255 &&
               image[ 1 * 4 + 0 ] == 0 &&
               image[ 2 * 4 + 3 ] == 0,
               "parent path and green source-over state restored" );
    }

    // The parent save stack is preserved, restore() cannot cross a layer
    // boundary, and saves left unmatched inside a layer are discarded.
    {
        canvas c( 8, 1 );
        c.save();
        c.begin_path();
        c.rectangle( 0, 0, 4, 1 );
        c.clip();
        check( c.begin_layer( 1.0f ), "begin layer preserving save stack" );
        c.restore();  // no-op: the layer starts with an empty save stack
        c.save();
        c.save();     // unmatched saves must be dropped on end_layer()
        check( c.end_layer(), "end discards unmatched in-layer saves" );
        c.restore();  // restores the single parent state saved earlier
        c.set_color( fill_style, 0.0f, 0.0f, 1.0f, 1.0f );
        c.fill_rectangle( 0, 0, 8, 1 );
        unsigned char image[ 32 ];
        c.get_image_data( image, 8, 1, 32, 0, 0 );
        check( image[ 7 * 4 + 3 ] == 255,
               "parent save stack restored across the layer" );
    }

    // Pixel read/write calls address the active layer.
    {
        canvas c( 2, 1 );
        check( c.begin_layer( 1.0f ), "begin layer for pixel IO" );
        unsigned char pixels[ 8 ] = { 10, 20, 30, 40, 50, 60, 70, 80 };
        c.put_image_data( pixels, 2, 1, 8, 0, 0 );
        unsigned char readback[ 8 ];
        c.get_image_data( readback, 2, 1, 8, 0, 0 );
        check( readback[ 0 ] >= 9 && readback[ 0 ] <= 11 &&
               readback[ 3 ] >= 39 && readback[ 3 ] <= 41,
               "get/put image data address the active layer" );
        check( c.end_layer(), "end layer used for pixel IO" );
        unsigned char composited[ 8 ];
        c.get_image_data( composited, 2, 1, 8, 0, 0 );
        check( composited[ 3 ] >= 39 && composited[ 3 ] <= 41,
               "layer pixels are composited after ending" );
    }

    // A zero-opacity layer never changes the parent target.
    {
        canvas c( 4, 1 );
        c.set_color( fill_style, 0.0f, 0.0f, 1.0f, 1.0f );
        c.fill_rectangle( 0, 0, 4, 1 );
        check( c.begin_layer( 0.0f ), "begin zero-opacity layer" );
        c.set_color( fill_style, 1.0f, 0.0f, 0.0f, 1.0f );
        c.fill_rectangle( 0, 0, 4, 1 );
        check( c.end_layer(), "end zero-opacity layer" );
        unsigned char image[ 16 ];
        c.get_image_data( image, 4, 1, 16, 0, 0 );
        check( image[ 2 ] == 255 && image[ 0 ] == 0 && image[ 3 ] == 255,
               "zero-opacity layer leaves the parent untouched" );
    }

    // Destroying a canvas with active, nested layers and unmatched saves
    // must release every layer and its state without leaking or crashing.
    {
        canvas c( 4, 4 );
        c.begin_layer( 0.7f );
        c.begin_layer( 0.3f );
        c.set_color( fill_style, 1.0f, 1.0f, 0.0f, 1.0f );
        c.fill_rectangle( 0, 0, 4, 4 );
        c.save();
        c.save();
    }
    printf( "PASS destruction releases active nested layers\n" );

    if ( failures )
    {
        printf( "%d layer contract check(s) failed\n", failures );
        return 1;
    }
    printf( "All layer contract checks passed\n" );
    return 0;
}
