/*
 * SMF Format 2 layered playback support for libADLMIDI.
 *
 * The declarations in this header are the internal integration points used by
 * adlmidi.cpp.  The public configuration API is declared in adlmidi.h.
 */

#ifndef ADLMIDI_SMF2_LAYERED_HPP
#define ADLMIDI_SMF2_LAYERED_HPP

#include <adlmidi.h>

#ifdef __cplusplus

namespace adl_smf2
{

/* Configure the optional layered mode before music is loaded. */
int setLayered(ADL_MIDIPlayer *device, int enabled);

/* Drop all state owned by a device.  Called before deleting its MIDIplay. */
void forget(ADL_MIDIPlayer *device);

/* Mark any successful music load so later configuration changes are rejected. */
void noteLoadSuccess(ADL_MIDIPlayer *device);

/*
 * Try to open data in layered SMF2 mode.
 * Returns 1 when handled successfully, 0 when the legacy loader should be
 * used, and -1 when layered SMF2 data was recognized but rejected.
 */
int openData(ADL_MIDIPlayer *device, const void *data, unsigned long size);

/* File wrapper used by adl_openFile(). */
int openFile(ADL_MIDIPlayer *device, const char *path);

/* Render through adl_play()'s signed 16-bit interleaved path. */
int playS16(ADL_MIDIPlayer *device, int sampleCount, short *out);

double totalTimeLength(ADL_MIDIPlayer *device);
double positionTell(ADL_MIDIPlayer *device);
void positionSeek(ADL_MIDIPlayer *device, double seconds);
void positionRewind(ADL_MIDIPlayer *device);
void handleReset(ADL_MIDIPlayer *device);
void setTempo(ADL_MIDIPlayer *device, double multiplier);
int atEnd(ADL_MIDIPlayer *device);

/* Test instrumentation: called immediately before a live event is submitted. */
typedef void (*DebugEventHook)(ADL_MIDIPlayer *device,
                               int track,
                               int order,
                               int status,
                               int data1,
                               int data2,
                               long long outputFrame,
                               void *userData);
void setDebugEventHook(DebugEventHook hook, void *userData);

} // namespace adl_smf2

#endif /* __cplusplus */

#endif /* ADLMIDI_SMF2_LAYERED_HPP */
