/*	MikMod sound library
	(c) 1998, 1999, 2000, 2001 Miodrag Vallat and others - see file AUTHORS
	for complete list.

	This library is free software; you can redistribute it and/or modify
	it under the terms of the GNU Library General Public License as
	published by the Free Software Foundation; either version 2 of
	the License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Library General Public License for more details.

	You should have received a copy of the GNU Library General Public
	License along with this library; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
	02111-1307, USA.
*/

/*==============================================================================

  Common source parts between the two software mixers.
  This file is probably the ugliest part of libmikmod.

==============================================================================*/

#if defined(HAVE_CONFIG_H) && !defined(_IN_VIRTCH_) /* config.h isn't guarded */
#include "config.h"
#endif
#include "mikmod_internals.h"

#ifndef NO_HQMIXER
extern ULONG VC1_SilenceBytes(SBYTE*,ULONG);
extern ULONG VC2_SilenceBytes(SBYTE*,ULONG);
extern ULONG VC1_WriteBytes(SBYTE*,ULONG);
extern ULONG VC2_WriteBytes(SBYTE*,ULONG);
extern void  VC1_Exit(void);
extern void  VC2_Exit(void);
extern UWORD VC1_VoiceGetVolume(UBYTE);
extern UWORD VC2_VoiceGetVolume(UBYTE);
extern ULONG VC1_VoiceGetPanning(UBYTE);
extern ULONG VC2_VoiceGetPanning(UBYTE);
extern void  VC1_VoiceSetFrequency(UBYTE,ULONG);
extern void  VC2_VoiceSetFrequency(UBYTE,ULONG);
extern ULONG VC1_VoiceGetFrequency(UBYTE);
extern ULONG VC2_VoiceGetFrequency(UBYTE);
extern void  VC1_VoicePlay(UBYTE,SWORD,ULONG,ULONG,ULONG,ULONG,UWORD);
extern void  VC2_VoicePlay(UBYTE,SWORD,ULONG,ULONG,ULONG,ULONG,UWORD);
extern void  VC1_VoiceStop(UBYTE);
extern void  VC2_VoiceStop(UBYTE);
extern BOOL  VC1_VoiceStopped(UBYTE);
extern BOOL  VC2_VoiceStopped(UBYTE);
extern SLONG VC1_VoiceGetPosition(UBYTE);
extern SLONG VC2_VoiceGetPosition(UBYTE);
extern void  VC1_VoiceSetVolume(UBYTE,UWORD);
extern void  VC2_VoiceSetVolume(UBYTE,UWORD);
extern void  VC1_VoiceSetPanning(UBYTE,ULONG);
extern void  VC2_VoiceSetPanning(UBYTE,ULONG);
extern void  VC1_SampleUnload(SWORD);
extern void  VC2_SampleUnload(SWORD);
extern SWORD VC1_SampleLoad(struct SAMPLOAD*,int);
extern SWORD VC2_SampleLoad(struct SAMPLOAD*,int);
extern ULONG VC1_SampleSpace(int);
extern ULONG VC2_SampleSpace(int);
extern ULONG VC1_SampleLength(int,SAMPLE*);
extern ULONG VC2_SampleLength(int,SAMPLE*);
extern ULONG VC1_VoiceRealVolume(UBYTE);
extern ULONG VC2_VoiceRealVolume(UBYTE);
#endif


#ifndef _IN_VIRTCH_

#ifndef NO_HQMIXER
extern int   VC1_Init(void);
extern int   VC2_Init(void);
static int  (*VC_Init_ptr)(void)=VC1_Init;
static void (*VC_Exit_ptr)(void)=VC1_Exit;
extern int   VC1_SetNumVoices(void);
extern int   VC2_SetNumVoices(void);
static int  (*VC_SetNumVoices_ptr)(void);
static ULONG (*VC_SampleSpace_ptr)(int);
static ULONG (*VC_SampleLength_ptr)(int,SAMPLE*);

extern int   VC2_PlayStart(void);
static int  (*VC_PlayStart_ptr)(void);
extern void  VC2_PlayStop(void);
static void (*VC_PlayStop_ptr)(void);

static SWORD (*VC_SampleLoad_ptr)(struct SAMPLOAD*,int);
static void (*VC_SampleUnload_ptr)(SWORD);

