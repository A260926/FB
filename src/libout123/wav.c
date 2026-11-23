/*
	wav.c: write wav/au/cdr files (and headerless raw

	copyright ?-2023 by the mpg123 project - free software under the terms of the LGPL 2.1
	see COPYING and AUTHORS files in distribution or http://mpg123.org
	initially written by Samuel Audet

	Geez, why are WAV RIFF headers are so secret?  I got something together,
	but wow...  anyway, I hope someone will find this useful.
	- Samuel Audet

	minor simplifications and ugly AU/CDR format stuff by MH

	It's not a very clean code ... Fix this!

	ThOr: The usage of stdio streams means we loose control over what data is actually written. On a full disk, fwrite() happily suceeds for ages, only a fflush fails.
	Now: Do we want to fflush() after every write? That defeats the purpose of buffered I/O. So, switching to good old write() is an option (kernel doing disk buffering anyway).

	ThOr: Again reworked things for libout123, with non-static state.
	This set of builtin "modules" is what we can use in automated tests
	of libout123. Code still not very nice, but I tried to keep modification
	of what stood the test of time minimal. One still can add a module to
	libout123 that uses sndfile and similar libraries for more choice on writing
	output files.
*/

#include "out123_int.h"
#include "wav.h"

#include <errno.h>
#include "../common/debug.h"

/* Create the two WAV headers. */

#define WAVE_FORMAT 1
#define RIFF_NAME riff_template
#define RIFF_STRUCT_NAME riff
#include "wavhead.h"

#undef WAVE_FORMAT
#undef RIFF_NAME
#undef RIFF_STRUCT_NAME
#define WAVE_FORMAT 3
#define RIFF_NAME riff_float_template
#define RIFF_STRUCT_NAME riff_float
#define FLOATOUT
#include "wavhead.h"

/* AU header struct... */

struct auhead {
  byte magic[4];
  byte headlen[4];
  byte datalen[4];
  byte encoding[4];
  byte rate[4];
  byte channels[4];
  byte dummy[8];
} const auhead_template = { 
  { 0x2e,0x73,0x6e,0x64 } , { 0x00,0x00,0x00,0x20 } , 
  { 0xff,0xff,0xff,0xff } , { 0,0,0,0 } , { 0,0,0,0 } , { 0,0,0,0 } , 
  { 0,0,0,0,0,0,0,0 }};

struct wavdata
{
	FILE *wavfp;
	long datalen;
	int flipendian;
	int bytes_per_sample;
	int floatwav; /* If we write a floating point WAV file. */
	/* 
		Open routines only prepare a header, stored here and written on first
		actual data write. If no data is written at all, proper files will
		still get a header via the update at closing; non-seekable streams will
		just have no no header if there is no data.
	*/
	void *the_header;
	size_t the_header_size;
	int seekable; /* file position can be rewound to fix the header */
	int framesize; /* bytes per PCM frame, for frame offsets */
	size_t riffextra; /* bytes appended after the data chunk (pad + cues) */
};

static struct wavdata* wavdata_new(void)
{
	struct wavdata *wdat = malloc(sizeof(struct wavdata));
	if(wdat)
	{
		wdat->wavfp = NULL;
		wdat->datalen = 0;
		wdat->flipendian = 0;
		wdat->bytes_per_sample = -1;
		wdat->floatwav = 0;
		wdat->the_header = NULL;
		wdat->the_header_size = 0;
		wdat->seekable = 0;
		wdat->framesize = 0;
		wdat->riffextra = 0;
	}
	return wdat;
}

static void wavdata_del(struct wavdata *wdat)
{
	if(!wdat) return;
	if(wdat->wavfp && wdat->wavfp != stdout)
		INT123_compat_fclose(wdat->wavfp);
	if(wdat->the_header)
		free(wdat->the_header);
	free(wdat);
}

