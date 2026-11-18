/*
 * tests/project_channels.cpp
 *
 * Tests for alacconvert's --project channel projection.
 *
 * The tests generate their own multichannel ALAC/CAF inputs with the real
 * ALAC encoder (and independently hand-build one uncompressed multi-element
 * packet), run the production projection path, then verify the result with
 * an independent CAF chunk parser and an independent ALAC element walker.
 * No test reads the projection implementation's internal structures.
 */

#include "task_support/check.h"

#include "ALACAudioTypes.h"
#include "ALACBitUtilities.h"
#include "ALACDecoder.h"
#include "ALACEncoder.h"
#include "ALACProjectChannels.h"
#include "CAFFileALAC.h"

#include "aglib.h"
#include "dplib.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

const char *kDir = ".task-build/pctest";
const uint32_t kFramesPerPacket = 4096;

// ---------------------------------------------------------------------------
// basic helpers
// ---------------------------------------------------------------------------

void ensureDir()
{
#if defined(_WIN32)
	_mkdir(kDir);
#else
	mkdir(kDir, 0777);
#endif
}

std::string testPath(const char *name)
{
	return std::string(kDir) + "/" + name;
}

bool fileExists(const char *name)
{
	FILE *f = fopen(name, "rb");
	if (f == NULL)
		return false;
	fclose(f);
	return true;
}

bool readAll(const char *name, std::vector<uint8_t> *out)
{
	FILE *f = fopen(name, "rb");
	if (f == NULL)
		return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0)
	{
		fclose(f);
		return false;
	}
	out->resize((size_t)n);
	bool ok = (n == 0) || fread(&(*out)[0], 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	return ok;
}

bool writeAll(const char *name, const std::vector<uint8_t> &data)
{
	FILE *f = fopen(name, "wb");
	if (f == NULL)
		return false;
	bool ok = data.empty() || fwrite(&data[0], 1, data.size(), f) == data.size();
	if (fclose(f) != 0)
		ok = false;
	return ok;
}

void putBE16(std::vector<uint8_t> *v, uint16_t x)
{
	v->push_back((uint8_t)(x >> 8));
	v->push_back((uint8_t)x);
}

void putBE32(std::vector<uint8_t> *v, uint32_t x)
{
	v->push_back((uint8_t)(x >> 24));
	v->push_back((uint8_t)(x >> 16));
	v->push_back((uint8_t)(x >> 8));
	v->push_back((uint8_t)x);
}

void putBE64(std::vector<uint8_t> *v, uint64_t x)
{
	putBE32(v, (uint32_t)(x >> 32));
	putBE32(v, (uint32_t)x);
}

uint16_t getBE16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

uint32_t getBE32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint64_t getBE64(const uint8_t *p)
{
	return ((uint64_t)getBE32(p) << 32) | (uint64_t)getBE32(p + 4);
}

uint32_t fourcc(const char *s)
{
	return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
	       ((uint32_t)(uint8_t)s[2] << 8) | (uint8_t)s[3];
}

void putFourCC(std::vector<uint8_t> *v, const char *s)
{
	v->push_back((uint8_t)s[0]);
	v->push_back((uint8_t)s[1]);
	v->push_back((uint8_t)s[2]);
	v->push_back((uint8_t)s[3]);
}

// ---------------------------------------------------------------------------
// standard layout tables (independent copy of the ALAC/CAF built-in layouts)
// ---------------------------------------------------------------------------

const uint32_t kStdTags[8] =
{
	(100u << 16) | 1, (101u << 16) | 2, (113u << 16) | 3, (116u << 16) | 4,
	(120u << 16) | 5, (124u << 16) | 6, (142u << 16) | 7, (127u << 16) | 8
};

const uint32_t kStdLabels[8][8] =
{
	{ 3 },
	{ 1, 2 },
	{ 3, 1, 2 },
	{ 3, 1, 2, 9 },
	{ 3, 1, 2, 5, 6 },
	{ 3, 1, 2, 5, 6, 4 },
	{ 3, 1, 2, 5, 6, 9, 4 },
	{ 3, 7, 8, 1, 2, 5, 6, 4 }
};

// sChannelMaps (independent copy) - 3 bits per channel.
const uint32_t kElementMaps[8] =
{
	0,
	1,
	(1u << 3) | 0,
	(0u << 9) | (1u << 3) | 0,
	(1u << 9) | (1u << 3) | 0,
	(0u << 15) | (1u << 9) | (1u << 3) | 0,
	(0u << 18) | (0u << 15) | (1u << 9) | (1u << 3) | 0,
	(0u << 21) | (1u << 15) | (1u << 9) | (1u << 3) | 0
};

// ---------------------------------------------------------------------------
// CAF parsing
// ---------------------------------------------------------------------------

struct Cfg
{
	int depth;
	int channels;
	int pb;
	int mb;
	int kb;
	int maxRun;
	bool hasChanTag;
	uint32_t chanTag;
	Cfg() : depth(0), channels(0), pb(0), mb(0), kb(0), maxRun(0),
	        hasChanTag(false), chanTag(0) {}
};

struct CAFData
{
	bool ok;
	double sampleRate;
	uint32_t formatID;
	uint32_t formatFlags;
	uint32_t bytesPerPacket;
	uint32_t framesPerPacket;
	uint32_t channels;
	uint32_t bitsPerChannel;
	int64_t numberPackets;
	int64_t validFrames;
	int32_t priming;
	int32_t remainder;
	std::vector<uint32_t> packetSizes;
	std::vector<uint8_t> cookie;
	std::vector<uint8_t> data;
	bool hasChan;
	uint32_t chanTag;
	uint32_t chanBitmap;
	uint32_t chanDescCount;
	std::vector<uint32_t> chanLabels;

	CAFData() : ok(false), sampleRate(0), formatID(0), formatFlags(0),
	            bytesPerPacket(0), framesPerPacket(0), channels(0),
	            bitsPerChannel(0), numberPackets(0), validFrames(0),
	            priming(0), remainder(0), hasChan(false), chanTag(0),
	            chanBitmap(0), chanDescCount(0) {}
};

bool parseCookieConfig(const std::vector<uint8_t> &cookie, Cfg *cfg)
{
	size_t off = 0;
	if (cookie.size() >= 12 && memcmp(&cookie[4], "frma", 4) == 0)
		off += 12;
	if (cookie.size() >= off + 12 && memcmp(&cookie[off + 4], "alac", 4) == 0)
		off += 12;
	if (cookie.size() < off + sizeof(ALACSpecificConfig))
		return false;
	const uint8_t *p = &cookie[off];
	cfg->depth = p[5];
	cfg->pb = p[6];
	cfg->mb = p[7];
	cfg->kb = p[8];
	cfg->channels = p[9];
	cfg->maxRun = getBE16(p + 10);
	cfg->hasChanTag = false;
	cfg->chanTag = 0;
	size_t atom = off + sizeof(ALACSpecificConfig);
	if (cookie.size() >= atom + 24 && memcmp(&cookie[atom + 4], "chan", 4) == 0 &&
	    getBE32(&cookie[atom]) >= 24)
	{
		cfg->hasChanTag = true;
		cfg->chanTag = getBE32(&cookie[atom + 12]);
		// Older encoders (including GetMagicCookie in this tree) wrote the
		// tag in host byte order.  Normalise it to the big-endian value.
		uint32_t count = cfg->chanTag & 0xFFFFu;
		if (count < 1 || count > 8)
		{
			uint32_t tag = cfg->chanTag;
			cfg->chanTag = ((tag & 0x000000FFu) << 24) |
			               ((tag & 0x0000FF00u) << 8) |
			               ((tag >> 8) & 0x0000FF00u) |
			               ((tag >> 24) & 0x000000FFu);
		}
	}
	return true;
}