static ULONG (*VC_WriteBytes_ptr)(SBYTE*,ULONG);
static ULONG (*VC_SilenceBytes_ptr)(SBYTE*,ULONG);

static void (*VC_VoiceSetVolume_ptr)(UBYTE,UWORD);
static UWORD (*VC_VoiceGetVolume_ptr)(UBYTE);
static void (*VC_VoiceSetFrequency_ptr)(UBYTE,ULONG);
static ULONG (*VC_VoiceGetFrequency_ptr)(UBYTE);
static void (*VC_VoiceSetPanning_ptr)(UBYTE,ULONG);
static ULONG (*VC_VoiceGetPanning_ptr)(UBYTE);
static void (*VC_VoicePlay_ptr)(UBYTE,SWORD,ULONG,ULONG,ULONG,ULONG,UWORD);

static void (*VC_VoiceStop_ptr)(UBYTE);
static BOOL (*VC_VoiceStopped_ptr)(UBYTE);
static SLONG (*VC_VoiceGetPosition_ptr)(UBYTE);
static ULONG (*VC_VoiceRealVolume_ptr)(UBYTE);

#define VC_PROC0(suffix) \
MIKMODAPI void VC_##suffix (void) { VC_##suffix##_ptr(); }

#define VC_FUNC0(suffix,ret) \
MIKMODAPI ret VC_##suffix (void) { return VC_##suffix##_ptr(); }

#define VC_PROC1(suffix,typ1) \
MIKMODAPI void VC_##suffix (typ1 a) { VC_##suffix##_ptr(a); }

#define VC_FUNC1(suffix,ret,typ1) \
MIKMODAPI ret VC_##suffix (typ1 a) { return VC_##suffix##_ptr(a); }

#define VC_PROC2(suffix,typ1,typ2) \
MIKMODAPI void VC_##suffix (typ1 a,typ2 b) { VC_##suffix##_ptr(a,b); }

#define VC_FUNC2(suffix,ret,typ1,typ2) \
MIKMODAPI ret VC_##suffix (typ1 a,typ2 b) { return VC_##suffix##_ptr(a,b); }

VC_FUNC0(Init,int)
VC_PROC0(Exit)
VC_FUNC0(SetNumVoices,int)
VC_FUNC1(SampleSpace,ULONG,int)
VC_FUNC2(SampleLength,ULONG,int,SAMPLE*)
VC_FUNC0(PlayStart,int)
VC_PROC0(PlayStop)
VC_FUNC2(SampleLoad,SWORD,struct SAMPLOAD*,int)
VC_PROC1(SampleUnload,SWORD)
VC_FUNC2(WriteBytes,ULONG,SBYTE*,ULONG)
VC_FUNC2(SilenceBytes,ULONG,SBYTE*,ULONG)
VC_PROC2(VoiceSetVolume,UBYTE,UWORD)
VC_FUNC1(VoiceGetVolume,UWORD,UBYTE)
VC_PROC2(VoiceSetFrequency,UBYTE,ULONG)
VC_FUNC1(VoiceGetFrequency,ULONG,UBYTE)
VC_PROC2(VoiceSetPanning,UBYTE,ULONG)
VC_FUNC1(VoiceGetPanning,ULONG,UBYTE)

void VC_VoicePlay(UBYTE a,SWORD b,ULONG c,ULONG d,ULONG e,ULONG f,UWORD g) {
     VC_VoicePlay_ptr(a,b,c,d,e,f,g);
}

VC_PROC1(VoiceStop,UBYTE)
VC_FUNC1(VoiceStopped,BOOL,UBYTE)
VC_FUNC1(VoiceGetPosition,SLONG,UBYTE)
VC_FUNC1(VoiceRealVolume,ULONG,UBYTE)