/* Pointer types are for pussies;-) */
static void* wavhead_new(void const *template, size_t size)
{
	void *header = malloc(size);
	if(header)
		memcpy(header, template, size);
	return header;
}

/* Convertfunctions: */
/* always little endian */

static void long2littleendian(long inval,byte *outval,int b)
{
  int i;
  for(i=0;i<b;i++) {
    outval[i] = (inval>>(i*8)) & 0xff;
  } 
}

/* always big endian */
static void long2bigendian(long inval,byte *outval,int b)
{
  int i;
  for(i=0;i<b;i++) {
    outval[i] = (inval>>((b-i-1)*8)) & 0xff;
  }
}

static long from_little(byte *inval, int b)
{
	long ret = 0;
	int i;
	for(i=0;i<b;++i) ret += ((long)inval[i])<<(i*8);

	return ret;
}

/* Unsigned 32 bit little endian helpers for the chunk lengths and frame
   offsets, which must not depend on the width of long. */
static void u32le(unsigned long inval, byte *outval)
{
	int i;
	for(i=0; i<4; ++i)
		outval[i] = (byte)((inval>>(i*8)) & 0xff);
}

static int testEndian(void) 
{
  long a=0,b=0,c=0;
  int ret = 0;
  size_t i;

  for(i=0;i<sizeof(long);i++) {
    ((byte *)&a)[i] = (byte)i;
    b<<=8;
    b |= i;
    c |= i << (i*8);
  }
  if(a == b)
      ret = 1;
  else if(a != c) {
      ret = -1;
  }
  return ret;
}

/* return: 0 is good, -1 is bad */
static int open_file(struct wavdata *wdat, char *filename)
{
	debug2("open_file(%p, %s)", (void*)wdat, filename ? filename : "<nil>");
	if(!wdat)
		return -1;
	if(!filename || !strcmp("-",filename) || !strcmp("", filename))
	{
		wdat->wavfp = stdout;
		INT123_compat_binmode(STDOUT_FILENO, TRUE);
		/* If stdout is redirected to a file, seeks suddenly can work.
		Doing one here to ensure that such a file has the same output
		it had when opening directly as such. */
		fseek(wdat->wavfp, 0L, SEEK_SET);
		return 0;
	}
	else
	{
		wdat->wavfp = INT123_compat_fopen(filename, "wb");
		if(!wdat->wavfp)
			return -1;
		else
			return 0;
	}
}

/* return: 0 is good, -1 is bad
   Works for any partial state of setup, especially should not complain if
   ao->userptr == NULL. */
static int close_file(out123_handle *ao)
{
	struct wavdata *wdat = ao->userptr;
	int ret = 0;

	if(wdat->wavfp != NULL && wdat->wavfp != stdout)
	{
		if(INT123_compat_fclose(wdat->wavfp))
		{
			if(!AOQUIET)
				error1("problem closing the audio file, probably because of flushing to disk: %s\n", INT123_strerror(errno));
			ret = -1;
		}
	}

	/* Always cleanup here. */
	wdat->wavfp = NULL;
	wavdata_del(wdat);
	ao->userptr = NULL;
	return ret;
}

/* return: 0 is good, -1 is bad */
static int write_header(out123_handle *ao)
{
	struct wavdata *wdat = ao->userptr;

	if(!wdat)
		return 0;

	if(
		wdat->the_header_size > 0
	&&	(
			fwrite(wdat->the_header, wdat->the_header_size, 1, wdat->wavfp) != 1
		|| fflush(wdat->wavfp)
		)
	)
	{
		if(!AOQUIET)
			error1("cannot write header: %s", INT123_strerror(errno));
		return -1;
	}
	else return 0;
}