bool parseCAF(const char *name, CAFData *out)
{
	std::vector<uint8_t> d;
	if (!readAll(name, &d) || d.size() < 8)
		return false;
	if (memcmp(&d[0], "caff", 4) != 0)
		return false;

	bool haveDesc = false, havePakt = false, haveData = false;
	size_t pos = 8;
	while (pos + 12 <= d.size())
	{
		uint32_t id = getBE32(&d[pos]);
		uint64_t size = getBE64(&d[pos + 4]);
		size_t dataOff = pos + 12;
		if (dataOff + size > d.size())
			return false;

		if (id == fourcc("desc"))
		{
			if (size < sizeof(port_CAFAudioDescription))
				return false;
			const uint8_t *p = &d[dataOff];
			uint64_t rateBits = getBE64(p);
			memcpy(&out->sampleRate, &rateBits, 8);
			out->formatID = getBE32(p + 8);
			out->formatFlags = getBE32(p + 12);
			out->bytesPerPacket = getBE32(p + 16);
			out->framesPerPacket = getBE32(p + 20);
			out->channels = getBE32(p + 24);
			out->bitsPerChannel = getBE32(p + 28);
			haveDesc = true;
		}
		else if (id == fourcc("pakt"))
		{
			if (size < 24)
				return false;
			out->numberPackets = (int64_t)getBE64(&d[dataOff]);
			out->validFrames = (int64_t)getBE64(&d[dataOff + 8]);
			out->priming = (int32_t)getBE32(&d[dataOff + 16]);
			out->remainder = (int32_t)getBE32(&d[dataOff + 20]);
			size_t p = dataOff + 24;
			size_t end = dataOff + (size_t)size;
			if (out->numberPackets < 0 || out->numberPackets > (int64_t)d.size())
				return false;
			out->packetSizes.clear();
			for (int64_t i = 0; i < out->numberPackets; ++i)
			{
				uint32_t value = 0;
				int count = 0;
				uint8_t byte;
				do
				{
					if (p >= end || count >= 5)
						return false;
					byte = d[p++];
					value = (value << 7) | (byte & 0x7F);
					++count;
				} while (byte & 0x80);
				out->packetSizes.push_back(value);
			}
			havePakt = true;
		}
		else if (id == fourcc("kuki"))
		{
			out->cookie.assign(d.begin() + dataOff, d.begin() + dataOff + (size_t)size);
		}
		else if (id == fourcc("chan"))
		{
			if (size < 12)
				return false;
			out->hasChan = true;
			out->chanTag = getBE32(&d[dataOff]);
			out->chanBitmap = getBE32(&d[dataOff + 4]);
			out->chanDescCount = getBE32(&d[dataOff + 8]);
			if (size < 12 + (uint64_t)out->chanDescCount * 20)
				return false;
			out->chanLabels.clear();
			for (uint32_t i = 0; i < out->chanDescCount; ++i)
				out->chanLabels.push_back(getBE32(&d[dataOff + 12 + i * 20]));
		}
		else if (id == fourcc("data"))
		{
			if (size < 4)
				return false;
			out->data.assign(d.begin() + dataOff + 4, d.begin() + dataOff + (size_t)size);
			haveData = true;
		}
		pos = dataOff + (size_t)size;
	}
	out->ok = haveDesc && havePakt && haveData;
	return out->ok;
}

uint32_t packetFramesAt(const CAFData &c, int64_t index)
{
	if (index < c.numberPackets - 1)
		return c.framesPerPacket;
	int64_t frames = c.validFrames - (c.numberPackets - 1) * (int64_t)c.framesPerPacket;
	return frames > 0 ? (uint32_t)frames : 0;
}

// ---------------------------------------------------------------------------
// independent ALAC element walker
// ---------------------------------------------------------------------------

struct Elem
{
	int tag;
	int instanceTag;
	uint32_t startBit;
	uint32_t endBit;
	uint32_t channelStart;
	int channels;
	uint32_t samples;
};

bool walkPacket(const std::vector<uint8_t> &raw, const Cfg &cfg,
                uint32_t nominalFrames, std::vector<Elem> *elems,
                uint32_t *outFrames)
{
	std::vector<uint8_t> packet(raw);
	packet.resize(raw.size() + 8, 0);
	BitBuffer bits;
	BitBufferInit(&bits, &packet[0], (uint32_t)raw.size());

	std::vector<int32_t> predictor(nominalFrames + 8);
	elems->clear();
	*outFrames = 0;
	bool haveFrames = false;
	uint32_t channelStart = 0;

	struct Reader
	{
		BitBuffer *b;
		uint32_t read(uint32_t n)
		{
			if (n <= 8)
				return BitBufferReadSmall(b, (uint8_t)n);
			return BitBufferRead(b, (uint8_t)n);
		}
	} rd = { &bits };

	while (true)
	{
		uint32_t start = BitBufferGetPosition(&bits);
		uint32_t tag = rd.read(3);
		if (tag == 7)
			break;
		if (tag != 0 && tag != 1 && tag != 3)
			return false;

		int channels = (tag == 1) ? 2 : 1;
		uint32_t instance = rd.read(4);
		if (rd.read(12) != 0)
			return false;
		uint32_t header = rd.read(4);
		uint32_t partial = header >> 3;
		uint32_t bytesShifted = (header >> 1) & 3;
		uint32_t escape = header & 1;
		if (bytesShifted == 3)
			return false;

		uint32_t samples = nominalFrames;
		if (partial)
			samples = (rd.read(16) << 16) | rd.read(16);
		if (!haveFrames)
		{
			*outFrames = samples;
			haveFrames = true;
		}
		else if (samples != *outFrames)
		{
			return false;
		}

		uint32_t chanBits;
		if (tag == 1)
		{
			chanBits = (uint32_t)cfg.depth - bytesShifted * 8 + 1;
			if (!escape)
			{
				rd.read(8); rd.read(8);
				rd.read(8);
				uint32_t h = rd.read(8);
				uint32_t pbU = h >> 5;
				uint32_t numU = h & 31;
				for (uint32_t i = 0; i < numU; ++i) rd.read(16);
				rd.read(8);
				h = rd.read(8);
				uint32_t pbV = h >> 5;
				uint32_t numV = h & 31;
				for (uint32_t i = 0; i < numV; ++i) rd.read(16);
				if (bytesShifted != 0)
					BitBufferAdvance(&bits, (bytesShifted * 8) * 2 * samples);
				for (int which = 0; which < 2; ++which)
				{
					AGParamRec ag;
					uint32_t pbFactor = which == 0 ? pbU : pbV;
					set_ag_params(&ag, cfg.mb, (cfg.pb * pbFactor) / 4, cfg.kb,
					              samples, samples, cfg.maxRun);
					uint32_t used = 0;
					if (dyn_decomp(&ag, &bits, &predictor[0], (int32_t)samples,
					               (int32_t)chanBits, &used) != ALAC_noErr)
						return false;
				}
			}
			else
			{
				BitBufferAdvance(&bits, samples * (uint32_t)cfg.depth * 2);
			}
		}
		else
		{
			chanBits = (uint32_t)cfg.depth - bytesShifted * 8;
			if (!escape)
			{
				rd.read(8); rd.read(8);
				rd.read(8);
				uint32_t h = rd.read(8);
				uint32_t pbU = h >> 5;
				uint32_t numU = h & 31;
				for (uint32_t i = 0; i < numU; ++i) rd.read(16);
				if (bytesShifted != 0)
					BitBufferAdvance(&bits, (bytesShifted * 8) * samples);
				AGParamRec ag;
				set_ag_params(&ag, cfg.mb, (cfg.pb * pbU) / 4, cfg.kb,
				              samples, samples, cfg.maxRun);
				uint32_t used = 0;
				if (dyn_decomp(&ag, &bits, &predictor[0], (int32_t)samples,
				               (int32_t)chanBits, &used) != ALAC_noErr)
					return false;
			}
			else
			{
				BitBufferAdvance(&bits, samples * chanBits);
			}
		}

		size_t pos = BitBufferGetPosition(&bits);
		if (pos > packet.size() * 8)
			return false;

		Elem e;
		e.tag = (int)tag;
		e.instanceTag = (int)instance;
		e.startBit = start;
		e.endBit = (uint32_t)pos;
		e.channelStart = channelStart;
		e.channels = channels;
		e.samples = samples;
		elems->push_back(e);
		channelStart += (uint32_t)channels;
	}
	return haveFrames;
}

