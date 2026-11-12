/* FluidLite monophonic legato support.
 *
 * See fluid_legato.h for the design overview.  This module owns the
 * per-channel physical key priority list and the cents-domain glide
 * trajectory; the synth module hooks note-on/note-off and block
 * rendering into it.
 */

#include "fluid_legato.h"
#include "fluid_synth.h"
#include "fluid_voice.h"
#include "fluid_chan.h"

#include <math.h>

/* Plain base pitch of a MIDI key in midicents (channel micro-tuning
 * and SoundFont tuning are handled separately). */
static fluid_real_t
fluid_legato_key_cents(int key)
{
  return (fluid_real_t)(100 * key);
}

fluid_legato_t*
new_fluid_legato(int polyphony)
{
  fluid_legato_t* legato = FLUID_NEW(fluid_legato_t);
  fluid_legato_group_t* group;
  if (legato == NULL) {
    FLUID_LOG(FLUID_ERR, "Out of memory");
    return NULL;
  }
  legato->enabled = 0;
  legato->glide_seconds = 0.0;

  /* The group and its voice table are allocated once here, never on
   * the render path. */
  group = FLUID_NEW(fluid_legato_group_t);
  if (group == NULL) {
    FLUID_FREE(legato);
    FLUID_LOG(FLUID_ERR, "Out of memory");
    return NULL;
  }
  FLUID_MEMSET(group, 0, sizeof(*group));
  group->voice = FLUID_ARRAY(fluid_voice_t*, polyphony);
  if (group->voice == NULL) {
    FLUID_FREE(group);
    FLUID_FREE(legato);
    FLUID_LOG(FLUID_ERR, "Out of memory");
    return NULL;
  }
  legato->group = group;
  return legato;
}

int
delete_fluid_legato(fluid_legato_t* legato)
{
  if (legato == NULL) {
    return FLUID_OK;
  }
  /* Voices themselves are owned and freed by the synth. */
  if (legato->group != NULL) {
    FLUID_FREE(legato->group->voice);
    FLUID_FREE(legato->group);
  }
  FLUID_FREE(legato);
  return FLUID_OK;
}

void
fluid_legato_reset(fluid_legato_t* legato)
{
  fluid_legato_group_t* group = legato->group;
  if (group == NULL) {
    return;
  }
  group->nkeys = 0;
  group->nvoice = 0;
  group->gliding = 0;
  group->glide_j = 0;
  group->noteid = 0;
}

int
fluid_legato_can_set_mode(fluid_legato_t* legato)
{
  fluid_legato_group_t* group = legato->group;
  return group->nkeys == 0 && group->nvoice == 0;
}

/* Restart the glide towards key from the pitch currently being
 * generated.  A re-target mid-glide continues from the actual current
 * position, never from the previous key's pitch. */
static void
fluid_legato_retarget(fluid_synth_t* synth, fluid_legato_t* legato,
                      fluid_legato_group_t* group, int key)
{
  fluid_real_t c0;
  unsigned int n;

  c0 = fluid_legato_base_pitch(group, 0);

  group->glide_c0 = c0;
  group->glide_ct = fluid_legato_key_cents(key);
  /* N = round(glide_seconds * sample_rate); N = 0 jumps immediately. */
  n = (unsigned int) floor(0.5 +
        legato->glide_seconds * synth->sample_rate);
  group->glide_n = n;
  group->glide_j = 0;
  group->gliding = (n > 0) && (group->glide_ct != c0);
}

static int
fluid_legato_find_key(fluid_legato_group_t* group, int key)
{
  int i;
  for (i = 0; i < group->nkeys; i++) {
    if (group->keys[i] == (unsigned char) key) {
      return i;
    }
  }
  return -1;
}