int INT123_au_open(out123_handle *ao)
{
	struct wavdata *wdat   = NULL;
	struct auhead  *auhead = NULL;

	if(ao->format < 0)
	{
		ao->rate = 44100;
		ao->channels = 2;
		ao->format = MPG123_ENC_SIGNED_16;
		return 0;
	}

	if(ao->format & MPG123_ENC_FLOAT)
	{
		if(!AOQUIET)
			error("AU file support for float values not there yet");
		goto au_open_bad;
	}

	if(
		!(wdat   = wavdata_new())
	||	!(auhead = wavhead_new(&auhead_template, sizeof(auhead_template)))
	)
	{
		ao->errcode = OUT123_DOOM;
		goto au_open_bad;
	}

	wdat->the_header      = auhead;
	wdat->the_header_size = sizeof(*auhead);

	wdat->flipendian = 0;

	switch(ao->format)
	{
		case MPG123_ENC_SIGNED_16:
		{
			int endiantest = testEndian();
			if(endiantest == -1)
				goto au_open_bad;
			wdat->flipendian = !endiantest; /* big end */
			long2bigendian(3,auhead->encoding,sizeof(auhead->encoding));
		}
		break;
		case MPG123_ENC_UNSIGNED_8:
			ao->format = MPG123_ENC_ULAW_8;
		case MPG123_ENC_ULAW_8:
			long2bigendian(1,auhead->encoding,sizeof(auhead->encoding));
		break;
		default:
			if(!AOQUIET)
				error("AU output is only a hack. This audio mode isn't supported yet.");
			goto au_open_bad;
	}

	long2bigendian(0xffffffff,auhead->datalen,sizeof(auhead->datalen));
	long2bigendian(ao->rate,auhead->rate,sizeof(auhead->rate));
	long2bigendian(ao->channels,auhead->channels,sizeof(auhead->channels));

	if(open_file(wdat, ao->device) < 0)
		goto au_open_bad;

	wdat->datalen = 0;

	ao->userptr = wdat;
	return 0;

au_open_bad:
	if(auhead)
		free(auhead);
	if(wdat)
	{
		wdat->the_header = NULL;
		wavdata_del(wdat);
	}
	return -1;
}

int INT123_cdr_open(out123_handle *ao)
{
	struct wavdata *wdat   = NULL;

	if(ao->format < 0)
	{
		ao->rate = 44100;
		ao->channels = 2;
		ao->format = MPG123_ENC_SIGNED_16;
		return 0;
	}

	if(
		ao->format != MPG123_ENC_SIGNED_16
	||	ao->rate != 44100
	||	ao->channels != 2
	)
	{
		if(!AOQUIET)
			error("Oops .. not forced to 16 bit, 44 kHz, stereo?");
		goto cdr_open_bad;
	}

	if(!(wdat = wavdata_new()))
	{
		ao->errcode = OUT123_DOOM;
		goto cdr_open_bad;
	}

	wdat->flipendian = !testEndian(); /* big end */

	if(open_file(wdat, ao->device) < 0)
	{
		if(!AOQUIET)
			error("cannot open file for writing");
		goto cdr_open_bad;
	}

	ao->userptr = wdat;
	return 0;
cdr_open_bad:
	if(wdat)
		wavdata_del(wdat);
	return -1;
}

/* RAW files are headerless WAVs where the format does not matter. */
int INT123_raw_open(out123_handle *ao)
{
	struct wavdata *wdat;

	if(ao->format < 0)
	{
		ao->rate = 44100;
		ao->channels = 2;
		ao->format = MPG123_ENC_SIGNED_16;
		return 0;
	}

	if(!(wdat = wavdata_new()))
	{
		ao->errcode = OUT123_DOOM;
		goto raw_open_bad;
	}

	if(open_file(wdat, ao->device) < 0)
		goto raw_open_bad;

	ao->userptr = wdat;
	return 1;
raw_open_bad:
	if(wdat)
		wavdata_del(wdat);
	return -1;
}