bool bitsEqual(const std::vector<uint8_t> &a, uint32_t aStart,
               const std::vector<uint8_t> &b, uint32_t bStart, uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i)
	{
		uint32_t pa = aStart + i;
		uint32_t pb = bStart + i;
		if (pa / 8 >= a.size() || pb / 8 >= b.size())
			return false;
		uint8_t ba = (a[pa / 8] >> (7 - (pa % 8))) & 1;
		uint8_t bb = (b[pb / 8] >> (7 - (pb % 8))) & 1;
		if (ba != bb)
			return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// decoding PCM with the real decoder
// ---------------------------------------------------------------------------

bool decodePCM(const CAFData &caf, std::vector<uint8_t> *pcm,
               std::vector<uint32_t> *frames)
{
	Cfg cfg;
	if (!parseCookieConfig(caf.cookie, &cfg))
		return false;
	int bps = cfg.depth / 8;
	if (bps != 2 && bps != 3)
		return false;

	ALACDecoder decoder;
	if (decoder.Init((void *)&caf.cookie[0], (uint32_t)caf.cookie.size()) != ALAC_noErr)
		return false;

	pcm->clear();
	frames->clear();
	size_t offset = 0;
	for (int64_t i = 0; i < caf.numberPackets; ++i)
	{
		uint32_t size = caf.packetSizes[(size_t)i];
		if (offset + size > caf.data.size())
			return false;
		std::vector<uint8_t> packet(caf.data.begin() + offset,
		                            caf.data.begin() + offset + size);
		offset += size;
		packet.resize(packet.size() + 8, 0);

		BitBuffer bits;
		BitBufferInit(&bits, &packet[0], size);
		uint32_t hint = packetFramesAt(caf, i);
		std::vector<uint8_t> out((size_t)hint * caf.channels * bps + 4096 * 4);
		uint32_t got = 0;
		if (decoder.Decode(&bits, &out[0], hint, caf.channels, &got) != ALAC_noErr)
			return false;
		pcm->insert(pcm->end(), out.begin(), out.begin() + (size_t)got * caf.channels * bps);
		frames->push_back(got);
	}
	return true;
}

// ---------------------------------------------------------------------------
// generating source CAF files
// ---------------------------------------------------------------------------

std::vector<uint8_t> makeCookie(int depth, int channels)
{
	AudioFormatDescription fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.mSampleRate = 48000;
	fmt.mFormatID = kALACFormatAppleLossless;
	fmt.mFormatFlags = (depth == 16) ? 1 : 3;
	fmt.mChannelsPerFrame = channels;
	fmt.mFramesPerPacket = kFramesPerPacket;

	ALACEncoder encoder;
	encoder.SetFrameSize(kFramesPerPacket);
	encoder.InitializeEncoder(fmt);
	uint32_t size = encoder.GetMagicCookieSize(channels);
	std::vector<uint8_t> cookie(size);
	encoder.GetMagicCookie(&cookie[0], &size);
	cookie.resize(size);
	return cookie;
}

bool writeCAFFile(const char *name, int depth, int channels,
                  const std::vector<std::vector<uint8_t> > &packets,
                  int64_t validFrames, const std::vector<uint8_t> &cookie,
                  bool haveChan, uint32_t chanTag,
                  const std::vector<uint32_t> *chanDescriptions)
{
	std::vector<uint8_t> file;
	putFourCC(&file, "caff");
	putBE16(&file, 1);
	putBE16(&file, 0);

	// desc
	std::vector<uint8_t> desc;
	double rate = 48000.0;
	uint64_t rateBits;
	memcpy(&rateBits, &rate, 8);
	putBE64(&desc, rateBits);
	putBE32(&desc, kALACFormatAppleLossless);
	putBE32(&desc, (depth == 16) ? 1u : 3u);
	putBE32(&desc, 0);                 // variable bytes per packet
	putBE32(&desc, kFramesPerPacket);
	putBE32(&desc, (uint32_t)channels);
	putBE32(&desc, 0);                 // no discernible bits per sample
	putFourCC(&file, "desc");
	putBE64(&file, desc.size());
	file.insert(file.end(), desc.begin(), desc.end());

	// kuki
	putFourCC(&file, "kuki");
	putBE64(&file, cookie.size());
	file.insert(file.end(), cookie.begin(), cookie.end());

	// chan
	if (haveChan)
	{
		std::vector<uint8_t> chan;
		if (chanDescriptions != NULL)
		{
			putBE32(&chan, 0); // UseChannelDescriptions
			putBE32(&chan, 0);
			putBE32(&chan, (uint32_t)chanDescriptions->size());
			for (size_t i = 0; i < chanDescriptions->size(); ++i)
			{
				putBE32(&chan, (*chanDescriptions)[i]);
				putBE32(&chan, 0);
				putBE32(&chan, 0);
				putBE32(&chan, 0);
				putBE32(&chan, 0);
			}
		}
		else
		{
			putBE32(&chan, chanTag);
			putBE32(&chan, 0);
			putBE32(&chan, 0);
		}
		putFourCC(&file, "chan");
		putBE64(&file, chan.size());
		file.insert(file.end(), chan.begin(), chan.end());
	}

	// pakt (byte sizes only; desc has a non-zero framesPerPacket)
	std::vector<uint8_t> pakt;
	putBE64(&pakt, (uint64_t)packets.size());
	putBE64(&pakt, (uint64_t)validFrames);
	putBE32(&pakt, 0);
	putBE32(&pakt, 0);
	for (size_t i = 0; i < packets.size(); ++i)
	{
		uint32_t value = (uint32_t)packets[i].size();
		uint8_t temp[5];
		int count = 0;
		do
		{
			temp[count++] = (uint8_t)(value & 0x7F);
			value >>= 7;
		} while (value != 0);
		for (int j = count - 1; j >= 0; --j)
		{
			uint8_t byte = temp[j];
			if (j != 0)
				byte |= 0x80;
			pakt.push_back(byte);
		}
	}
	putFourCC(&file, "pakt");
	putBE64(&file, pakt.size());
	file.insert(file.end(), pakt.begin(), pakt.end());

	// data
	std::vector<uint8_t> data;
	putBE32(&data, 0); // edit count
	for (size_t i = 0; i < packets.size(); ++i)
		data.insert(data.end(), packets[i].begin(), packets[i].end());
	putFourCC(&file, "data");
	putBE64(&file, data.size());
	file.insert(file.end(), data.begin(), data.end());

	return writeAll(name, file);
}

void generatePCM(int depth, int channels, int frames, std::vector<uint8_t> *pcm)
{
	int bps = depth / 8;
	pcm->assign((size_t)frames * channels * bps, 0);
	for (int i = 0; i < frames; ++i)
	{
		for (int c = 0; c < channels; ++c)
		{
			int32_t value = (int32_t)((i * 97 + c * 7919 + 13) % 20001 - 10000) * 3 + c * 113;
			if (depth == 16)
			{
				int16_t sample = (int16_t)value;
				memcpy(&(*pcm)[((size_t)i * channels + c) * bps], &sample, 2);
			}
			else
			{
				int32_t sample = value << 4;
				if (sample > 0x7FFFFF) sample = 0x7FFFFF;
				if (sample < -0x800000) sample = -0x800000;
				uint8_t *p = &(*pcm)[((size_t)i * channels + c) * bps];
				p[0] = (uint8_t)(sample & 0xFF);
				p[1] = (uint8_t)((sample >> 8) & 0xFF);
				p[2] = (uint8_t)((sample >> 16) & 0xFF);
			}
		}
	}
}

bool buildEncodedSource(const char *name, int depth, int channels,
                        int fullPackets, int tail)
{
	int frames = fullPackets * (int)kFramesPerPacket + tail;
	std::vector<uint8_t> pcm;
	generatePCM(depth, channels, frames, &pcm);

	AudioFormatDescription in;
	memset(&in, 0, sizeof(in));
	in.mSampleRate = 48000;
	in.mFormatID = kALACFormatLinearPCM;
	in.mFormatFlags = kALACFormatFlagIsSignedInteger | kALACFormatFlagIsPacked;
	in.mBytesPerPacket = in.mBytesPerFrame =
	    (uint32_t)((depth / 8) * channels);
	in.mFramesPerPacket = 1;
	in.mChannelsPerFrame = channels;
	in.mBitsPerChannel = depth;

	AudioFormatDescription out;
	memset(&out, 0, sizeof(out));
	out.mSampleRate = 48000;
	out.mFormatID = kALACFormatAppleLossless;
	out.mFormatFlags = (depth == 16) ? 1 : 3;
	out.mChannelsPerFrame = channels;
	out.mFramesPerPacket = kFramesPerPacket;

	ALACEncoder encoder;
	encoder.SetFrameSize(kFramesPerPacket);
	if (encoder.InitializeEncoder(out) != ALAC_noErr)
		return false;

	std::vector<std::vector<uint8_t> > packets;
	std::vector<uint8_t> buffer((size_t)in.mBytesPerPacket * kFramesPerPacket + 65536);
	int pos = 0;
	while (pos < frames)
	{
		int count = frames - pos;
		if (count > (int)kFramesPerPacket)
			count = (int)kFramesPerPacket;
		int32_t bytes = (int32_t)((size_t)count * in.mBytesPerPacket);
		if (encoder.Encode(in, in, &pcm[(size_t)pos * in.mBytesPerPacket],
		                   &buffer[0], &bytes) != ALAC_noErr)
			return false;
		packets.push_back(std::vector<uint8_t>(buffer.begin(), buffer.begin() + bytes));
		pos += count;
	}
	encoder.Finish();

	std::vector<uint8_t> cookie = makeCookie(depth, channels);
	return writeCAFFile(name, depth, channels, packets, frames, cookie,
	                    true, kStdTags[channels - 1], NULL);
}

// Hand-build a single uncompressed multi-element packet using escape elements.
// When invalidFirstHeader is true, the first element's 12 "unused header"
// bits are non-zero.  The packet stays structurally complete (correct element
// sizes, one ID_END, byte aligned) but the first (discarded) element is not a
// valid audio element.
bool buildUncompressedSourceEx(const char *name, int depth, int channels, int frames,
                               bool invalidFirstHeader)
{
	if (channels != 6)
		return false;

	int tags[4] = { 0, 1, 1, 3 };
	int tagChannels[4] = { 1, 2, 2, 1 };
	std::vector<uint32_t> samples;
	samples.assign((size_t)frames * channels, 0);
	for (int i = 0; i < frames; ++i)
		for (int c = 0; c < channels; ++c)
			samples[(size_t)i * channels + c] = (uint32_t)(int32_t)((i * 53 + c * 911 + 7) % 3001 - 1500);

	// bit writer
	struct Writer
	{
		std::vector<uint8_t> bytes;
		uint32_t bit;
		Writer() : bit(0) { bytes.push_back(0); }
		void write(uint32_t value, uint32_t count)
		{
			for (int i = (int)count - 1; i >= 0; --i)
			{
				size_t index = bit / 8;
				if (index >= bytes.size())
					bytes.resize(index + 2, 0);
				uint8_t mask = (uint8_t)(1u << (7 - (bit % 8)));
				if ((value >> i) & 1)
					bytes[index] |= mask;
				else
					bytes[index] &= (uint8_t)~mask;
				++bit;
			}
		}
		void align()
		{
			while (bit % 8 != 0)
				write(0, 1);
		}
	} writer;
	int instance[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	int channelOffset = 0;
	for (int e = 0; e < 4; ++e)
	{
		int tag = tags[e];
		writer.write((uint32_t)tag, 3);
		writer.write((uint32_t)instance[tag]++, 4);
		writer.write((e == 0 && invalidFirstHeader) ? 1u : 0u, 12);
		writer.write((1u << 3) | 1u, 4); // partial frame + escape
		writer.write((uint32_t)frames, 32);
		for (int i = 0; i < frames; ++i)
		{
			for (int c = 0; c < tagChannels[e]; ++c)
				writer.write(samples[(size_t)i * channels + channelOffset + c],
				             (uint32_t)depth);
		}
		channelOffset += tagChannels[e];
	}
	writer.write(7, 3); // ID_END
	writer.align();
	writer.bytes.resize((writer.bit + 7) / 8);

	std::vector<std::vector<uint8_t> > packets;
	packets.push_back(writer.bytes);
	std::vector<uint8_t> cookie = makeCookie(depth, channels);
	return writeCAFFile(name, depth, channels, packets, frames, cookie,
	                    false, 0, NULL);
}

bool buildUncompressedSource(const char *name, int depth, int channels, int frames)
{
	return buildUncompressedSourceEx(name, depth, channels, frames, false);
}

// ---------------------------------------------------------------------------
// projection verification
// ---------------------------------------------------------------------------

bool same(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b)
{
	return a.size() == b.size() && (a.empty() || memcmp(&a[0], &b[0], a.size() * sizeof(uint32_t)) == 0);
}

bool expectedReuse(const std::vector<Elem> &source, int tag, int channels,
                   uint32_t firstSlot, const std::vector<uint32_t> &slots,
                   uint32_t outputStart, int *sourceIndex)
{
	for (size_t t = 0; t < source.size(); ++t)
	{
		const Elem &e = source[t];
		if (e.tag != tag || (int)e.channels != channels)
			continue;
		if (e.channelStart != firstSlot)
			continue;
		bool exact = true;
		for (int i = 0; i < channels; ++i)
		{
			if (slots[outputStart + i] != e.channelStart + (uint32_t)i)
			{
				exact = false;
				break;
			}
		}
		if (exact)
		{
			*sourceIndex = (int)t;
			return true;
		}
	}
	return false;
}

// Verifies the whole projected file: PCM, container, element reuse.
bool verifyProjection(const char *inFile, const std::vector<uint32_t> &slots,
                      const char *outFile, bool checkReuse)
{
	CAFData in, out;
	if (!parseCAF(inFile, &in) || !parseCAF(outFile, &out))
	{
		fprintf(stderr, "verify: cannot parse CAF\n");
		return false;
	}
	Cfg inCfg, outCfg;
	if (!parseCookieConfig(in.cookie, &inCfg) || !parseCookieConfig(out.cookie, &outCfg))
	{
		fprintf(stderr, "verify: cannot parse cookie\n");
		return false;
	}

	std::vector<uint8_t> sourcePCM, outputPCM;
	std::vector<uint32_t> sourceFrames, outputFrames;
	if (!decodePCM(in, &sourcePCM, &sourceFrames) ||
	    !decodePCM(out, &outputPCM, &outputFrames))
	{
		fprintf(stderr, "verify: cannot decode PCM\n");
		return false;
	}

	int depth = inCfg.depth;
	int bps = depth / 8;
	uint32_t outChannels = (uint32_t)slots.size();

	// PCM must equal the projected source PCM.
	if (outCfg.depth != depth || out.channels != outChannels || outputFrames != sourceFrames)
	{
		fprintf(stderr, "verify: format/frame mismatch\n");
		return false;
	}
	if (outputPCM.size() != sourcePCM.size() / (size_t)in.channels * outChannels)
	{
		fprintf(stderr, "verify: PCM size mismatch\n");
		return false;
	}
	size_t frames = sourcePCM.size() / ((size_t)in.channels * bps);
	for (size_t i = 0; i < frames; ++i)
	{
		for (uint32_t c = 0; c < outChannels; ++c)
		{
			const uint8_t *a = &sourcePCM[(i * in.channels + slots[c]) * bps];
			const uint8_t *b = &outputPCM[(i * outChannels + c) * bps];
			if (memcmp(a, b, bps) != 0)
			{
				fprintf(stderr, "verify: PCM differs at frame %zu channel %u\n", i, c);
				return false;
			}
		}
	}

	// Container invariants.
	if (out.formatID != kALACFormatAppleLossless || out.framesPerPacket != kFramesPerPacket ||
	    out.bytesPerPacket != 0 || out.formatFlags != (uint32_t)(depth == 16 ? 1 : 3))
	{
		fprintf(stderr, "verify: desc fields wrong\n");
		return false;
	}
	if (out.numberPackets != in.numberPackets || out.validFrames != in.validFrames ||
	    out.priming != in.priming || out.remainder != in.remainder)
	{
		fprintf(stderr, "verify: pakt counters not inherited\n");
		return false;
	}
	int64_t totalBytes = 0;
	uint32_t maxPacket = 0;
	for (size_t i = 0; i < out.packetSizes.size(); ++i)
	{
		totalBytes += out.packetSizes[i];
		if (out.packetSizes[i] > maxPacket)
			maxPacket = out.packetSizes[i];
	}
	if ((int64_t)out.data.size() != totalBytes)
	{
		fprintf(stderr, "verify: pakt does not close over data\n");
		return false;
	}

	// Cookie config.
	const uint8_t *config = &out.cookie[0];
	size_t configOffset = 0;
	if (out.cookie.size() >= 12 && memcmp(&out.cookie[4], "frma", 4) == 0)
		configOffset += 12;
	if (out.cookie.size() >= configOffset + 12 &&
	    memcmp(&out.cookie[configOffset + 4], "alac", 4) == 0)
		configOffset += 12;
	const uint8_t *cfgBytes = config + configOffset;
	if (getBE32(cfgBytes) != kFramesPerPacket || cfgBytes[4] != 0 ||
	    cfgBytes[5] != (uint8_t)depth || cfgBytes[9] != (uint8_t)outChannels ||
	    getBE16(cfgBytes + 10) != 255 || getBE32(cfgBytes + 12) != maxPacket ||
	    getBE32(cfgBytes + 20) != (uint32_t)in.sampleRate)
	{
		fprintf(stderr, "verify: cookie config wrong\n");
		return false;
	}

	// Roles: output labels are inherited from the input channel roles.
	std::vector<uint32_t> inputLabels;
	if (in.hasChan)
	{
		if (in.chanTag == 0)
		{
			inputLabels = in.chanLabels;
		}
		else
		{
			uint32_t count = in.chanTag & 0xFFFFu;
			if (count < 1 || count > 8 || count != in.channels)
				return false;
			inputLabels.assign(kStdLabels[count - 1], kStdLabels[count - 1] + count);
		}
	}
	else if (inCfg.hasChanTag)
	{
		uint32_t count = inCfg.chanTag & 0xFFFFu;
		if (count < 1 || count > 8 || count != in.channels)
			return false;
		inputLabels.assign(kStdLabels[count - 1], kStdLabels[count - 1] + count);
	}
	else
	{
		return false;
	}

	std::vector<uint32_t> outputLabels;
	for (uint32_t i = 0; i < outChannels; ++i)
		outputLabels.push_back(inputLabels[slots[i]]);
	bool standard = true;
	if (outChannels < 1 || outChannels > 8)
		standard = false;
	else
		for (uint32_t i = 0; i < outChannels; ++i)
			if (outputLabels[i] != kStdLabels[outChannels - 1][i])
				standard = false;

	if (standard && outChannels > 2)
	{
		uint32_t expectedTag = kStdTags[outChannels - 1];
		bool cookieHasChan = false;
		uint32_t cookieChan = 0;
		size_t atom = configOffset + sizeof(ALACSpecificConfig);
		if (out.cookie.size() >= atom + 24 &&
		    memcmp(&out.cookie[atom + 4], "chan", 4) == 0)
		{
			cookieHasChan = true;
			cookieChan = getBE32(&out.cookie[atom + 12]);
		}
		if (!cookieHasChan || cookieChan != expectedTag)
		{
			fprintf(stderr, "verify: standard cookie chan atom wrong\n");
			return false;
		}
		if (!out.hasChan || out.chanTag != expectedTag)
		{
			fprintf(stderr, "verify: standard CAF chan chunk wrong\n");
			return false;
		}
	}
	else if (!standard)
	{
		size_t atom = configOffset + sizeof(ALACSpecificConfig);
		if (out.cookie.size() >= atom + 24 &&
		    memcmp(&out.cookie[atom + 4], "chan", 4) == 0)
		{
			fprintf(stderr, "verify: non-standard cookie should omit chan atom\n");
			return false;
		}
		if (!out.hasChan || out.chanTag != 0 || out.chanDescCount != outChannels ||
		    !same(out.chanLabels, outputLabels))
		{
			fprintf(stderr, "verify: non-standard chan descriptions wrong\n");
			return false;
		}
	}

	if (!checkReuse)
		return true;

	// Element-level verification.
	if (outputFrames != sourceFrames)
		return false;
	size_t sourceOffset = 0, outputOffset = 0;
	uint32_t map = (outChannels >= 1 && outChannels <= 8) ? kElementMaps[outChannels - 1] : 0;

	for (int64_t p = 0; p < out.numberPackets; ++p)
	{
		int instanceCounters[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		uint32_t inSize = in.packetSizes[(size_t)p];
		uint32_t outSize = out.packetSizes[(size_t)p];
		if (sourceOffset + inSize > in.data.size() || outputOffset + outSize > out.data.size())
			return false;
		std::vector<uint8_t> inPacket(in.data.begin() + sourceOffset,
		                              in.data.begin() + sourceOffset + inSize);
		std::vector<uint8_t> outPacket(out.data.begin() + outputOffset,
		                               out.data.begin() + outputOffset + outSize);
		sourceOffset += inSize;
		outputOffset += outSize;

		std::vector<Elem> inElems, outElems;
		uint32_t inPacketFrames = 0, outPacketFrames = 0;
		if (!walkPacket(inPacket, inCfg, kFramesPerPacket, &inElems, &inPacketFrames) ||
		    !walkPacket(outPacket, outCfg, kFramesPerPacket, &outElems, &outPacketFrames))
		{
			fprintf(stderr, "verify: independent element walk failed\n");
			return false;
		}
		if (inPacketFrames != sourceFrames[(size_t)p] || outPacketFrames != outputFrames[(size_t)p])
		{
			fprintf(stderr, "verify: element walk frame count mismatch\n");
			return false;
		}

		// Expected output element map.
		struct Expected { int tag; int channels; uint32_t outStart; };
		std::vector<Expected> expected;
		uint32_t channel = 0;
		while (channel < outChannels)
		{
			uint32_t tag = (map >> (channel * 3)) & 7u;
			Expected e;
			e.tag = (int)tag;
			e.channels = (tag == 1) ? 2 : 1;
			e.outStart = channel;
			expected.push_back(e);
			channel += (uint32_t)e.channels;
		}
		if (outElems.size() != expected.size())
		{
			fprintf(stderr, "verify: output element count wrong\n");
			return false;
		}

		for (size_t e = 0; e < expected.size(); ++e)
		{
			const Elem &outElem = outElems[e];
			if (outElem.tag != expected[e].tag || (int)outElem.channels != expected[e].channels)
			{
				fprintf(stderr, "verify: output element type/order wrong\n");
				return false;
			}
			if (outElem.instanceTag != instanceCounters[outElem.tag])
			{
				fprintf(stderr, "verify: instance tag not renumbered\n");
				return false;
			}
			instanceCounters[outElem.tag]++;

			int sourceIndex = -1;
			bool reuse = expectedReuse(inElems, expected[e].tag,
			                           expected[e].channels, slots[expected[e].outStart],
			                           slots, expected[e].outStart, &sourceIndex);
			if (reuse)
			{
				const Elem &src = inElems[(size_t)sourceIndex];
				uint32_t srcBodyStart = src.startBit + 7;
				uint32_t outBodyStart = outElem.startBit + 7;
				uint32_t srcBits = src.endBit - srcBodyStart;
				uint32_t outBits = outElem.endBit - outBodyStart;
				if (srcBits != outBits || !bitsEqual(inPacket, srcBodyStart,
				                                    outPacket, outBodyStart, srcBits))
				{
					fprintf(stderr, "verify: reused element bits differ\n");
					return false;
				}
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// small file mutators for failure cases
// ---------------------------------------------------------------------------

struct ChunkRef { size_t header; size_t data; uint64_t size; };

std::vector<ChunkRef> chunkList(const std::vector<uint8_t> &d)
{
	std::vector<ChunkRef> list;
	size_t pos = 8;
	while (pos + 12 <= d.size())
	{
		ChunkRef ref;
		ref.header = pos;
		ref.data = pos + 12;
		ref.size = getBE64(&d[pos + 4]);
		if (ref.data + ref.size > d.size())
			break;
		list.push_back(ref);
		pos = ref.data + (size_t)ref.size;
	}
	return list;
}

bool mutateChunk(const char *in, const char *out, const char *id,
                 const std::vector<uint8_t> &newData)
{
	std::vector<uint8_t> d;
	if (!readAll(in, &d))
		return false;
	uint32_t target = fourcc(id);
	std::vector<ChunkRef> chunks = chunkList(d);
	for (size_t i = 0; i < chunks.size(); ++i)
	{
		if (getBE32(&d[chunks[i].header]) != target)
			continue;
		std::vector<uint8_t> result;
		result.insert(result.end(), d.begin(), d.begin() + chunks[i].header);
		putFourCC(&result, id);
		putBE64(&result, newData.size());
		result.insert(result.end(), newData.begin(), newData.end());
		result.insert(result.end(), d.begin() + chunks[i].data + chunks[i].size, d.end());
		return writeAll(out, result);
	}
	return false;
}

bool swapCookieChanTag(const char *in, const char *out)
{
	std::vector<uint8_t> d;
	if (!readAll(in, &d))
		return false;
	std::vector<ChunkRef> chunks = chunkList(d);
	for (size_t i = 0; i < chunks.size(); ++i)
	{
		if (getBE32(&d[chunks[i].header]) != fourcc("kuki"))
			continue;
		size_t atom = chunks[i].data + sizeof(ALACSpecificConfig);
		if (atom + 24 > chunks[i].data + chunks[i].size)
			return false;
		if (memcmp(&d[atom + 4], "chan", 4) != 0)
			return false;
		uint32_t tag = getBE32(&d[atom + 12]);
		uint32_t swapped = ((tag & 0x000000FFu) << 24) |
		                   ((tag & 0x0000FF00u) << 8) |
		                   ((tag >> 8) & 0x0000FF00u) |
		                   ((tag >> 24) & 0x000000FFu);
		d[atom + 12] = (uint8_t)(swapped >> 24);
		d[atom + 13] = (uint8_t)(swapped >> 16);
		d[atom + 14] = (uint8_t)(swapped >> 8);
		d[atom + 15] = (uint8_t)swapped;
		return writeAll(out, d);
	}
	return false;
}

bool setPaktValidFrames(const char *in, const char *out, int64_t validFrames)
{
	std::vector<uint8_t> d;
	if (!readAll(in, &d))
		return false;
	std::vector<ChunkRef> chunks = chunkList(d);
	for (size_t i = 0; i < chunks.size(); ++i)
	{
		if (getBE32(&d[chunks[i].header]) != fourcc("pakt"))
			continue;
		size_t at = chunks[i].data + 8;
		uint64_t v = (uint64_t)validFrames;
		for (int b = 0; b < 8; ++b)
			d[at + b] = (uint8_t)(v >> (56 - b * 8));
		return writeAll(out, d);
	}
	return false;
}

// Rewrites the CAF 'chan' chunk as explicit Core Audio channel descriptions.
bool setChanDescriptions(const char *in, const char *out,
                         const std::vector<uint32_t> &labels)
{
	std::vector<uint8_t> replacement;
	putBE32(&replacement, 0); // UseChannelDescriptions
	putBE32(&replacement, 0);
	putBE32(&replacement, (uint32_t)labels.size());
	for (size_t i = 0; i < labels.size(); ++i)
	{
		putBE32(&replacement, labels[i]);
		putBE32(&replacement, 0);
		putBE32(&replacement, 0);
		putBE32(&replacement, 0);
		putBE32(&replacement, 0);
	}
	return mutateChunk(in, out, "chan", replacement);
}

bool growDataChunk(const char *in, const char *out)
{
	std::vector<uint8_t> d;
	if (!readAll(in, &d))
		return false;
	std::vector<ChunkRef> chunks = chunkList(d);
	for (size_t i = 0; i < chunks.size(); ++i)
	{
		if (getBE32(&d[chunks[i].header]) != fourcc("data"))
			continue;
		uint64_t size = chunks[i].size + 1;
		size_t at = chunks[i].header + 4;
		for (int b = 0; b < 8; ++b)
			d[at + b] = (uint8_t)(size >> (56 - b * 8));
		d.push_back(0);
		return writeAll(out, d);
	}
	return false;
}

int runProjection(const char *in, const char *out, const char *list)
{
	std::vector<uint32_t> slots;
	const char *error = NULL;
	if (ParseProjectSlots(list, &slots, &error) != 0)
		return -2;
	return RunChannelProjection(in, out, slots);
}

int runCLIProjection(const char *in, const char *out, const char *list)
{
	std::string command = std::string(".task-build/alacconvert --project=") +
	                      list + " \"" + in + "\" \"" + out + "\" >/dev/null 2>&1";
	int rc = system(command.c_str());
#if defined(_WIN32)
	return rc;
#else
	if (rc == -1)
		return -1;
	if (WIFEXITED(rc))
		return WEXITSTATUS(rc);
	return -2;
#endif
}

// ---------------------------------------------------------------------------
// test cases
// ---------------------------------------------------------------------------

int caseReuseWholeCPE()
{
	ensureDir();
	std::string in = testPath("reuse_cpe_in.caf");
	std::string out = testPath("reuse_cpe_out.caf");
	remove(out.c_str());
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 2, 77));
	uint32_t slotsArr[] = { 1, 2 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 2);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));
	// Reused body begins immediately after tag+instance, i.e. at bit 7.
	CAFData outData;
	TASK_CHECK(parseCAF(out.c_str(), &outData));
	std::vector<uint8_t> packet(outData.data.begin(), outData.data.begin() + outData.packetSizes[0]);
	std::vector<Elem> elems;
	Cfg cfg;
	uint32_t frames = 0;
	TASK_CHECK(parseCookieConfig(outData.cookie, &cfg));
	TASK_CHECK(walkPacket(packet, cfg, kFramesPerPacket, &elems, &frames));
	TASK_CHECK(elems.size() == 1 && elems[0].tag == 1 && elems[0].startBit == 0);
	TASK_CHECK(elems[0].endBit > 7 && (elems[0].endBit - 7) % 8 != 0);
	return 0;
}

int caseReuseWholeElementsReordered()
{
	ensureDir();
	std::string in = testPath("reuse_reorder_in.caf");
	std::string out = testPath("reuse_reorder_out.caf");
	remove(out.c_str());
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 2, 31));
	uint32_t slotsArr[] = { 0, 3, 4, 1, 2, 5 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 6);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));

	// Whole elements are kept but re-ordered, so at least one reused element
	// starts (and therefore is copied to) a non-byte-aligned bit position.
	CAFData data;
	TASK_CHECK(parseCAF(out.c_str(), &data));
	std::vector<uint8_t> packet(data.data.begin(), data.data.begin() + data.packetSizes[0]);
	Cfg cfg;
	std::vector<Elem> elems;
	uint32_t frames = 0;
	TASK_CHECK(parseCookieConfig(data.cookie, &cfg));
	TASK_CHECK(walkPacket(packet, cfg, kFramesPerPacket, &elems, &frames));
	TASK_CHECK(elems.size() == 4);
	bool sawUnaligned = false;
	for (size_t i = 1; i < elems.size(); ++i)
		if (elems[i].startBit % 8 != 0)
			sawUnaligned = true;
	TASK_CHECK(sawUnaligned);
	return 0;
}