int
fluid_legato_noteon(fluid_synth_t* synth, int chan, int key,
                    int* start_noteon)
{
  fluid_legato_t* legato = synth->channel[chan]->legato;
  fluid_legato_group_t* group;
  int idx, i;

  *start_noteon = 0;

  if (!legato->enabled) {
    *start_noteon = 1;
    return FLUID_OK;
  }

  group = legato->group;

  if (group->nkeys == 0 || group->nvoice == 0) {
    /* First physical key: the caller runs the normal preset note-on. */
    group->nkeys = 0;
    group->nvoice = 0;
    group->gliding = 0;
    group->glide_j = 0;
    group->glide_c0 = fluid_legato_key_cents(key);
    group->glide_ct = fluid_legato_key_cents(key);
    group->keys[group->nkeys++] = (unsigned char) key;
    *start_noteon = 1;
    return FLUID_OK;
  }

  /* Already sounding: reuse the group.  A repeated note-on of a held
   * key only moves it to the end of the priority list. */
  idx = fluid_legato_find_key(group, key);
  if (idx >= 0) {
    for (i = idx; i < group->nkeys - 1; i++) {
      group->keys[i] = group->keys[i + 1];
    }
    group->keys[group->nkeys - 1] = (unsigned char) key;
  } else {
    if (group->nkeys >= FLUID_LEGATO_MAX_KEYS) {
      return FLUID_FAILED;
    }
    group->keys[group->nkeys++] = (unsigned char) key;
  }

  fluid_legato_retarget(synth, legato, group, key);
  return FLUID_OK;
}

int
fluid_legato_adopt_group(fluid_synth_t* synth, int chan, int key,
                         unsigned int noteid)
{
  fluid_legato_t* legato = synth->channel[chan]->legato;
  fluid_legato_group_t* group = legato->group;
  int i, n = 0;

  if (!legato->enabled) {
    return FLUID_OK;
  }

  /* Collect every voice created by this note-on.  Multi-zone presets
   * produce several voices, all sharing one note id. */
  for (i = 0; i < synth->polyphony && n < synth->polyphony; i++) {
    fluid_voice_t* voice = synth->voice[i];
    if (_PLAYING(voice) && (int) voice->chan == chan
        && fluid_voice_get_id(voice) == noteid) {
      group->voice[n++] = voice;
      voice->legato_group = group;
      /* Freeze the current pitch offset (SoundFont fine/coarse tuning,
       * micro-tuning and any active pitch bend).  The glide drives the
       * plain key pitch underneath; later bend changes - including the
       * wheel returning to center - cannot alter this offset. */
      voice->legato_pitch_offset = voice->pitch
                                   - fluid_legato_key_cents(key);
    }
  }

  group->nvoice = n;
  group->noteid = noteid;
  group->glide_c0 = fluid_legato_key_cents(key);
  group->glide_ct = fluid_legato_key_cents(key);
  group->gliding = 0;
  group->glide_j = 0;

  return n > 0 ? FLUID_OK : FLUID_FAILED;
}

int
fluid_legato_noteoff(fluid_synth_t* synth, int chan, int key,
                     int* release_group)
{
  fluid_legato_t* legato = synth->channel[chan]->legato;
  fluid_legato_group_t* group;
  int idx, i, was_current;

  *release_group = 0;

  if (!legato->enabled || legato->group == NULL) {
    return FLUID_OK;
  }
  group = legato->group;

  idx = fluid_legato_find_key(group, key);
  if (idx < 0) {
    return 0;
  }

  was_current = (group->keys[group->nkeys - 1] == (unsigned char) key);

  for (i = idx; i < group->nkeys - 1; i++) {
    group->keys[i] = group->keys[i + 1];
  }
  group->nkeys--;

  if (group->nkeys == 0) {
    /* Last physical key up: the caller runs the ordinary note-off on
     * every voice (sustain pedal rules included). */
    *release_group = 1;
    group->gliding = 0;
    return 1;
  }

  if (was_current) {
    /* Glide towards the newest remaining held key.  Releasing any other
     * key leaves the sounding pitch untouched. */
    fluid_legato_retarget(synth, legato, group,
                          group->keys[group->nkeys - 1]);
  }

  return 1;
}