int INT123_wav_open(out123_handle *ao)
{
	int bps;
	struct wavdata    *wdat      = NULL;
	struct riff       *inthead   = NULL;
	struct riff_float *floathead = NULL;

	if(ao->format < 0)
	{
		ao->rate = 44100;
		ao->channels = 2;
		ao->format = MPG123_ENC_SIGNED_16;
		return 0;
	}

	if(!(wdat = wavdata_new()))
	{
		ao->errcode = OUT123_DOOM;
		goto wav_open_bad;
	}

	wdat->floatwav = (ao->format & MPG123_ENC_FLOAT);
	if(wdat->floatwav)
	{
		if(!(floathead = wavhead_new( &riff_float_template
		,	sizeof(riff_float_template)) ))
		{
			ao->errcode = OUT123_DOOM;
			goto wav_open_bad;
		}
		wdat->the_header = floathead;
		wdat->the_header_size = sizeof(*floathead);
	}
	else
	{
		if(!(inthead = wavhead_new( &riff_template
		,	sizeof(riff_template)) ))
		{
			ao->errcode = OUT123_DOOM;
			goto wav_open_bad;
		}
		wdat->the_header = inthead;
		wdat->the_header_size = sizeof(*inthead);

		/* standard MS PCM, and its format specific is BitsPerSample */
		long2littleendian(1, inthead->WAVE.fmt.FormatTag
		,	sizeof(inthead->WAVE.fmt.FormatTag));
	}

	if(ao->format == MPG123_ENC_FLOAT_32)
	{
		long2littleendian(3, floathead->WAVE.fmt.FormatTag
		,	sizeof(floathead->WAVE.fmt.FormatTag));
		long2littleendian(bps=32, floathead->WAVE.fmt.BitsPerSample
		,	sizeof(floathead->WAVE.fmt.BitsPerSample));
		wdat->flipendian = testEndian();
	}
	else if(ao->format == MPG123_ENC_SIGNED_32)
	{
		long2littleendian(bps=32, inthead->WAVE.fmt.BitsPerSample
		,	sizeof(inthead->WAVE.fmt.BitsPerSample));
		wdat->flipendian = testEndian();
	}
	else if(ao->format == MPG123_ENC_SIGNED_24)
	{
		long2littleendian(bps=24, inthead->WAVE.fmt.BitsPerSample
		,	sizeof(inthead->WAVE.fmt.BitsPerSample));
		wdat->flipendian = testEndian();
	}
	else if(ao->format == MPG123_ENC_SIGNED_16)
	{
		long2littleendian(bps=16, inthead->WAVE.fmt.BitsPerSample
		,	sizeof(inthead->WAVE.fmt.BitsPerSample));
		wdat->flipendian = testEndian();
	}
	else if(ao->format == MPG123_ENC_UNSIGNED_8)
		long2littleendian(bps=8, inthead->WAVE.fmt.BitsPerSample
		,	sizeof(inthead->WAVE.fmt.BitsPerSample));
	else
	{
		if(!AOQUIET)
			error("Format not supported.");
		goto wav_open_bad;
	}

	if(wdat->floatwav)
	{
		long2littleendian(ao->channels, floathead->WAVE.fmt.Channels
		,	sizeof(floathead->WAVE.fmt.Channels));
		long2littleendian(ao->rate, floathead->WAVE.fmt.SamplesPerSec
		,	sizeof(floathead->WAVE.fmt.SamplesPerSec));
		long2littleendian( (int)(ao->channels * ao->rate * bps)>>3
		,	floathead->WAVE.fmt.AvgBytesPerSec
		,	sizeof(floathead->WAVE.fmt.AvgBytesPerSec) );
		long2littleendian( (int)(ao->channels * bps)>>3
		,	floathead->WAVE.fmt.BlockAlign
		,	sizeof(floathead->WAVE.fmt.BlockAlign) );
		// Sanity check of stored values, could be truncated/overflown.
		if(from_little(floathead->WAVE.fmt.Channels,sizeof(floathead->WAVE.fmt.Channels)) < 1
		||	from_little(floathead->WAVE.fmt.BitsPerSample,sizeof(floathead->WAVE.fmt.BitsPerSample))/8 < 1)
			goto wav_open_bad;
	}
	else
	{
		long2littleendian(ao->channels, inthead->WAVE.fmt.Channels
		,	sizeof(inthead->WAVE.fmt.Channels));
		long2littleendian(ao->rate, inthead->WAVE.fmt.SamplesPerSec
		,	sizeof(inthead->WAVE.fmt.SamplesPerSec));
		long2littleendian( (int)(ao->channels * ao->rate * bps)>>3
		,	inthead->WAVE.fmt.AvgBytesPerSec
		,sizeof(inthead->WAVE.fmt.AvgBytesPerSec) );
		long2littleendian( (int)(ao->channels * bps)>>3
		,	inthead->WAVE.fmt.BlockAlign
		,	sizeof(inthead->WAVE.fmt.BlockAlign) );
		// Sanity check of stored values, could be truncated/overflown.
		if(from_little(inthead->WAVE.fmt.Channels,sizeof(inthead->WAVE.fmt.Channels)) < 1
		||	from_little(inthead->WAVE.fmt.BitsPerSample,sizeof(inthead->WAVE.fmt.BitsPerSample))/8 < 1)
			goto wav_open_bad;
	}

	if(open_file(wdat, ao->device) < 0)
		goto wav_open_bad;

	/* Chapter marks need to rewind and rewrite the header at closing, which
	   only works on a seekable file. A regular file always qualifies; a pipe
	   or a device does not. */
	wdat->seekable = fseek(wdat->wavfp, 0L, SEEK_CUR) == 0;
	ao->cue_support = wdat->seekable;

	if(wdat->floatwav)
	{
		long2littleendian(wdat->datalen, floathead->WAVE.data.datalen
		,	sizeof(floathead->WAVE.data.datalen));
		long2littleendian(wdat->datalen+sizeof(floathead->WAVE)
		,	floathead->WAVElen, sizeof(floathead->WAVElen));
	}
	else
	{
		long2littleendian(wdat->datalen, inthead->WAVE.data.datalen
		,	sizeof(inthead->WAVE.data.datalen));
		long2littleendian( wdat->datalen+sizeof(inthead->WAVE)
		,	inthead->WAVElen
		,	sizeof(inthead->WAVElen) );
	}

	wdat->bytes_per_sample = bps>>3;
	wdat->framesize = ao->channels * wdat->bytes_per_sample;

	ao->userptr = wdat;
	return 0;

wav_open_bad:
	if(inthead)
		free(inthead);
	if(floathead)
		free(floathead);
	if(wdat)
	{
		wdat->the_header = NULL;
		wavdata_del(wdat);
	}
	return -1;
}