int caseReuse24Bit()
{
	ensureDir();
	std::string in = testPath("reuse24_in.caf");
	std::string out = testPath("reuse24_out.caf");
	remove(out.c_str());
	TASK_CHECK(buildEncodedSource(in.c_str(), 24, 6, 1, 5));
	uint32_t slotsArr[] = { 3, 4 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 2);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));
	return 0;
}

int caseReuseLFE()
{
	ensureDir();
	std::string in = testPath("reuse_lfe_in.caf");
	std::string out = testPath("reuse_lfe_out.caf");
	remove(out.c_str());
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 0));
	uint32_t slotsArr[] = { 5 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 1);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));
	return 0;
}

int caseRepairSplitCPE()
{
	ensureDir();
	std::string in = testPath("repair_split_in.caf");
	std::string out = testPath("repair_split_out.caf");
	remove(out.c_str());
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 40));
	uint32_t slotsArr[] = { 1 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 1);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));

	// The single output element must be an SCE, never the source CPE.
	CAFData data;
	TASK_CHECK(parseCAF(out.c_str(), &data));
	std::vector<uint8_t> packet(data.data.begin(), data.data.begin() + data.packetSizes[0]);
	Cfg cfg;
	std::vector<Elem> elems;
	uint32_t frames = 0;
	TASK_CHECK(parseCookieConfig(data.cookie, &cfg));
	TASK_CHECK(walkPacket(packet, cfg, kFramesPerPacket, &elems, &frames));
	TASK_CHECK(elems.size() == 1 && elems[0].tag == 0);
	return 0;
}

