#ifndef LIBXMP_MIXER_H
#define LIBXMP_MIXER_H

#include "common.h"
#include "mix_sinc.h"

#define C4_PERIOD	428.0

#define SMIX_NUMVOC	128	/* default number of softmixer voices */
#define SMIX_SHIFT	16
#define SMIX_MASK	0xffff

#define FILTER_SHIFT	22
#define ANTICLICK_SHIFT	3

/* fidx / mixer-table index bits, see mix_all.h for the full index map. */
#define FLAG_16_BITS	0x01
#define FLAG_STEREO	0x02
#define FLAG_STEREOOUT	0x04
#define FLAG_FILTER	0x08
#define FLAG_ACTIVE	0x10
/* #define FLAG_SYNTH	0x20 */
#define FIDX_FLAGMASK	(FLAG_16_BITS | FLAG_STEREO | FLAG_STEREOOUT | FLAG_FILTER)

#ifdef LIBXMP_PAULA_SIMULATOR
#include "paula.h"
#endif

struct mixer_voice {
	int chn;		/* channel number */
	int root;		/* */
	int note;		/* */
#define PAN_SURROUND 0x8000
	int pan;		/* */
	int vol;		/* */
	double period;		/* current period */
	double pos;		/* position in sample */
	int pos0;		/* position in sample before mixing */
	int fidx;		/* mixer function index */
	int ins;		/* instrument number */
	int smp;		/* sample number */
	int start;		/* loop start */
	int end;		/* loop end */
	int act;		/* nna info & status of voice */
	int key;		/* key for DCA note check */
	int old_vl;		/* previous volume, left channel */
	int old_vr;		/* previous volume, right channel */
	int sleft;		/* last left sample output, in 32bit */
	int sright;		/* last right sample output, in 32bit */
#define VOICE_RELEASE	(1 << 0)
#define ANTICLICK	(1 << 1)
#define SAMPLE_LOOP	(1 << 2)
#define VOICE_REVERSE	(1 << 3)
#define VOICE_BIDIR	(1 << 4)
#define SAMPLE_QUEUED	(1 << 5)
#define SAMPLE_PAUSED	(1 << 6)
	int flags;		/* flags */
	void *sptr;		/* sample pointer */
#ifdef LIBXMP_PAULA_SIMULATOR
	struct paula_state *paula; /* paula simulation state */
#endif

	struct {		/* Protracker queued instrument change */
		int smp;
	} queued;

#ifndef LIBXMP_CORE_DISABLE_IT
	struct {
		int r1;		/* filter variables */
		int r2;
		int l1;
		int l2;
		int a0;
		int b0;
		int b1;
		int cutoff;
		int resonance;
	} filter;
#endif

	/* Persistent state for XMP_INTERP_SINC.  Embedded by value so the
	 * voice array allocation is the only workspace needed. */
	struct sinc_voice_state sinc;
};

LIBXMP_BEGIN_DECLS

int	libxmp_mixer_on		(struct context_data *, int, int, int);
void	libxmp_mixer_off	(struct context_data *);
void    libxmp_mixer_setvol	(struct context_data *, int, int);
void    libxmp_mixer_seteffect	(struct context_data *, int, int, int);
void    libxmp_mixer_setpan	(struct context_data *, int, int);
int	libxmp_mixer_numvoices	(struct context_data *, int);
void	libxmp_mixer_softmixer	(struct context_data *);
void	libxmp_mixer_reset	(struct context_data *);
void	libxmp_mixer_setpatch	(struct context_data *, int, int, int);
void	libxmp_mixer_queuepatch	(struct context_data *, int, int);
void	libxmp_mixer_voicepos	(struct context_data *, int, double, int);
double	libxmp_mixer_getvoicepos(struct context_data *, int);
void	libxmp_mixer_setnote	(struct context_data *, int, int);
void	libxmp_mixer_setperiod	(struct context_data *, int, double);
void	libxmp_mixer_release	(struct context_data *, int, int);
void	libxmp_mixer_reverse	(struct context_data *, int, int);
int	libxmp_mixer_get_ticksize(int freq, double time_factor, double rrate, int bpm);

LIBXMP_END_DECLS

#endif /* LIBXMP_MIXER_H */