int INT123_wav_write(out123_handle *ao, unsigned char *buf, int len)
{
	struct wavdata *wdat = ao->userptr;
	int temp;
	int i;

	if(!wdat || !wdat->wavfp)
		return 0; /* Really? Zero? */

	if(wdat->datalen == 0 && write_header(ao) < 0)
		return -1;
// TODO: offer way to hand in big-endian data already, as libmpg123
// can enforce endianess now
	/* Endianess conversion. Not fancy / optimized. */
	if(wdat->flipendian)
	{
		if(wdat->bytes_per_sample == 4) /* 32 bit */
		{
			if(len & 3)
			{
				if(!AOQUIET)
					error("Number of bytes no multiple of 4 (32bit)!");
				return -1;
			}
			for(i=0;i<len;i+=4)
			{
				int j;
				unsigned char tmp[4];
				for(j = 0; j<=3; ++j) tmp[j] = buf[i+j];
				for(j = 0; j<=3; ++j) buf[i+j] = tmp[3-j];
			}
		}
		else /* 16 bit */
		{
			if(len & 1)
			{
				error("Odd number of bytes!");
				return -1;
			}
			for(i=0;i<len;i+=2)
			{
				unsigned char tmp;
				tmp = buf[i+0];
				buf[i+0] = buf[i+1];
				buf[i+1] = tmp;
			}
		}
	}

	temp = fwrite(buf, 1, len, wdat->wavfp);
	if(temp <= 0) return temp;
/* That would kill it of early when running out of disk space. */
#if 0
if(fflush(wdat->wavfp))
{
	if(!AOQUIET)
		error1("flushing failed: %s\n", INT123_strerror(errno));
	return -1;
}
#endif
	wdat->datalen += temp;

	return temp;
}