void VC_SetupPointers(void)
{
	if (md_mode&DMODE_HQMIXER) {
		VC_Init_ptr=VC2_Init;
		VC_Exit_ptr=VC2_Exit;
		VC_SetNumVoices_ptr=VC2_SetNumVoices;
		VC_SampleSpace_ptr=VC2_SampleSpace;
		VC_SampleLength_ptr=VC2_SampleLength;
		VC_PlayStart_ptr=VC2_PlayStart;
		VC_PlayStop_ptr=VC2_PlayStop;
		VC_SampleLoad_ptr=VC2_SampleLoad;
		VC_SampleUnload_ptr=VC2_SampleUnload;
		VC_WriteBytes_ptr=VC2_WriteBytes;
		VC_SilenceBytes_ptr=VC2_SilenceBytes;
		VC_VoiceSetVolume_ptr=VC2_VoiceSetVolume;
		VC_VoiceGetVolume_ptr=VC2_VoiceGetVolume;
		VC_VoiceSetFrequency_ptr=VC2_VoiceSetFrequency;
		VC_VoiceGetFrequency_ptr=VC2_VoiceGetFrequency;
		VC_VoiceSetPanning_ptr=VC2_VoiceSetPanning;
		VC_VoiceGetPanning_ptr=VC2_VoiceGetPanning;
		VC_VoicePlay_ptr=VC2_VoicePlay;
		VC_VoiceStop_ptr=VC2_VoiceStop;
		VC_VoiceStopped_ptr=VC2_VoiceStopped;
		VC_VoiceGetPosition_ptr=VC2_VoiceGetPosition;
		VC_VoiceRealVolume_ptr=VC2_VoiceRealVolume;
	} else {
		VC_Init_ptr=VC1_Init;
		VC_Exit_ptr=VC1_Exit;
		VC_SetNumVoices_ptr=VC1_SetNumVoices;
		VC_SampleSpace_ptr=VC1_SampleSpace;
		VC_SampleLength_ptr=VC1_SampleLength;
		VC_PlayStart_ptr=VC1_PlayStart;
		VC_PlayStop_ptr=VC1_PlayStop;
		VC_SampleLoad_ptr=VC1_SampleLoad;
		VC_SampleUnload_ptr=VC1_SampleUnload;
		VC_WriteBytes_ptr=VC1_WriteBytes;
		VC_SilenceBytes_ptr=VC1_SilenceBytes;
		VC_VoiceSetVolume_ptr=VC1_VoiceSetVolume;
		VC_VoiceGetVolume_ptr=VC1_VoiceGetVolume;
		VC_VoiceSetFrequency_ptr=VC1_VoiceSetFrequency;
		VC_VoiceGetFrequency_ptr=VC1_VoiceGetFrequency;
		VC_VoiceSetPanning_ptr=VC1_VoiceSetPanning;
		VC_VoiceGetPanning_ptr=VC1_VoiceGetPanning;
		VC_VoicePlay_ptr=VC1_VoicePlay;
		VC_VoiceStop_ptr=VC1_VoiceStop;
		VC_VoiceStopped_ptr=VC1_VoiceStopped;
		VC_VoiceGetPosition_ptr=VC1_VoiceGetPosition;
		VC_VoiceRealVolume_ptr=VC1_VoiceRealVolume;
	}
}
#endif/* !NO_HQMIXER */

#else /* _IN_VIRTCH_ */

#ifndef _VIRTCH_COMMON_
#define _VIRTCH_COMMON_

/*==============================================================================
  Retrigger crossfade support.

  The mixer keeps a "departing" playback state next to each voice.  When a
  Voice_Play() retriggers a voice that is already producing PCM, the voice's
  next-sample cursor is snapshotted (integer/fractional phase, direction, loop
  bounds, step, effective gains) and the normal mixer machinery is run on that
  snapshot for the duration of the crossfade.  Both ends are mixed into
  separate scratch buffers and blended with per-output-frame weights before the
  down-sampler, the effects and the output callback run, so those stages only
  advance once.

  Sample buffers are held by generation: a loaded buffer gets a small reference
  record, the handle slot owns one reference, and each running departing state
  owns one.  Freeing the SAMPLE while a crossfade still needs the PCM retires
  the buffer from its handle instead of releasing it; the last reference frees
  it.  A later load may therefore reuse the same numeric handle without
  touching the retired generation.
==============================================================================*/

typedef struct VC_GEN {
	SWORD *data;
	int    refs;
} VC_GEN;

static VC_GEN **vc_gen=NULL;
static SLONG *vc_fade_bufA=NULL,*vc_fade_bufB=NULL;
static int    vc_fade_factor=1;
static ULONG  vc_fade_len=0;
static int    vfd_count=0;