int caseRepairCrossElementPair()
{
	ensureDir();
	std::string in = testPath("repair_pair_in.caf");
	std::string out = testPath("repair_pair_out.caf");
	remove(out.c_str());
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 11));
	uint32_t slotsArr[] = { 0, 1 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 2);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));
	return 0;
}

int caseRepairSwapPair()
{
	ensureDir();
	std::string in = testPath("repair_swap_in.caf");
	std::string out = testPath("repair_swap_out.caf");
	remove(out.c_str());
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 9));
	uint32_t slotsArr[] = { 0, 2, 1 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 3);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));

	// C is repaired to an SCE; L R survives as a reused CPE.
	CAFData data;
	TASK_CHECK(parseCAF(out.c_str(), &data));
	std::vector<uint8_t> packet(data.data.begin(), data.data.begin() + data.packetSizes[0]);
	Cfg cfg;
	std::vector<Elem> elems;
	uint32_t frames = 0;
	TASK_CHECK(parseCookieConfig(data.cookie, &cfg));
	TASK_CHECK(walkPacket(packet, cfg, kFramesPerPacket, &elems, &frames));
	TASK_CHECK(elems.size() == 2);
	TASK_CHECK(elems[0].tag == 0);
	TASK_CHECK(elems[1].tag == 1);
	return 0;
}