/* Write a single RIFF chunk header with the given four-character ID and
   payload length. The length does not include the pad byte. */
static int write_chunk_header(FILE *fp, const char *id, size_t len)
{
	byte head[8];
	memcpy(head, id, 4);
	u32le((unsigned long)len, head+4);
	return fwrite(head, sizeof(head), 1, fp) == 1 ? 0 : -1;
}

/* Write zero padding if the given length is odd. */
static int write_pad(FILE *fp, size_t len)
{
	byte zero = 0;
	if(len & 1)
		return fwrite(&zero, 1, 1, fp) == 1 ? 0 : -1;
	return 0;
}

/* Append the cue and LIST/adtl chunks describing the registered marks.
   The file position is expected to be at the end of the audio data (that is,
   including a pad byte after an odd-sized data chunk). Returns 0 on success
   and stores the number of appended bytes in *extra. */
int INT123_wav_write_cues(out123_handle *ao, const struct out123_cue *cues
,	size_t cue_count, size_t *extra)
{
	struct wavdata *wdat = ao->userptr;
	FILE *fp;
	size_t c;
	size_t cue_body, list_body, list_payload;
	size_t total = 0;
	byte *buf;

	if(!wdat || !wdat->wavfp)
		return -1;
	if(!cue_count)
	{
		if(extra)
			*extra = 0;
		return 0;
	}
	/* Every cue entry is 24 bytes: ID, position, data chunk id, chunk
	   start, block start, sample offset. The body is preceded by the
	   entry count. */
	cue_body = 4 + cue_count*24;
	/* LIST payload: "adtl" plus one labl subchunk per non-empty label,
	   each with the name including the terminating NUL, plus its pad. */
	list_payload = 4;
	for(c=0; c<cue_count; ++c)
	{
		size_t labellen;
		size_t lablpayload;
		if(!cues[c].label)
			continue;
		labellen = strlen(cues[c].label) + 1;
		/* The labl payload is the 32 bit cue ID plus the name with NUL. */
		lablpayload = 4 + labellen;
		list_payload += 8 + lablpayload + (lablpayload & 1);
	}
	list_body = list_payload;
	/* The cue chunk itself, including its pad byte if odd. */
	total = 8 + cue_body + (cue_body & 1);
	/* The LIST chunk (its payload always includes "adtl", so nonzero). */
	total += 8 + list_body + (list_body & 1);

	if(!(buf = malloc(cue_body)))
	{
		ao->errcode = OUT123_DOOM;
		return -1;
	}
	fp = wdat->wavfp;
	u32le((unsigned long)cue_count, buf);
	byte *p = buf + 4;
	for(c=0; c<cue_count; ++c)
	{
		/* Chapter IDs are passed as unsigned long, but the file field is
		   32 bit. out123_wav_cue() already refuses IDs above 32 bit. */
		u32le(cues[c].id & 0xffffffffUL, p);
		u32le((unsigned long)cues[c].pos & 0xffffffffUL, p+4);
		memcpy(p+8, "data", 4);
		u32le(0, p+12); /* chunk start */
		u32le(0, p+16); /* block start */
		u32le((unsigned long)cues[c].pos & 0xffffffffUL, p+20);
		p += 24;
	}
	if(  write_chunk_header(fp, "cue ", cue_body)
	  || fwrite(buf, cue_body, 1, fp) != 1
	  || write_pad(fp, cue_body) )
	{
		free(buf);
		ao->errcode = OUT123_DEV_PLAY;
		return -1;
	}
	free(buf);

	if(write_chunk_header(fp, "LIST", list_body) || fwrite("adtl", 4, 1, fp) != 1)
	{
		ao->errcode = OUT123_DEV_PLAY;
		return -1;
	}
	for(c=0; c<cue_count; ++c)
	{
		size_t labellen;
		size_t lablpayload;
		byte idbuf[4];
		if(!cues[c].label)
			continue;
		labellen = strlen(cues[c].label) + 1;
		lablpayload = 4 + labellen;
		/* labl subchunk: 4 byte ID, 4 byte length, then the 32 bit cue ID
		   followed by the name including its terminating NUL. */
		u32le(cues[c].id & 0xffffffffUL, idbuf);
		if(  write_chunk_header(fp, "labl", lablpayload)
		  || fwrite(idbuf, 4, 1, fp) != 1
		  || fwrite(cues[c].label, labellen, 1, fp) != 1
		  || write_pad(fp, lablpayload) )
		{
			ao->errcode = OUT123_DEV_PLAY;
			return -1;
		}
	}
	if(write_pad(fp, list_body))
	{
		ao->errcode = OUT123_DEV_PLAY;
		return -1;
	}
	if(extra)
		*extra = total;
	return 0;
}