/* factor used by the mixer that includes this file, for buffer sizing and
   for translating the configured output-frame length into internal samples. */
#define VC_FADE_FACTOR  vc_fade_factor

/* Provided by the mixer file that includes this one. */
static void VC1_FadeOnPlay(UBYTE voice);
static void VC1_FadeOnStop(UBYTE voice);

static void vc_fade_end(UBYTE voice);

static void vc_gen_release(VC_GEN *g)
{
	if(!g) return;
	if(--g->refs<=0) {
		MikMod_afree(g->data);
		MikMod_free(g);
	}
}

static void vc_gen_setup(void)
{
	if(!vc_fade_len || vc_gen) return;
	if(!(vc_gen=(VC_GEN**)MikMod_calloc(MAXSAMPLEHANDLES,sizeof(VC_GEN*))))
		vc_fade_len=0;
}

static void vc_gen_free_all(void)
{
	int i;

	if(!vc_gen) return;
	for(i=0;i<MAXSAMPLEHANDLES;i++) {
		if(vc_gen[i]) {
			MikMod_afree(vc_gen[i]->data);
			MikMod_free(vc_gen[i]);
			vc_gen[i]=NULL;
		}
	}
	MikMod_free(vc_gen);
	vc_gen=NULL;
}

static void vc_gen_create(SWORD handle,SWORD *data)
{
	VC_GEN *g;

	if(!vc_fade_len) return;
	vc_gen_setup();
	if(!vc_gen || handle<0 || handle>=MAXSAMPLEHANDLES) return;
	/* The slot may still reference a retired generation that a departing
	   voice is reading; leave that object (and the fade's reference) intact
	   and only make the new generation the slot's owner. */
	if(!(g=(VC_GEN*)MikMod_calloc(1,sizeof(VC_GEN)))) return;
	g->data=data;
	g->refs=1;
	vc_gen[handle]=g;
}

/* Take a reference on the buffer currently loaded in 'handle'.  Returns NULL
   when the crossfade machinery is inactive or no generation record exists. */
static VC_GEN *vc_gen_acquire(SWORD handle)
{
	vc_gen_setup();
	if(!vc_gen || handle<0 || handle>=MAXSAMPLEHANDLES) return NULL;
	if(!vc_gen[handle]) return NULL;
	vc_gen[handle]->refs++;
	return vc_gen[handle];
}

/* Called when the driver releases a handle. */
static void vc_gen_unload(SWORD handle)
{
	VC_GEN *g;

	if(!Samples || handle<0 || handle>=MAXSAMPLEHANDLES) return;
	if(vc_gen && (g=vc_gen[handle])!=NULL) {
		vc_gen[handle]=NULL;
		Samples[handle]=NULL;
		vc_gen_release(g);
		return;
	}
	MikMod_afree(Samples[handle]);
	Samples[handle]=NULL;
}

/* Drop the departing state of a voice (if any) and release its reference. */
static void vc_fade_end(UBYTE voice)
{
	VFADE *fd;

	if(!vfd || voice>=vfd_count) return;
	fd=&vfd[voice];
	fd->running=0;
	fd->pos=0;
	fd->data=NULL;
	fd->len=0;
	if(fd->token) {
		vc_gen_release((VC_GEN*)fd->token);
		fd->token=NULL;
	}
}

static void vc_fade_release_all(void)
{
	int t;

	if(!vfd) return;
	for(t=0;t<vfd_count;t++)
		vc_fade_end((UBYTE)t);
}

/* Start (or restart) the departing state for 'voice' from its current next
   sample position.  Only called when the voice is genuinely producing PCM. */
