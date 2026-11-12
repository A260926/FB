/* FluidLite monophonic legato support (internal API).
 *
 * A legato group is the set of voices created by one physical key
 * press while legato mode is enabled on a channel.  As long as at
 * least one key is held, further note-on events retarget the existing
 * group's glide instead of starting new voices: no zone is reselected,
 * no sample, phase, envelope, LFO, filter history or velocity
 * modulation is reset.
 */

#ifndef _FLUID_LEGATO_H
#define _FLUID_LEGATO_H

#include "fluidsynth_priv.h"

/* Maximum number of physically held keys on one channel. */
#define FLUID_LEGATO_MAX_KEYS 128

typedef struct _fluid_legato_group_t fluid_legato_group_t;

struct _fluid_legato_group_t
{  /* Voices that make up this (possibly multi-zone) articulation.
   * Allocated once at synthesizer creation, never on the render path. */
  fluid_voice_t** voice;
  int nvoice;
  unsigned int noteid;          /* voice id shared by the whole group */

  /* Physical key list, most recently pressed last. */
  unsigned char keys[FLUID_LEGATO_MAX_KEYS];
  int nkeys;

  /* Base pitch glide in midicents, excluding pitch bend, SoundFont
   * fine/coarse tuning and LFO / envelope pitch contributions. */
  fluid_real_t glide_c0;        /* pitch at the start of the segment */
  fluid_real_t glide_ct;        /* target pitch of the segment */
  unsigned int glide_n;         /* segment length in generated samples */
  unsigned int glide_j;         /* samples generated in this segment */
  char gliding;                 /* nonzero while a finite glide is running */
};

/* Per-channel legato state. */
typedef struct _fluid_legato_t fluid_legato_t;

struct _fluid_legato_t
{
  char enabled;
  double glide_seconds;
  fluid_legato_group_t* group;  /* always allocated while the synth lives */
};

fluid_legato_t* new_fluid_legato(int polyphony);
int delete_fluid_legato(fluid_legato_t* legato);

/* Clear the key list and glide target, detaching any voices.  The
 * voices themselves are left untouched so callers can apply their
 * usual release / stop policy first. */
void fluid_legato_reset(fluid_legato_t* legato);

/* True if legato mode may be toggled: no held keys and no active group. */
int fluid_legato_can_set_mode(fluid_legato_t* legato);

/*
 * Note-on handling.
 *
 * For the first held key *start_noteon is set nonzero and the caller
 * runs the normal preset note-on, then adopts the created voices with
 * fluid_legato_adopt_group().  Every later note-on is handled here:
 * the key priority list is updated and the glide retargeted, without
 * restarting any voice.  A repeated note-on of a held key only moves
 * it to the end of the priority list.
 */
int fluid_legato_noteon(fluid_synth_t* synth, int chan, int key,
                        int* start_noteon);

/* Adopt the voices just created by a normal preset note-on. */
int fluid_legato_adopt_group(fluid_synth_t* synth, int chan, int key,
                             unsigned int noteid);

/*
 * Note-off handling.
 *
 * Removes the key from the priority list.  Releasing the current key
 * retargets the glide to the newest remaining held key; releasing any
 * other key leaves the sounding pitch untouched.  When the last
 * physical key is released *release_group is set and the caller
 * performs the ordinary voice note-off (sustain pedal rules included),
 * followed by fluid_legato_detach_group().
 *
 * Returns 1 when the key was physically held and removed, 0 otherwise.
 */
int fluid_legato_noteoff(fluid_synth_t* synth, int chan, int key,
                         int* release_group);

/* Detach the group from its voices.  Each voice keeps sounding at the
 * pitch the glide has reached so far, but no longer receives glide
 * updates; then the key list and glide target are cleared. */
void fluid_legato_detach_group(fluid_synth_t* synth, int chan);

/* Advance the glide clocks by one rendered 64-sample block.  Called
 * from fluid_synth_one_block() after the voices have been rendered, so
 * the clock follows samples actually generated, not write_float()
 * call counts. */
void fluid_legato_process_block(fluid_synth_t* synth);

/* Per-sample base pitch (midicents, excluding bend / tuning / LFO
 * terms) for sample j (0-based) of the block currently being rendered. */
fluid_real_t fluid_legato_base_pitch(fluid_legato_group_t* group, int j);

/* Pitch the glide has actually reached at the start of the next block,
 * regardless of whether the segment is still running.  Used when a
 * group is detached so its voices keep the reached pitch. */
fluid_real_t fluid_legato_current_pitch(fluid_legato_group_t* group);

#endif /* _FLUID_LEGATO_H */