int INT123_wav_close(out123_handle *ao)
{
	struct wavdata *wdat = ao->userptr;

	if(!wdat) /* Special case: Opened only for format query. */
		return 0;

	if(!wdat || !wdat->wavfp)
		return -1;

	/* flush before seeking to catch out-of-disk explicitly at least at the end */
	if(fflush(wdat->wavfp))
	{
		if(!AOQUIET)
			error1("cannot flush WAV stream: %s", INT123_strerror(errno));
		return close_file(ao);
	}
	/* Append the chapter chunks after the audio data, if any. The data chunk
	   length does not include its own pad byte; pad it first so the chunks
	   that follow are aligned. riffextra holds everything appended after the
	   raw PCM (including that pad byte and the cue chunks).
	   Only marks whose boundary was actually reached by written PCM are
	   effective; anything beyond the real file tail is silently skipped. */
	if(ao->cue_count)
	{
		size_t cue_extra = 0;
		size_t data_pad = (wdat->datalen & 1) ? 1 : 0;
		size_t written_frames = wdat->framesize
		?	(size_t)wdat->datalen / (size_t)wdat->framesize : 0;
		size_t c, effective = 0;
		struct out123_cue *filtered = malloc(ao->cue_count*sizeof(*filtered));
		if(!filtered)
		{
			ao->errcode = OUT123_DOOM;
			return close_file(ao);
		}
		for(c=0; c<ao->cue_count; ++c)
		{
			if(ao->cues[c].pos <= written_frames)
				filtered[effective++] = ao->cues[c];
		}
		if(effective)
		{
			if(  fseek(wdat->wavfp, 0L, SEEK_END) == 0
			  && write_pad(wdat->wavfp, (size_t)wdat->datalen) == 0
			  && INT123_wav_write_cues(ao, filtered, effective
			     , &cue_extra) == 0 )
				wdat->riffextra = data_pad + cue_extra;
			else
			{
				free(filtered);
				if(!AOQUIET)
					error("cannot append chapter marks to WAV stream");
				return close_file(ao);
			}
		}
		free(filtered);
	}
	if(fseek(wdat->wavfp, 0L, SEEK_SET) >= 0)
	{
		if(wdat->floatwav)
		{
			struct riff_float *floathead = wdat->the_header;
			long2littleendian(wdat->datalen
			,	floathead->WAVE.data.datalen
			,	sizeof(floathead->WAVE.data.datalen));
			/* The RIFF length covers all appendage after the data chunk. */
			long2littleendian( wdat->datalen+sizeof(floathead->WAVE)
			+	(long)wdat->riffextra
			,	floathead->WAVElen
			,	sizeof(floathead->WAVElen));
			long2littleendian( wdat->datalen
			/	(
					from_little(floathead->WAVE.fmt.Channels,2)
				*	from_little(floathead->WAVE.fmt.BitsPerSample,2)/8
				)
			,	floathead->WAVE.fact.samplelen
			,	sizeof(floathead->WAVE.fact.samplelen) );
		}
		else
		{
			struct riff *inthead = wdat->the_header;
			long2littleendian(wdat->datalen, inthead->WAVE.data.datalen
			,	sizeof(inthead->WAVE.data.datalen));
			long2littleendian( wdat->datalen+sizeof(inthead->WAVE)
			+	(long)wdat->riffextra
			,	inthead->WAVElen
			,	sizeof(inthead->WAVElen));
		}
		/* Always (over)writing the header here; also for stdout, when
		   fseek worked, this overwrite works. */
		write_header(ao);
	}
	else if(!AOQUIET)
		warning("Cannot rewind WAV file. File-format isn't fully conform now.");

	return close_file(ao);
}