static void vc_fade_start(UBYTE voice)
{
	VINFO *m;
	VFADE *fd;
	VC_GEN *g;

	if(!vfd || !vc_fade_len || voice>=vfd_count) return;
	m=&vinf[voice];
	fd=&vfd[voice];

	/* A pending new target has not produced PCM yet: keep the departing
	   state captured by the previous Play() so consecutive Play() calls on
	   one output boundary collapse to {pre-batch main, latest target}. */
	if(m->kick) return;

	if(!m->active) {
		/* the old voice already ended naturally: plain attack */
		vc_fade_end(voice);
		return;
	}

	/* Pin the main voice's current buffer generation before retiring any
	   previous departing state, so that a re-entrant replacement on the same
	   handle keeps the buffer alive. */
	g=vc_gen_acquire(m->handle);
	/* Either way the previous departing state is superseded. */
	vc_fade_end(voice);
	if(!g) {
		/* cannot pin the buffer (dead handle): plain attack, no tail */
		return;
	}

	fd->dep=*m;
	fd->dep.active=1;
	fd->dep.kick=0;
	fd->dep.rampvol=0;
#ifdef VC_FADE_HAS_CLICK
	fd->dep.click=0;
	fd->dep.lastvalL=fd->dep.lastvalR=0;
#endif
	fd->token=g;
	fd->data=g->data;
	fd->handle=m->handle;
	fd->len=(int)vc_fade_len;
	fd->pos=0;
	fd->running=1;
}

/* Set up the shared index globals from the departing state. */
static void vc_fade_prepare_dep(VFADE *fd)
{
	VINFO *dep=&fd->dep;

	vnf=dep;
	idxsize=(dep->size)?((SLONGLONG)dep->size<<FRACBITS)-1:0;
	idxlend=(dep->repend)?((SLONGLONG)dep->repend<<FRACBITS)-1:0;
	idxlpos=(SLONGLONG)dep->reppos<<FRACBITS;
	vc_add_srce=fd->data;
}

/* Blend this voice's main and departing scratch buffers into 'dst'.
   'samples' is the number of internal sample frames (not counting channels);
   one output frame spans VC_FADE_FACTOR internal frames. */
static void vc_fade_combine(UBYTE voice,SLONG *dst,const SLONG *nbuf,const SLONG *obuf,int samples)
{
	VFADE *fd=&vfd[voice];
	int per_frame=VC_FADE_FACTOR*((vc_mode&DMODE_STEREO)?2:1);
	int frames=samples/VC_FADE_FACTOR;
	int f,s;

	for(f=0;f<frames;f++) {
		int base=f*per_frame;
		if(fd->running && fd->pos<fd->len) {
			SLONGLONG ow=(SLONGLONG)(fd->len-fd->pos);
			SLONGLONG nw=(SLONGLONG)fd->pos;
			for(s=0;s<per_frame;s++) {
				SLONGLONG v=(ow*(SLONGLONG)obuf[base+s] +
				            nw*(SLONGLONG)nbuf[base+s]) / fd->len;
				dst[base+s]+=(SLONG)v;
			}
			fd->pos++;
			if(fd->pos>=fd->len) vc_fade_end(voice);
		} else {
			for(s=0;s<per_frame;s++)
				dst[base+s]+=nbuf[base+s];
		}
	}
}

static void vc_fade_buffers_free(void)
{
	MikMod_afree(vc_fade_bufA);
	MikMod_afree(vc_fade_bufB);
	vc_fade_bufA=vc_fade_bufB=NULL;
}

static int vc_fade_buffers_alloc(void)
{
	if(!vc_fade_len) return 1;
	if(vc_fade_bufA) return 1;
	vc_fade_bufA=(SLONG*)MikMod_amalloc((TICKLSIZE+32)*sizeof(SLONG));
	vc_fade_bufB=(SLONG*)MikMod_amalloc((TICKLSIZE+32)*sizeof(SLONG));
	if(!vc_fade_bufA || !vc_fade_bufB) {
		vc_fade_buffers_free();
		vc_fade_len=0;
		return 1;
	}
	return 0;
}

static void vc_fade_shutdown(void)
{
	vc_fade_release_all();
	vc_fade_buffers_free();
	vc_gen_free_all();
	MikMod_free(vfd);
	vfd=NULL;
	vfd_count=0;
}

/* Snapshot the configured crossfade length for this mixer instance.  Only the
   C scalar software path is extended: no SIMD and interpolation required. */
static void vc_fade_configure(void)
{
	vc_fade_len=0;
	if(!md_retriggerfade) return;
#if defined HAVE_SSE2 || defined HAVE_ALTIVEC
	return;
#endif
	if((md_mode & DMODE_INTERP) /* HQ always sets it */)
		vc_fade_len=(md_retriggerfade>4096)?4096:md_retriggerfade;
}