void
fluid_legato_detach_group(fluid_synth_t* synth, int chan)
{
  fluid_legato_t* legato = synth->channel[chan]->legato;
  fluid_legato_group_t* group = legato->group;
  int i;

  if (group == NULL) {
    return;
  }

  /* The released voices keep the pitch the glide has actually reached:
   * bake it into the voice's pitch generators, then detach so they no
   * longer follow glide updates (and regain ordinary bend behaviour
   * from that pitch). */
  for (i = 0; i < group->nvoice; i++) {
    fluid_voice_t* voice = group->voice[i];
    fluid_real_t current_pitch;
    if (voice == NULL) {
      continue;
    }
    current_pitch = fluid_legato_current_pitch(group)
                    + voice->legato_pitch_offset;
    /* Fold tuning and the frozen bend into GEN_PITCH and clear the
     * other pitch contributors, so the normal modulation chain
     * continues from the pitch the glide has actually reached.  Later
     * pitch-bend events overwrite GEN_PITCH.mod through the voice's
     * own bend modulator. */
    voice->gen[GEN_PITCH].val = current_pitch;
    voice->gen[GEN_PITCH].mod = 0.0;
    voice->gen[GEN_PITCH].nrpn = 0.0;
    voice->gen[GEN_COARSETUNE].val = 0.0;
    voice->gen[GEN_COARSETUNE].mod = 0.0;
    voice->gen[GEN_COARSETUNE].nrpn = 0.0;
    voice->gen[GEN_FINETUNE].val = 0.0;
    voice->gen[GEN_FINETUNE].mod = 0.0;
    voice->gen[GEN_FINETUNE].nrpn = 0.0;
    voice->pitch = current_pitch;
    voice->legato_group = NULL;
    group->voice[i] = NULL;
  }
  fluid_legato_reset(legato);
}

void
fluid_legato_process_block(fluid_synth_t* synth)
{
  int c;
  for (c = 0; c < synth->midi_channels; c++) {
    fluid_legato_t* legato = synth->channel[c]->legato;
    fluid_legato_group_t* group;
    if (!legato->enabled || legato->group == NULL) {
      continue;
    }
    group = legato->group;
    if (group->gliding) {
      group->glide_j += FLUID_BUFSIZE;
      if (group->glide_j >= group->glide_n) {
        /* Segment completed; clamp and hold the target. */
        group->glide_j = group->glide_n;
        group->gliding = 0;
        group->glide_c0 = group->glide_ct;
      }
    }
  }
}

fluid_real_t
fluid_legato_base_pitch(fluid_legato_group_t* group, int j)
{
  double pos;

  if (!group->gliding || group->glide_n == 0) {
    return group->glide_ct;
  }

  /* j is relative to the start of the block currently being rendered;
   * glide_j counts the samples already generated in earlier blocks. */
  pos = (double)(group->glide_j + (unsigned int) j)
        / (double) group->glide_n;
  if (pos >= 1.0) {
    return group->glide_ct;
  }

  return group->glide_c0 + (group->glide_ct - group->glide_c0)
         * (fluid_real_t) pos;
}

fluid_real_t
fluid_legato_current_pitch(fluid_legato_group_t* group)
{
  double pos;

  /* Unlike base_pitch(), do not shortcut to the target when the
   * segment is no longer flagged as gliding: a last-key note-off or a
   * reset can stop the clock mid-segment, and the reached position
   * (c0, ct, j, n are retained) must be preserved. */
  if (group->glide_n == 0) {
    return group->glide_ct;
  }
  pos = (double) group->glide_j / (double) group->glide_n;
  if (pos >= 1.0) {
    return group->glide_ct;
  }
  return group->glide_c0 + (group->glide_ct - group->glide_c0)
         * (fluid_real_t) pos;
}