int INT123_au_close(out123_handle *ao)
{
	struct wavdata *wdat = ao->userptr;

	if(!wdat) /* Special case: Opened only for format query. */
		return 0;

	if(!wdat->wavfp)
		return -1;

	/* flush before seeking to catch out-of-disk explicitly at least at the end */
	if(fflush(wdat->wavfp))
	{
		if(!AOQUIET)
			error1("cannot flush WAV stream: %s", INT123_strerror(errno));
		return close_file(ao);
	}
	if(fseek(wdat->wavfp, 0L, SEEK_SET) >= 0)
	{
		struct auhead *auhead = wdat->the_header;
		long2bigendian(wdat->datalen, auhead->datalen, sizeof(auhead->datalen));
		/* Always (over)writing the header here; also for stdout, when
		   fseek worked, this overwrite works. */
		write_header(ao);
	}
	else if(!AOQUIET)
		warning("Cannot rewind AU file. File-format isn't fully conform now.");

	return close_file(ao);
}

/* CDR data also uses that. */
int INT123_raw_close(out123_handle *ao)
{
	struct wavdata *wdat = ao->userptr;

	if(!wdat) /* Special case: Opened only for format query. */
		return 0;

	if(!wdat->wavfp)
		return -1;

	return close_file(ao);
}

/* Some trivial functions to interface with out123's module architecture. */

int INT123_cdr_formats(out123_handle *ao)
{
	if(ao->rate == 44100 && ao->channels == 2)
		return MPG123_ENC_SIGNED_16;
	else
		return 0;
}

int INT123_au_formats(out123_handle *ao)
{
	return MPG123_ENC_SIGNED_16|MPG123_ENC_UNSIGNED_8|MPG123_ENC_ULAW_8;
}

int INT123_raw_formats(out123_handle *ao)
{
	return MPG123_ENC_ANY;
}

int INT123_wav_formats(out123_handle *ao)
{
	return
		MPG123_ENC_SIGNED_16
	|	MPG123_ENC_UNSIGNED_8
	|	MPG123_ENC_FLOAT_32
	|	MPG123_ENC_SIGNED_24
	|	MPG123_ENC_SIGNED_32;
}

/* Draining is flushing to disk. Words do suck at times.
   One could call fsync(), too, but to be safe, that would need to
   be called on the directory, too. Also, apps randomly calling
   fsync() can cause annoying issues in a system. */
void INT123_wav_drain(out123_handle *ao)
{
	struct wavdata *wdat = ao->userptr;

	if(!wdat)
		return;

	if(fflush(wdat->wavfp) && !AOQUIET)
		error1("flushing failed: %s\n", INT123_strerror(errno));
}