int caseRepairUncompressed()
{
	ensureDir();
	std::string in = testPath("repair_unc_in.caf");
	std::string out = testPath("repair_unc_out.caf");
	TASK_CHECK(buildUncompressedSource(in.c_str(), 16, 6, 100));

	// Whole CPE survives unchanged.
	remove(out.c_str());
	uint32_t wholeArr[] = { 1, 2 };
	std::vector<uint32_t> whole(wholeArr, wholeArr + 2);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), whole) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), whole, out.c_str(), true));

	// Cross-element pairing forces a fresh encode from the real PCM.
	remove(out.c_str());
	uint32_t crossArr[] = { 0, 3 };
	std::vector<uint32_t> cross(crossArr, crossArr + 2);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), cross) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), cross, out.c_str(), true));
	return 0;
}

int caseContainerStandardLayouts()
{
	ensureDir();
	std::string in = testPath("cont_std_in.caf");
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 13));

	// 3-channel standard C/L/R: built-in tag in kuki and CAF chan.
	std::string out = testPath("cont_std_out.caf");
	remove(out.c_str());
	uint32_t slotsArr[] = { 0, 1, 2 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 3);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));
	CAFData data;
	TASK_CHECK(parseCAF(out.c_str(), &data));
	TASK_CHECK(data.hasChan && data.chanTag == kStdTags[2]);
	TASK_CHECK(data.cookie.size() == sizeof(ALACSpecificConfig) + 24);

	// Non-standard C/R/L: descriptions only, no kuki chan atom.
	remove(out.c_str());
	uint32_t nonStdArr[] = { 0, 2, 1 };
	std::vector<uint32_t> nonStd(nonStdArr, nonStdArr + 3);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), nonStd) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), nonStd, out.c_str(), true));
	TASK_CHECK(parseCAF(out.c_str(), &data));
	TASK_CHECK(data.hasChan && data.chanTag == 0 && data.chanDescCount == 3);
	TASK_CHECK(data.cookie.size() == sizeof(ALACSpecificConfig));
	TASK_CHECK(data.chanLabels.size() == 3 &&
	           data.chanLabels[0] == 3 && data.chanLabels[1] == 2 && data.chanLabels[2] == 1);
	return 0;
}