/* (Re)allocate the per-voice departing state table. */
static void vc_fade_setup_voices(void)
{
	int t;

	vc_fade_release_all();
	MikMod_free(vfd);
	vfd=NULL;
	vfd_count=0;
	if(!vc_fade_len) return;
	if(!(vfd=(VFADE*)MikMod_calloc(vc_softchn?vc_softchn:1,sizeof(VFADE)))) {
		vc_fade_len=0;
		return;
	}
	vfd_count=vc_softchn;
	for(t=0;t<vc_softchn;t++)
		vfd[t].dep.active=0;
	vc_gen_setup();
	vc_fade_buffers_alloc();
}

static ULONG samples2bytes(ULONG samples)
{
	if(vc_mode & DMODE_FLOAT) samples <<= 2;
	else if(vc_mode & DMODE_16BITS) samples <<= 1;
	if(vc_mode & DMODE_STEREO) samples <<= 1;
	return samples;
}

static ULONG bytes2samples(ULONG bytes)
{
	if(vc_mode & DMODE_FLOAT) bytes >>= 2;
	else if(vc_mode & DMODE_16BITS) bytes >>= 1;
	if(vc_mode & DMODE_STEREO) bytes >>= 1;
	return bytes;
}

/* Fill the buffer with 'todo' bytes of silence (it depends on the mixing mode
   how the buffer is filled) */
ULONG VC1_SilenceBytes(SBYTE* buf,ULONG todo)
{
	todo=samples2bytes(bytes2samples(todo));

	/* clear the buffer to zero (16 bits signed) or 0x80 (8 bits unsigned) */
	if(vc_mode &(DMODE_16BITS|DMODE_FLOAT))
		memset(buf,0,todo);
	else
		memset(buf,0x80,todo);

	return todo;
}

void VC1_WriteSamples(SBYTE*,ULONG);

/* Writes 'todo' mixed SBYTES (!!) to 'buf'. It returns the number of SBYTES
   actually written to 'buf' (which is rounded to number of samples that fit
   into 'todo' bytes). */
ULONG VC1_WriteBytes(SBYTE* buf,ULONG todo)
{
	if(!vc_softchn)
		return VC1_SilenceBytes(buf,todo);

	todo = bytes2samples(todo);
	VC1_WriteSamples(buf,todo);

	return samples2bytes(todo);
}

void VC1_Exit(void)
{
	vc_fade_shutdown();
	MikMod_free(vinf);
	MikMod_afree(vc_tickbuf);
	MikMod_afree(Samples);

	vc_tickbuf = NULL;
	vinf = NULL;
	vfd = NULL;
	Samples = NULL;

	VC_SetupPointers();
}

UWORD VC1_VoiceGetVolume(UBYTE voice)
{
	return vinf[voice].vol;
}

ULONG VC1_VoiceGetPanning(UBYTE voice)
{
	return vinf[voice].pan;
}

void VC1_VoiceSetFrequency(UBYTE voice,ULONG frq)
{
	vinf[voice].frq=frq;
}

ULONG VC1_VoiceGetFrequency(UBYTE voice)
{
	return vinf[voice].frq;
}

void VC1_VoicePlay(UBYTE voice,SWORD handle,ULONG start,ULONG size,ULONG reppos,ULONG repend,UWORD flags)
{
	VC1_FadeOnPlay(voice);
	vinf[voice].flags  = flags;
	vinf[voice].handle = handle;
	vinf[voice].start  = start;
	vinf[voice].size   = size;
	vinf[voice].reppos = reppos;
	vinf[voice].repend = repend;
	vinf[voice].kick   = 1;
}

void VC1_VoiceStop(UBYTE voice)
{
	VC1_FadeOnStop(voice);
	vinf[voice].active = 0;
	/* With the crossfade active, a pending attack must also be cancelled so
	   the slot never inherits a tail or a stale kick on reuse.  When the
	   feature is off the historical behaviour (only 'active' cleared) is
	   preserved. */
	if(vfd) vinf[voice].kick = 0;
}

BOOL VC1_VoiceStopped(UBYTE voice)
{
	return(vinf[voice].active==0);
}

SLONG VC1_VoiceGetPosition(UBYTE voice)
{
	return (SLONG)(vinf[voice].current>>FRACBITS);
}