int caseFailureDescribedInput()
{
	ensureDir();
	std::string base = testPath("fail_desc_base.caf");
	TASK_CHECK(buildEncodedSource(base.c_str(), 16, 6, 1, 15));

	// The initial supported input set requires built-in layout tags, so a
	// description-based 'chan' chunk is rejected (the output path can still
	// write descriptions for non-standard results).
	std::vector<uint32_t> labels(kStdLabels[5], kStdLabels[5] + 6);
	std::string in = testPath("fail_desc_in.caf");
	TASK_CHECK(setChanDescriptions(base.c_str(), in.c_str(), labels));

	std::string out = testPath("fail_desc_out.caf");
	remove(out.c_str());
	TASK_CHECK(runProjection(in.c_str(), out.c_str(), "1,2") != 0);
	TASK_CHECK(!fileExists(out.c_str()));
	return 0;
}

int caseContainerPaktAndTail()
{
	ensureDir();
	std::string in = testPath("cont_tail_in.caf");
	std::string out = testPath("cont_tail_out.caf");
	remove(out.c_str());
	// Two full packets plus a real short tail.
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 2, 100));
	uint32_t slotsArr[] = { 1, 2, 5 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 3);
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));

	CAFData source, data;
	TASK_CHECK(parseCAF(in.c_str(), &source));
	TASK_CHECK(parseCAF(out.c_str(), &data));
	TASK_CHECK(source.numberPackets == 3 && data.numberPackets == 3);
	TASK_CHECK(data.validFrames == (int64_t)2 * kFramesPerPacket + 100);
	// Last packet's real frame count must stay short.
	TASK_CHECK(packetFramesAt(data, 2) == 100);
	TASK_CHECK(data.packetSizes[2] < data.packetSizes[0]);
	// Sum of pakt entries closes over the data chunk.
	int64_t sum = 0;
	for (size_t i = 0; i < data.packetSizes.size(); ++i)
		sum += data.packetSizes[i];
	TASK_CHECK(sum == (int64_t)data.data.size());
	return 0;
}

int caseContainerCookieOrders()
{
	ensureDir();
	std::string in = testPath("cont_cookie_in.caf");
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 17));

	// The encoder's GetMagicCookie writes the layout tag in host byte order.
	std::string hostOrder = testPath("cont_cookie_host.caf");
	std::string bigOrder = testPath("cont_cookie_big.caf");
	TASK_CHECK(swapCookieChanTag(in.c_str(), bigOrder.c_str()));
	// Both encodings must be read as the same built-in tag.
	std::string out = testPath("cont_cookie_out.caf");
	uint32_t slotsArr[] = { 1, 2 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 2);
	remove(out.c_str());
	TASK_CHECK(RunChannelProjection(in.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));
	remove(out.c_str());
	TASK_CHECK(RunChannelProjection(bigOrder.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));

	// New output always writes the layout tag big-endian.  Use a 3-channel
	// standard (C/L/R) output so the optional kuki chan atom is present.
	std::string out3 = testPath("cont_cookie_out3.caf");
	uint32_t threeArr[] = { 0, 1, 2 };
	std::vector<uint32_t> three(threeArr, threeArr + 3);
	remove(out3.c_str());
	TASK_CHECK(RunChannelProjection(in.c_str(), out3.c_str(), three) == 0);
	TASK_CHECK(verifyProjection(in.c_str(), three, out3.c_str(), true));
	CAFData data;
	TASK_CHECK(parseCAF(out3.c_str(), &data));
	Cfg cfg;
	TASK_CHECK(parseCookieConfig(data.cookie, &cfg));
	TASK_CHECK(cfg.hasChanTag && cfg.chanTag == kStdTags[2]);
	// Verify the raw cookie bytes are the big-endian tag.
	size_t atom = sizeof(ALACSpecificConfig);
	TASK_CHECK(data.cookie.size() >= atom + 24);
	TASK_CHECK(getBE32(&data.cookie[atom + 12]) == kStdTags[2]);
	return 0;
}

int caseContainerCLI()
{
	ensureDir();
	std::string in = testPath("cont_cli_in.caf");
	std::string out = testPath("cont_cli_out.caf");
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 3));
	remove(out.c_str());
	TASK_CHECK(runCLIProjection(in.c_str(), out.c_str(), "0,2,1") == 0);
	uint32_t slotsArr[] = { 0, 2, 1 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 3);
	TASK_CHECK(verifyProjection(in.c_str(), slots, out.c_str(), true));

	// The legacy two-filename form still works.
	std::string returned = testPath("cont_cli_returned.caf");
	remove(returned.c_str());
	std::string command = std::string(".task-build/alacconvert \"") + in + "\" \"" +
	                      returned + "\" >/dev/null 2>&1";
#if defined(_WIN32)
	int rc = system(command.c_str());
	TASK_CHECK(rc == 0);
#else
	int rc = system(command.c_str());
	TASK_CHECK(rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0);
#endif
	TASK_CHECK(fileExists(returned.c_str()));
	return 0;
}

int caseFailureListParsing()
{
	std::vector<uint32_t> slots;
	const char *error = NULL;
	TASK_CHECK(ParseProjectSlots("1,1", &slots, &error) != 0);
	TASK_CHECK(ParseProjectSlots("0,", &slots, &error) != 0);
	TASK_CHECK(ParseProjectSlots("", &slots, &error) != 0);
	TASK_CHECK(ParseProjectSlots("0,8", &slots, &error) != 0);
	TASK_CHECK(ParseProjectSlots("0,1,2,3,4,5,6,7,0", &slots, &error) != 0);
	TASK_CHECK(ParseProjectSlots("0,2,1", &slots, &error) == 0 && slots.size() == 3);
	return 0;
}

int caseFailureRange()
{
	ensureDir();
	std::string in = testPath("fail_range_in.caf");
	std::string out = testPath("fail_range_out.caf");
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 4));
	remove(out.c_str());
	TASK_CHECK(runProjection(in.c_str(), out.c_str(), "0,6") != 0);
	TASK_CHECK(!fileExists(out.c_str()));
	TASK_CHECK(runProjection(in.c_str(), out.c_str(), "0,7") != 0);
	TASK_CHECK(!fileExists(out.c_str()));
	return 0;
}

int caseFailureLayoutConflict()
{
	ensureDir();
	std::string in = testPath("fail_layout_in.caf");
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 6));

	// Rewrite the chan chunk as descriptions that disagree with the cookie.
	std::vector<uint32_t> labels;
	labels.push_back(3); labels.push_back(1); labels.push_back(2);
	labels.push_back(5); labels.push_back(6); labels.push_back(3);
	std::vector<uint8_t> replacement;
	putBE32(&replacement, 0);
	putBE32(&replacement, 0);
	putBE32(&replacement, (uint32_t)labels.size());
	for (size_t i = 0; i < labels.size(); ++i)
	{
		putBE32(&replacement, labels[i]);
		putBE32(&replacement, 0);
		putBE32(&replacement, 0);
		putBE32(&replacement, 0);
		putBE32(&replacement, 0);
	}
	std::string conflict = testPath("fail_layout_conflict.caf");
	TASK_CHECK(mutateChunk(in.c_str(), conflict.c_str(), "chan", replacement));

	std::string out = testPath("fail_layout_out.caf");
	remove(out.c_str());
	TASK_CHECK(runProjection(conflict.c_str(), out.c_str(), "1,2") != 0);
	TASK_CHECK(!fileExists(out.c_str()));
	return 0;
}

int caseFailureDiscardedChannelCorrupt()
{
	ensureDir();
	std::string in = testPath("fail_discard_in.caf");
	std::string corrupt = testPath("fail_discard_corrupt.caf");
	// A structurally complete packet whose first element (channel 0, which a
	// --project=1,2 run discards) has invalid element syntax: its 12 "unused
	// header" bits are non-zero.  All element sizes and the single ID_END are
	// intact, so only full-source validation can catch this.
	TASK_CHECK(buildUncompressedSourceEx(corrupt.c_str(), 16, 6, 32, true));

	std::string out = testPath("fail_discard_out.caf");
	remove(out.c_str());
	TASK_CHECK(runProjection(corrupt.c_str(), out.c_str(), "1,2") != 0);
	TASK_CHECK(!fileExists(out.c_str()));

	// The same packet without the invalid header projects successfully.
	std::string good = testPath("fail_discard_good.caf");
	TASK_CHECK(buildUncompressedSource(good.c_str(), 16, 6, 32));
	uint32_t slotsArr[] = { 1, 2 };
	std::vector<uint32_t> slots(slotsArr, slotsArr + 2);
	TASK_CHECK(RunChannelProjection(good.c_str(), out.c_str(), slots) == 0);
	TASK_CHECK(verifyProjection(good.c_str(), slots, out.c_str(), true));
	return 0;
}

int caseFailureFrameMismatch()
{
	ensureDir();
	std::string in = testPath("fail_frames_in.caf");
	std::string bad = testPath("fail_frames_bad.caf");
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 0));
	TASK_CHECK(setPaktValidFrames(in.c_str(), bad.c_str(), 4000));

	std::string out = testPath("fail_frames_out.caf");
	remove(out.c_str());
	TASK_CHECK(runProjection(bad.c_str(), out.c_str(), "1,2") != 0);
	TASK_CHECK(!fileExists(out.c_str()));
	return 0;
}

int caseFailurePaktOpen()
{
	ensureDir();
	std::string in = testPath("fail_pakt_in.caf");
	std::string bad = testPath("fail_pakt_bad.caf");
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 12));
	TASK_CHECK(growDataChunk(in.c_str(), bad.c_str()));

	std::string out = testPath("fail_pakt_out.caf");
	remove(out.c_str());
	TASK_CHECK(runProjection(bad.c_str(), out.c_str(), "1,2") != 0);
	TASK_CHECK(!fileExists(out.c_str()));
	return 0;
}

int caseFailureUnsupportedElement()
{
	ensureDir();
	std::string base = testPath("fail_elem_base.caf");
	TASK_CHECK(buildEncodedSource(base.c_str(), 16, 6, 1, 1));
	CAFData data;
	TASK_CHECK(parseCAF(base.c_str(), &data));

	// Build a one-packet CAF whose packet begins with an unsupported element.
	std::vector<uint8_t> packet;
	putBE32(&packet, 0x80000000u); // ID_DSE tag (100) followed by arbitrary bits
	packet.resize(16, 0);
	std::vector<std::vector<uint8_t> > packets;
	packets.push_back(packet);
	std::string bad = testPath("fail_elem_bad.caf");
	TASK_CHECK(writeCAFFile(bad.c_str(), 16, 6, packets, 1, data.cookie,
	                        true, kStdTags[5], NULL));

	std::string out = testPath("fail_elem_out.caf");
	remove(out.c_str());
	TASK_CHECK(runProjection(bad.c_str(), out.c_str(), "1,2") != 0);
	TASK_CHECK(!fileExists(out.c_str()));
	return 0;
}

int caseFailureOutputAndInput()
{
	ensureDir();
	std::string in = testPath("fail_same_in.caf");
	TASK_CHECK(buildEncodedSource(in.c_str(), 16, 6, 1, 2));

	// Same path for input and output is rejected; the input stays intact.
	std::vector<uint8_t> before, after;
	TASK_CHECK(readAll(in.c_str(), &before));
	TASK_CHECK(runProjection(in.c_str(), in.c_str(), "1,2") != 0);
	TASK_CHECK(readAll(in.c_str(), &after));
	TASK_CHECK(before == after);

	// WAVE output is not part of projection mode.
	std::string wave = testPath("fail_same_out.wav");
	remove(wave.c_str());
	TASK_CHECK(runProjection(in.c_str(), wave.c_str(), "1,2") != 0);
	TASK_CHECK(!fileExists(wave.c_str()));

	// A non-ALAC CAF input is rejected.
	std::string lpcm = testPath("fail_same_lpcm.caf");
	std::vector<uint8_t> d;
	TASK_CHECK(readAll(in.c_str(), &d));
	std::vector<ChunkRef> chunks = chunkList(d);
	for (size_t i = 0; i < chunks.size(); ++i)
	{
		if (getBE32(&d[chunks[i].header]) != fourcc("desc"))
			continue;
		size_t at = chunks[i].data + 8;
		d[at] = 'l'; d[at + 1] = 'p'; d[at + 2] = 'c'; d[at + 3] = 'm';
		break;
	}
	TASK_CHECK(writeAll(lpcm.c_str(), d));
	std::string out = testPath("fail_same_out.caf");
	remove(out.c_str());
	TASK_CHECK(runProjection(lpcm.c_str(), out.c_str(), "1,2") != 0);
	TASK_CHECK(!fileExists(out.c_str()));
	return 0;
}

const struct task_case kCases[] =
{
	{ "reuse", "whole_cpe", caseReuseWholeCPE },
	{ "reuse", "whole_elements_reordered", caseReuseWholeElementsReordered },
	{ "reuse", "24bit", caseReuse24Bit },
	{ "reuse", "lfe", caseReuseLFE },
	{ "repair", "split_cpe", caseRepairSplitCPE },
	{ "repair", "cross_element_pair", caseRepairCrossElementPair },
	{ "repair", "swap_pair", caseRepairSwapPair },
	{ "repair", "uncompressed_elements", caseRepairUncompressed },
	{ "container", "standard_and_nonstandard", caseContainerStandardLayouts },
	{ "container", "pakt_and_short_tail", caseContainerPaktAndTail },
	{ "container", "cookie_byte_orders", caseContainerCookieOrders },
	{ "container", "cli_and_compatibility", caseContainerCLI },
	{ "failure", "list_parsing", caseFailureListParsing },
	{ "failure", "out_of_range", caseFailureRange },
	{ "failure", "layout_conflict", caseFailureLayoutConflict },
	{ "failure", "described_input", caseFailureDescribedInput },
	{ "failure", "discarded_channel_corrupt", caseFailureDiscardedChannelCorrupt },
	{ "failure", "frame_count_mismatch", caseFailureFrameMismatch },
	{ "failure", "pakt_not_closed", caseFailurePaktOpen },
	{ "failure", "unsupported_element", caseFailureUnsupportedElement },
	{ "failure", "output_and_input", caseFailureOutputAndInput },
};

} // namespace

extern const struct task_case task_cases[];
extern const size_t task_case_count;

const struct task_case task_cases[] = {
#define TASK_CASE_ENTRY(x) x
	TASK_CASE_ENTRY(kCases[0]), TASK_CASE_ENTRY(kCases[1]),
	TASK_CASE_ENTRY(kCases[2]), TASK_CASE_ENTRY(kCases[3]),
	TASK_CASE_ENTRY(kCases[4]), TASK_CASE_ENTRY(kCases[5]),
	TASK_CASE_ENTRY(kCases[6]), TASK_CASE_ENTRY(kCases[7]),
	TASK_CASE_ENTRY(kCases[8]), TASK_CASE_ENTRY(kCases[9]),
	TASK_CASE_ENTRY(kCases[10]), TASK_CASE_ENTRY(kCases[11]),
	TASK_CASE_ENTRY(kCases[12]), TASK_CASE_ENTRY(kCases[13]),
	TASK_CASE_ENTRY(kCases[14]), TASK_CASE_ENTRY(kCases[15]),
	TASK_CASE_ENTRY(kCases[16]), TASK_CASE_ENTRY(kCases[17]),
	TASK_CASE_ENTRY(kCases[18]), TASK_CASE_ENTRY(kCases[19]),
	TASK_CASE_ENTRY(kCases[20])
#undef TASK_CASE_ENTRY
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