void VC1_VoiceSetVolume(UBYTE voice,UWORD vol)
{
	/* protect against clicks if volume variation is too high */
	if(abs((int)vinf[voice].vol-(int)vol)>32)
		vinf[voice].rampvol=CLICK_BUFFER;
	vinf[voice].vol=vol;
}

void VC1_VoiceSetPanning(UBYTE voice,ULONG pan)
{
	/* protect against clicks if panning variation is too high */
	if(abs((int)vinf[voice].pan-(int)pan)>48)
		vinf[voice].rampvol=CLICK_BUFFER;
	vinf[voice].pan=pan;
}

/*========== External mixer interface */

void VC1_SampleUnload(SWORD handle)
{
	/* With the crossfade active, retire the buffer generation instead of
	   freeing it; the last reference (the handle slot, or a departing voice
	   still using the PCM) frees the memory. */
	if(vc_gen) {
		vc_gen_unload(handle);
		return;
	}
	if (Samples && (handle < MAXSAMPLEHANDLES)) {
		MikMod_afree(Samples[handle]);
		Samples[handle]=NULL;
	}
}

SWORD VC1_SampleLoad(struct SAMPLOAD* sload,int type)
{
	SAMPLE *s = sload->sample;
	int handle;
	ULONG t, length,loopstart,loopend,looplen;

	if(type==MD_HARDWARE) return -1;

	if(s->length > MAX_SAMPLE_SIZE) {
		_mm_errno = MMERR_NOT_A_STREAM; /* better error? */
		return -1;
	}

	/* Find empty slot to put sample address in */
	for(handle=0;handle<MAXSAMPLEHANDLES;handle++)
		if(!Samples[handle]) break;

	if(handle==MAXSAMPLEHANDLES) {
		_mm_errno = MMERR_OUT_OF_HANDLES;
		return -1;
	}

	/* Reality check for loop settings */
	if (s->loopend > s->length)
		s->loopend = s->length;
	if (s->loopstart >= s->loopend)
		s->flags &= ~SF_LOOP;

	length    = s->length;
	loopstart = s->loopstart;
	loopend   = s->loopend;

	SL_SampleSigned(sload);
	SL_Sample8to16(sload);

	if(!(Samples[handle]=(SWORD*)MikMod_amalloc((length+20)<<1))) {
		_mm_errno = MMERR_SAMPLE_TOO_BIG;
		return -1;
	}

	/* read sample into buffer */
	if (SL_Load(Samples[handle],sload,length)) {
		MikMod_afree(Samples[handle]);
		Samples[handle]=NULL;
		return -1;
	}
	vc_gen_create(handle,Samples[handle]);

	/* Unclick sample */
	if(s->flags & SF_LOOP) {
		looplen = loopend - loopstart;/* handle short samples */
		if(s->flags & SF_BIDI)
			for(t=0;t<16 && t<looplen;t++)
				Samples[handle][loopend+t]=Samples[handle][(loopend-t)-1];
		else
			for(t=0;t<16 && t<looplen;t++)
				Samples[handle][loopend+t]=Samples[handle][t+loopstart];
	} else
		for(t=0;t<16;t++)
			Samples[handle][t+length]=0;

	return handle;
}

ULONG VC1_SampleSpace(int type)
{
	return vc_memory;
}

ULONG VC1_SampleLength(int type,SAMPLE* s)
{
	if (!s) return 0;

	return (s->length*((s->flags&SF_16BITS)?2:1))+16;
}

ULONG VC1_VoiceRealVolume(UBYTE voice)
{
	ULONG i,s,size;
	int k,j;
	SWORD *smp;
	SLONG t;

	t = (SLONG)(vinf[voice].current>>FRACBITS);
	if(!vinf[voice].active) return 0;

	s = vinf[voice].handle;
	size = vinf[voice].size;

	i=64; t-=64; k=0; j=0;
	if(i>size) i = size;
	if(t<0) t = 0;
	if(t+i > size) t = size-i;

	i &= ~1;  /* make sure it's EVEN. */

	smp = &Samples[s][t];
	for(;i;i--,smp++) {
		if(k<*smp) k = *smp;
		if(j>*smp) j = *smp;
	}
	return abs(k-j);
}

#endif /* _VIRTCH_COMMON_ */

#endif /* _IN_VIRTCH_ */

/* ex:set ts=4: */
