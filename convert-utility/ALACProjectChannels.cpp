/*
 * ALACProjectChannels.cpp
 *
 * Channel projection ("--project") for alacconvert.
 *
 * See ALACProjectChannels.h for the feature description.  The implementation
 * walks the CAF container strictly, decodes one packet at a time, reuses
 * ALAC elements whose complete channel set survives unchanged (only
 * element_instance_tag is renumbered) and encodes the remaining output
 * elements from the real PCM of the packet.  Only the packet directory and a
 * single packet of PCM / bitstream are held in memory.
 */

#include "ALACProjectChannels.h"
#include "CAFFileALAC.h"
#include "ALACAudioTypes.h"
#include "ALACBitUtilities.h"
#include "ALACDecoder.h"
#include "ALACEncoder.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define ALACPROJ_GETPID _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#define ALACPROJ_GETPID getpid
#endif

namespace {

const int64_t kMaxFileSize = ((int64_t)1) << 31; // < 2 GiB inputs
const uint32_t kDefaultFrameSize = kALACDefaultFramesPerPacket; // 4096
const uint32_t kMaxPacketBytes = 1024u * 1024u;

// ---------------------------------------------------------------------------
// byte helpers
// ---------------------------------------------------------------------------

void SetError(std::string *error, const char *message)
{
	if (error)
		*error = message;
}

uint16_t ReadBE16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }

uint32_t ReadBE32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint64_t ReadBE64(const uint8_t *p)
{
	uint64_t hi = ReadBE32(p);
	uint64_t lo = ReadBE32(p + 4);
	return (hi << 32) | lo;
}

void PutBE16(std::vector<uint8_t> *out, uint16_t v)
{
	out->push_back((uint8_t)(v >> 8));
	out->push_back((uint8_t)v);
}

void PutBE32(std::vector<uint8_t> *out, uint32_t v)
{
	out->push_back((uint8_t)(v >> 24));
	out->push_back((uint8_t)(v >> 16));
	out->push_back((uint8_t)(v >> 8));
	out->push_back((uint8_t)v);
}

void PutBE64(std::vector<uint8_t> *out, uint64_t v)
{
	PutBE32(out, (uint32_t)(v >> 32));
	PutBE32(out, (uint32_t)v);
}

double ReadBEDouble(const uint8_t *p)
{
	uint64_t bits = ReadBE64(p);
	double value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

void PutBEDouble(std::vector<uint8_t> *out, double value)
{
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	PutBE64(out, bits);
}

bool ReadBE(FILE *file, void *buffer, size_t bytes)
{
	return bytes == 0 || fread(buffer, 1, bytes, file) == bytes;
}

bool SeekTo(FILE *file, int64_t offset)
{
	if (offset < 0)
		return false;
#if defined(_WIN32)
	return _fseeki64(file, offset, SEEK_SET) == 0;
#else
	return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

int64_t FileSize(FILE *file)
{
	int64_t current = -1;
#if defined(_WIN32)
	if (_fseeki64(file, 0, SEEK_CUR) != 0) return -1;
	current = _ftelli64(file);
	if (_fseeki64(file, 0, SEEK_END) != 0) return -1;
	int64_t size = _ftelli64(file);
	_fseeki64(file, current, SEEK_SET);
#else
	off_t cur = ftello(file);
	if (cur < 0) return -1;
	current = (int64_t)cur;
	if (fseeko(file, 0, SEEK_END) != 0) return -1;
	off_t end = ftello(file);
	if (end < 0) return -1;
	int64_t size = (int64_t)end;
	fseeko(file, (off_t)current, SEEK_SET);
#endif
	return size;
}

// ---------------------------------------------------------------------------
// Core Audio channel layout constants
// ---------------------------------------------------------------------------

const uint32_t kAudioChannelLayoutTag_UseChannelDescriptions = 0;
const uint32_t kAudioChannelLayoutTag_UseChannelBitmap = (1u << 16) | 0;

const uint32_t kChannelLabelLeft = 1;
const uint32_t kChannelLabelRight = 2;
const uint32_t kChannelLabelCenter = 3;
const uint32_t kChannelLabelLFEScreen = 4;
const uint32_t kChannelLabelLeftSurround = 5;
const uint32_t kChannelLabelRightSurround = 6;
const uint32_t kChannelLabelLeftCenter = 7;
const uint32_t kChannelLabelRightCenter = 8;
const uint32_t kChannelLabelCenterSurround = 9;

const uint32_t kStandardLayoutTags[kALACMaxChannels] =
{
	kALACChannelLayoutTag_Mono,
	kALACChannelLayoutTag_Stereo,
	kALACChannelLayoutTag_MPEG_3_0_B,
	kALACChannelLayoutTag_MPEG_4_0_B,
	kALACChannelLayoutTag_MPEG_5_0_D,
	kALACChannelLayoutTag_MPEG_5_1_D,
	kALACChannelLayoutTag_AAC_6_1,
	kALACChannelLayoutTag_MPEG_7_1_B
};

// Channel labels in ALAC bitstream channel order for each standard layout.
const uint32_t kStandardLayoutLabels[kALACMaxChannels][kALACMaxChannels] =
{
	{ kChannelLabelCenter },
	{ kChannelLabelLeft, kChannelLabelRight },
	{ kChannelLabelCenter, kChannelLabelLeft, kChannelLabelRight },
	{ kChannelLabelCenter, kChannelLabelLeft, kChannelLabelRight,
	  kChannelLabelCenterSurround },
	{ kChannelLabelCenter, kChannelLabelLeft, kChannelLabelRight,
	  kChannelLabelLeftSurround, kChannelLabelRightSurround },
	{ kChannelLabelCenter, kChannelLabelLeft, kChannelLabelRight,
	  kChannelLabelLeftSurround, kChannelLabelRightSurround, kChannelLabelLFEScreen },
	{ kChannelLabelCenter, kChannelLabelLeft, kChannelLabelRight,
	  kChannelLabelLeftSurround, kChannelLabelRightSurround,
	  kChannelLabelCenterSurround, kChannelLabelLFEScreen },
	{ kChannelLabelCenter, kChannelLabelLeftCenter, kChannelLabelRightCenter,
	  kChannelLabelLeft, kChannelLabelRight, kChannelLabelLeftSurround,
	  kChannelLabelRightSurround, kChannelLabelLFEScreen }
};

bool TagToStandardChannelCount(uint32_t tag, uint32_t *numChannels)
{
	for (uint32_t i = 0; i < kALACMaxChannels; ++i)
	{
		if (kStandardLayoutTags[i] == tag)
		{
			*numChannels = i + 1;
			return true;
		}
	}
	return false;
}

bool LabelsAreStandard(const std::vector<uint32_t> &labels)
{
	uint32_t count = (uint32_t)labels.size();
	if (count < 1 || count > kALACMaxChannels)
		return false;
	for (uint32_t i = 0; i < count; ++i)
	{
		if (labels[i] != kStandardLayoutLabels[count - 1][i])
			return false;
	}
	return true;
}

uint32_t StandardTagForLabels(const std::vector<uint32_t> &labels)
{
	return kStandardLayoutTags[labels.size() - 1];
}

uint32_t SwapLayoutTag(uint32_t tag)
{
	return ((tag & 0x000000FFu) << 24) |
	       ((tag & 0x0000FF00u) << 8) |
	       ((tag >> 8) & 0x0000FF00u) |
	       ((tag >> 24) & 0x000000FFu);
}

// ---------------------------------------------------------------------------
// CAF container model
// ---------------------------------------------------------------------------

struct CAFChannelDescription
{
	uint32_t label;
	uint32_t flags;
	float coordinates[3];
};

struct CAFLayout
{
	uint32_t tag;
	uint32_t bitmap;
	std::vector<CAFChannelDescription> descriptions;
	CAFLayout() : tag(0), bitmap(0) {}
};

struct CAFFile
{
	int64_t fileSize;

	// 'desc'
	bool hasDesc;
	double sampleRate;
	uint32_t formatID;
	uint32_t formatFlags;
	uint32_t bytesPerPacket;
	uint32_t framesPerPacket;
	uint32_t channelsPerFrame;
	uint32_t bitsPerChannel;

	// 'pakt'
	bool hasPakt;
	int64_t numberPackets;
	int64_t numberValidFrames;
	int32_t primingFrames;
	int32_t remainderFrames;
	int64_t packetsStartOffset;
	std::vector<uint32_t> packetSizes;
	std::vector<uint32_t> packetFrames;

	// 'data'
	bool hasData;
	int64_t dataOffset;
	int64_t dataSize;

	// 'chan'
	bool hasChan;
	CAFLayout chanLayout;

	// 'kuki'
	bool hasKuki;
	std::vector<uint8_t> cookie;

	CAFFile()
	    : fileSize(0), hasDesc(false), sampleRate(0), formatID(0), formatFlags(0),
	      bytesPerPacket(0), framesPerPacket(0), channelsPerFrame(0),
	      bitsPerChannel(0), hasPakt(false), numberPackets(0),
	      numberValidFrames(0), primingFrames(0), remainderFrames(0),
	      packetsStartOffset(0), hasData(false), dataOffset(0), dataSize(0),
	      hasChan(false), hasKuki(false)
	{
	}
};

const int64_t kCAFFileHeaderSize = 8;
const int64_t kCAFChunkHeaderSize = 12;
const int64_t kCAFPacketTableHeaderSize = 24;

bool ReadBER32(const uint8_t *data, size_t available, size_t *used, uint32_t *value)
{
	uint32_t result = 0;
	size_t count = 0;
	while (count < 5)
	{
		if (count >= available)
			return false;
		uint8_t byte = data[count++];
		if (count == 5 && (byte & 0x80) != 0)
			return false;
		result = (result << 7) | (byte & 0x7F);
		if ((byte & 0x80) == 0)
		{
			*used = count;
			*value = result;
			return true;
		}
	}
	return false;
}

void PutBER32(std::vector<uint8_t> *out, uint32_t value)
{
	uint8_t temp[5];
	int32_t count = 0;
	uint32_t v = value;
	do
	{
		temp[count++] = (uint8_t)(v & 0x7F);
		v >>= 7;
	} while (v != 0);
	for (int32_t i = count - 1; i >= 0; --i)
	{
		uint8_t byte = temp[i];
		if (i != 0)
			byte |= 0x80;
		out->push_back(byte);
	}
}

bool ParseContainer(FILE *file, CAFFile *caf, std::string *error)
{
	int64_t size = FileSize(file);
	if (size < 0 || size > kMaxFileSize)
	{
		SetError(error, "input file is too large or cannot be measured");
		return false;
	}
	caf->fileSize = size;

	if (!SeekTo(file, 0))
	{
		SetError(error, "cannot seek input file");
		return false;
	}
	uint8_t header[kCAFFileHeaderSize];
	if (!ReadBE(file, header, sizeof(header)))
	{
		SetError(error, "input is too small to be a CAF file");
		return false;
	}
	if (memcmp(header, "caff", 4) != 0)
	{
		SetError(error, "input is not a CAF file");
		return false;
	}
	if (ReadBE16(header + 4) != k_port_CAF_FileVersion_Initial)
	{
		SetError(error, "unsupported CAF file version");
		return false;
	}

	int64_t pos = kCAFFileHeaderSize;
	int descCount = 0, dataCount = 0, paktCount = 0, chanCount = 0, kukiCount = 0;

	while (pos < size)
	{
		if (pos + kCAFChunkHeaderSize > size)
		{
			SetError(error, "truncated CAF chunk header");
			return false;
		}
		uint8_t chunkHeader[kCAFChunkHeaderSize];
		if (!SeekTo(file, pos) || !ReadBE(file, chunkHeader, sizeof(chunkHeader)))
		{
			SetError(error, "cannot read CAF chunk header");
			return false;
		}
		uint32_t type = ReadBE32(chunkHeader);
		int64_t declared = (int64_t)ReadBE64(chunkHeader + 4);
		int64_t dataPos = pos + kCAFChunkHeaderSize;
		int64_t available = size - dataPos;

		if (declared < 0)
		{
			SetError(error, "CAF chunk declares an unspecified size");
			return false;
		}
		if (declared > available)
		{
			SetError(error, "CAF chunk size exceeds the file size");
			return false;
		}

		if (type == k_port_CAF_StreamDescriptionChunkID)
		{
			++descCount;
			if (declared < (int64_t)sizeof(port_CAFAudioDescription))
			{
				SetError(error, "CAF desc chunk is too small");
				return false;
			}
			uint8_t desc[sizeof(port_CAFAudioDescription)];
			if (!SeekTo(file, dataPos) || !ReadBE(file, desc, sizeof(desc)))
			{
				SetError(error, "cannot read CAF desc chunk");
				return false;
			}
			caf->sampleRate = ReadBEDouble(desc);
			caf->formatID = ReadBE32(desc + 8);
			caf->formatFlags = ReadBE32(desc + 12);
			caf->bytesPerPacket = ReadBE32(desc + 16);
			caf->framesPerPacket = ReadBE32(desc + 20);
			caf->channelsPerFrame = ReadBE32(desc + 24);
			caf->bitsPerChannel = ReadBE32(desc + 28);
			caf->hasDesc = true;
		}
		else if (type == k_port_CAF_AudioDataChunkID)
		{
			++dataCount;
			if (declared < 4)
			{
				SetError(error, "CAF data chunk is smaller than its edit count");
				return false;
			}
			caf->dataOffset = dataPos + 4;
			caf->dataSize = declared - 4;
			caf->hasData = true;
		}
		else if (type == k_port_CAF_PacketTableChunkID)
		{
			++paktCount;
			if (declared < kCAFPacketTableHeaderSize)
			{
				SetError(error, "CAF pakt chunk is too small");
				return false;
			}
			if (!caf->hasDesc)
			{
				SetError(error, "CAF desc chunk must precede the pakt chunk");
				return false;
			}
			std::vector<uint8_t> pakt((size_t)declared);
			if (!SeekTo(file, dataPos) || !ReadBE(file, &pakt[0], pakt.size()))
			{
				SetError(error, "cannot read CAF pakt chunk");
				return false;
			}
			caf->numberPackets = (int64_t)ReadBE64(&pakt[0]);
			caf->numberValidFrames = (int64_t)ReadBE64(&pakt[8]);
			caf->primingFrames = (int32_t)ReadBE32(&pakt[16]);
			caf->remainderFrames = (int32_t)ReadBE32(&pakt[20]);
			caf->hasPakt = true;

			if (caf->numberPackets < 0 || caf->numberPackets > size ||
			    caf->numberValidFrames < 0)
			{
				SetError(error, "CAF pakt header contains invalid counts");
				return false;
			}

			// The projection path requires the canonical ALAC form: variable
			// packet byte sizes with a constant frame count per packet.
			bool needBytes = (caf->bytesPerPacket == 0);
			bool needFrames = (caf->framesPerPacket == 0);
			if (!needBytes || needFrames)
			{
				SetError(error, "unsupported CAF pakt entry form");
				return false;
			}

			size_t tablePos = kCAFPacketTableHeaderSize;
			size_t tableEnd = pakt.size();
			caf->packetSizes.reserve((size_t)caf->numberPackets);
			for (int64_t i = 0; i < caf->numberPackets; ++i)
			{
				uint32_t bytes = 0;
				size_t used = 0;
				if (!ReadBER32(&pakt[tablePos], tableEnd - tablePos, &used, &bytes))
				{
					SetError(error, "truncated CAF pakt byte entry");
					return false;
				}
				tablePos += used;
				caf->packetSizes.push_back(bytes);
			}

			// Frame counts are implicit: every packet is full length except a
			// possibly short final packet.
			int64_t full = caf->framesPerPacket;
			int64_t remaining = caf->numberValidFrames;
			for (int64_t i = 0; i < caf->numberPackets; ++i)
			{
				int64_t frames = (remaining >= full) ? full : remaining;
				caf->packetFrames.push_back((uint32_t)frames);
				remaining -= frames;
			}
			if (remaining != 0)
			{
				SetError(error, "packet table frame counts do not match the valid frame count");
				return false;
			}
		}
		else if (type == k_port_CAF_ChannelLayoutChunkID)
		{
			++chanCount;
			if (declared < 12)
			{
				SetError(error, "CAF chan chunk is too small");
				return false;
			}
			std::vector<uint8_t> chan((size_t)declared);
			if (!SeekTo(file, dataPos) || !ReadBE(file, &chan[0], chan.size()))
			{
				SetError(error, "cannot read CAF chan chunk");
				return false;
			}
			caf->chanLayout.tag = ReadBE32(&chan[0]);
			caf->chanLayout.bitmap = ReadBE32(&chan[4]);
			uint32_t count = ReadBE32(&chan[8]);
			if (count > kALACMaxChannels)
			{
				SetError(error, "CAF chan chunk declares too many descriptions");
				return false;
			}
			if ((uint64_t)declared < 12 + (uint64_t)count * 20)
			{
				SetError(error, "CAF chan chunk is truncated");
				return false;
			}
			caf->chanLayout.descriptions.clear();
			for (uint32_t i = 0; i < count; ++i)
			{
				const uint8_t *d = &chan[12 + i * 20];
				CAFChannelDescription desc;
				desc.label = ReadBE32(d);
				desc.flags = ReadBE32(d + 4);
				for (int c = 0; c < 3; ++c)
				{
					uint32_t raw = ReadBE32(d + 8 + c * 4);
					memcpy(&desc.coordinates[c], &raw, sizeof(float));
				}
				caf->chanLayout.descriptions.push_back(desc);
			}
			caf->hasChan = true;
		}
		else if (type == k_port_CAF_MagicCookieID)
		{
			++kukiCount;
			caf->cookie.resize((size_t)declared);
			if (declared > 0 && (!SeekTo(file, dataPos) ||
			                     !ReadBE(file, &caf->cookie[0], caf->cookie.size())))
			{
				SetError(error, "cannot read CAF kuki chunk");
				return false;
			}
			caf->hasKuki = true;
		}

		pos = dataPos + declared;
	}

	if (descCount != 1 || dataCount != 1 || paktCount != 1 || kukiCount != 1 || chanCount > 1)
	{
		SetError(error, "CAF file must contain exactly one desc, pakt, kuki and data chunk");
		return false;
	}
	if (!caf->hasDesc || !caf->hasData || !caf->hasPakt || !caf->hasKuki)
	{
		SetError(error, "CAF file is missing a required chunk");
		return false;
	}
	caf->packetsStartOffset = caf->dataOffset;
	return true;
}

// Parses the ALACSpecificConfig and the optional 'chan' atom in the kuki.
bool ParseCookie(const std::vector<uint8_t> &cookie, ALACSpecificConfig *config,
                 bool *hasChanTag, uint32_t *chanTag, std::string *error)
{
	memset(config, 0, sizeof(*config));
	*hasChanTag = false;
	*chanTag = 0;

	size_t offset = 0;
	if (cookie.size() >= 12 && memcmp(&cookie[4], "frma", 4) == 0)
		offset += 12;
	if (cookie.size() >= offset + 12 && memcmp(&cookie[offset + 4], "alac", 4) == 0)
		offset += 12;

	if (cookie.size() < offset + sizeof(ALACSpecificConfig))
	{
		SetError(error, "ALAC magic cookie is too small");
		return false;
	}

	const uint8_t *p = &cookie[offset];
	config->frameLength = ReadBE32(p);
	config->compatibleVersion = p[4];
	config->bitDepth = p[5];
	config->pb = p[6];
	config->mb = p[7];
	config->kb = p[8];
	config->numChannels = p[9];
	config->maxRun = ReadBE16(p + 10);
	config->maxFrameBytes = ReadBE32(p + 12);
	config->avgBitRate = ReadBE32(p + 16);
	config->sampleRate = ReadBE32(p + 20);

	// Optional 'chan' atom immediately after the config.
	size_t atomOffset = offset + sizeof(ALACSpecificConfig);
	if (cookie.size() >= atomOffset + 12 &&
	    memcmp(&cookie[atomOffset + 4], "chan", 4) == 0)
	{
		uint32_t atomSize = ReadBE32(&cookie[atomOffset]);
		if (atomSize < 12 || atomOffset + atomSize > cookie.size() ||
		    atomSize < 24)
		{
			SetError(error, "ALAC cookie 'chan' atom is malformed");
			return false;
		}
		uint32_t layoutTag = ReadBE32(&cookie[atomOffset + 12]);
		uint32_t channels = 0;
		if (!TagToStandardChannelCount(layoutTag, &channels))
		{
			uint32_t swapped = SwapLayoutTag(layoutTag);
			if (TagToStandardChannelCount(swapped, &channels))
				layoutTag = swapped;
			else
			{
				SetError(error, "ALAC cookie 'chan' atom has an unknown layout tag");
				return false;
			}
		}
		*hasChanTag = true;
		*chanTag = layoutTag;
	}
	return true;
}

// Derives the channel roles (Core Audio labels) in ALAC bitstream order.
bool DeriveInputLabels(const CAFFile &caf, const ALACSpecificConfig &config,
                       bool cookieHasTag, uint32_t cookieTag,
                       std::vector<uint32_t> *labels, std::string *error)
{
	uint32_t channels = caf.channelsPerFrame;
	labels->clear();

	bool haveChanLabels = false;
	std::vector<uint32_t> chanLabels;

	if (caf.hasChan)
	{
		const CAFLayout &layout = caf.chanLayout;
		// Input roles are defined by the CAF 'chan' built-in tag (or the ALAC
		// cookie tag when there is no 'chan' chunk).  Description-based input
		// layouts are outside this initial supported set.
		if (layout.tag == kAudioChannelLayoutTag_UseChannelDescriptions ||
		    layout.tag == kAudioChannelLayoutTag_UseChannelBitmap)
		{
			SetError(error, "CAF chan chunk must use a built-in layout tag");
			return false;
		}
		uint32_t tagChannels = 0;
		if (!TagToStandardChannelCount(layout.tag, &tagChannels))
		{
			SetError(error, "CAF chan chunk has an unsupported layout tag");
			return false;
		}
		if (tagChannels != channels)
		{
			SetError(error, "CAF chan layout tag does not match the channel count");
			return false;
		}
		chanLabels.assign(kStandardLayoutLabels[tagChannels - 1],
		                  kStandardLayoutLabels[tagChannels - 1] + tagChannels);
		haveChanLabels = true;
	}

	bool haveCookieLabels = false;
	std::vector<uint32_t> cookieLabels;
	if (cookieHasTag)
	{
		uint32_t tagChannels = 0;
		if (!TagToStandardChannelCount(cookieTag, &tagChannels))
		{
			SetError(error, "ALAC cookie has an unsupported layout tag");
			return false;
		}
		if (tagChannels != channels)
		{
			SetError(error, "ALAC cookie layout tag does not match the channel count");
			return false;
		}
		cookieLabels.assign(kStandardLayoutLabels[tagChannels - 1],
		                    kStandardLayoutLabels[tagChannels - 1] + tagChannels);
		haveCookieLabels = true;
	}

	if (haveChanLabels && haveCookieLabels)
	{
		if (chanLabels != cookieLabels)
		{
			SetError(error, "CAF chan chunk and ALAC cookie disagree on the layout");
			return false;
		}
	}
	if (haveChanLabels)
		*labels = chanLabels;
	else if (haveCookieLabels)
		*labels = cookieLabels;
	else
	{
		SetError(error, "input has no recognisable built-in channel layout");
		return false;
	}

	if ((uint32_t)labels->size() != channels)
	{
		SetError(error, "channel layout does not match the channel count");
		return false;
	}
	(void)config;
	return true;
}

// ---------------------------------------------------------------------------
// projection helpers
// ---------------------------------------------------------------------------

struct OutputElement
{
	uint32_t tag;          // ID_SCE / ID_CPE / ID_LFE
	uint32_t outStart;     // first output channel
	uint32_t channelCount; // 1 or 2
	uint32_t instanceTag;
	bool reuse;
	uint32_t sourceTraceIndex;
	uint32_t sourceStartBit; // when reusing
	uint32_t sourceEndBit;   // when reusing
};

bool BuildOutputElements(uint32_t outChannels, const std::vector<uint32_t> &slots,
                         const std::vector<ALACElementTrace> &traces,
                         std::vector<OutputElement> *elements,
                         std::string *error)
{
	uint32_t map = 0;
	if (!ALACGetChannelElementMap(outChannels, &map))
	{
		SetError(error, "unsupported output channel count");
		return false;
	}

	uint8_t nextInstance[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	elements->clear();

	uint32_t channel = 0;
	while (channel < outChannels)
	{
		uint32_t tag = (map >> (channel * 3)) & 0x7u;
		uint32_t elementChannels = (tag == ID_CPE) ? 2 : 1;
		if (tag != ID_SCE && tag != ID_CPE && tag != ID_LFE)
		{
			SetError(error, "output element map is unsupported");
			return false;
		}
		if (channel + elementChannels > outChannels)
		{
			SetError(error, "output element map does not cover the channel count");
			return false;
		}

		OutputElement element;
		element.tag = tag;
		element.outStart = channel;
		element.channelCount = elementChannels;
		element.instanceTag = nextInstance[tag]++;
		element.reuse = false;
		element.sourceTraceIndex = 0;
		element.sourceStartBit = 0;
		element.sourceEndBit = 0;

		// A complete source element can be reused only when the output
		// element selects exactly its whole contiguous channel set in the
		// same order and with the same element type.
		for (size_t t = 0; t < traces.size(); ++t)
		{
			const ALACElementTrace &trace = traces[t];
			if (trace.tag != tag || trace.channelCount != elementChannels)
				continue;
			if (trace.channelStart != slots[channel])
				continue;
			bool exact = true;
			for (uint32_t i = 0; i < elementChannels; ++i)
			{
				if (slots[channel + i] != trace.channelStart + i)
				{
					exact = false;
					break;
				}
			}
			if (exact)
			{
				element.reuse = true;
				element.sourceTraceIndex = (uint32_t)t;
				element.sourceStartBit = trace.startBit;
				element.sourceEndBit = trace.endBit;
				break;
			}
		}

		elements->push_back(element);
		channel += elementChannels;
	}
	return true;
}

void CopyBits(BitBuffer *out, const uint8_t *source, uint32_t sourceBytes,
              uint32_t startBit, uint32_t numBits)
{
	BitBuffer in;
	BitBufferInit(&in, (uint8_t *)source, sourceBytes);
	BitBufferAdvance(&in, startBit);
	uint32_t remaining = numBits;
	while (remaining > 0)
	{
		uint32_t chunk = (remaining > 16) ? 16 : remaining;
		uint32_t value = BitBufferRead(&in, (uint8_t)chunk);
		BitBufferWrite(out, value, chunk);
		remaining -= chunk;
	}
}

uint32_t BytesPerSample(uint32_t bitDepth) { return bitDepth / 8; }

void CopySample(uint8_t *destination, const uint8_t *source, uint32_t bytes)
{
	memcpy(destination, source, bytes);
}

// ---------------------------------------------------------------------------
// file writing helpers
// ---------------------------------------------------------------------------

void WriteChunkHeader(std::vector<uint8_t> *out, uint32_t type, uint64_t size)
{
	PutBE32(out, type);
	PutBE64(out, size);
}

bool WriteAll(FILE *file, const void *data, size_t bytes)
{
	return bytes == 0 || fwrite(data, 1, bytes, file) == bytes;
}

bool CopyRange(FILE *source, FILE *destination, int64_t offset, int64_t bytes)
{
	if (!SeekTo(source, offset))
		return false;
	uint8_t buffer[65536];
	int64_t remaining = bytes;
	while (remaining > 0)
	{
		size_t chunk = (size_t)((remaining > (int64_t)sizeof(buffer)) ? (int64_t)sizeof(buffer) : remaining);
		if (fread(buffer, 1, chunk, source) != chunk)
			return false;
		if (fwrite(buffer, 1, chunk, destination) != chunk)
			return false;
		remaining -= (int64_t)chunk;
	}
	return true;
}

bool SameFile(const char *a, const char *b)
{
#if defined(_WIN32)
	return _stricmp(a, b) == 0;
#else
	struct stat sa, sb;
	if (stat(a, &sa) == 0 && stat(b, &sb) == 0)
		return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
	return strcmp(a, b) == 0;
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

int32_t ParseProjectSlots(const char *text, std::vector<uint32_t> *slots,
                          const char **error)
{
	if (error)
		*error = NULL;
	slots->clear();
	if (text == NULL || *text == '\0')
	{
		if (error) *error = "empty projection list";
		return -1;
	}

	const char *p = text;
	while (true)
	{
		if (*p < '0' || *p > '9')
		{
			if (error) *error = "projection list must contain decimal slot numbers";
			return -1;
		}
		uint32_t value = 0;
		while (*p >= '0' && *p <= '9')
		{
			value = value * 10 + (uint32_t)(*p - '0');
			if (value > 7)
			{
				if (error) *error = "projection slot is out of range";
				return -1;
			}
			++p;
		}
		for (size_t i = 0; i < slots->size(); ++i)
		{
			if ((*slots)[i] == value)
			{
				if (error) *error = "projection list contains a duplicate slot";
				return -1;
			}
		}
		slots->push_back(value);
		if (*p == '\0')
			break;
		if (*p != ',')
		{
			if (error) *error = "projection list has an unexpected character";
			return -1;
		}
		++p;
		if (*p == '\0')
		{
			if (error) *error = "projection list has a trailing comma";
			return -1;
		}
	}

	if (slots->size() < 1 || slots->size() > kALACMaxChannels)
	{
		if (error) *error = "projection list must contain between 1 and 8 slots";
		return -1;
	}
	return 0;
}

int32_t RunChannelProjection(const char *inputPath, const char *outputPath,
                             const std::vector<uint32_t> &slots)
{
	std::string error;
	FILE *input = NULL;
	FILE *payload = NULL;
	FILE *finalFile = NULL;
	std::string payloadPath;
	std::string finalPath;
	bool published = false;

	if (inputPath == NULL || outputPath == NULL)
	{
		fprintf(stderr, "projection requires an input and an output file\n");
		return -1;
	}
	if (slots.empty() || slots.size() > kALACMaxChannels)
	{
		fprintf(stderr, "projection list must contain between 1 and 8 slots\n");
		return -1;
	}

	// The projection path only accepts ALAC/CAF and never emits WAVE.
	const char *extension = strrchr(outputPath, '.');
	if (extension != NULL && strcmp(extension, ".wav") == 0)
	{
		fprintf(stderr, "projection mode only writes ALAC/CAF output\n");
		return -1;
	}
	if (SameFile(inputPath, outputPath))
	{
		fprintf(stderr, "output file must not be the input file\n");
		return -1;
	}

	input = fopen(inputPath, "rb");
	if (input == NULL)
	{
		fprintf(stderr, "cannot open input file \"%s\": %s\n", inputPath, strerror(errno));
		return -1;
	}

	CAFFile caf;
	if (!ParseContainer(input, &caf, &error))
	{
		fprintf(stderr, "%s\n", error.c_str());
		fclose(input);
		return -1;
	}

	// ---- validate the input format ----
	if (caf.formatID != kALACFormatAppleLossless)
	{
		fprintf(stderr, "projection input must be ALAC\n");
		fclose(input);
		return -1;
	}
	if (caf.channelsPerFrame < 3 || caf.channelsPerFrame > kALACMaxChannels)
	{
		fprintf(stderr, "projection input must have between 3 and 8 channels\n");
		fclose(input);
		return -1;
	}
	if (caf.framesPerPacket != kDefaultFrameSize)
	{
		fprintf(stderr, "projection input must use 4096 frames per packet\n");
		fclose(input);
		return -1;
	}
	if (caf.bytesPerPacket != 0)
	{
		fprintf(stderr, "projection input must have a variable packet byte size\n");
		fclose(input);
		return -1;
	}
	if (caf.primingFrames != 0 || caf.remainderFrames != 0)
	{
		fprintf(stderr, "projection input must have zero priming and remainder frames\n");
		fclose(input);
		return -1;
	}
	if (caf.sampleRate <= 0 || caf.sampleRate > 4294967295.0)
	{
		fprintf(stderr, "projection input has an unsupported sample rate\n");
		fclose(input);
		return -1;
	}

	ALACSpecificConfig config;
	bool cookieHasTag = false;
	uint32_t cookieTag = 0;
	if (!ParseCookie(caf.cookie, &config, &cookieHasTag, &cookieTag, &error))
	{
		fprintf(stderr, "%s\n", error.c_str());
		fclose(input);
		return -1;
	}
	if (config.compatibleVersion > kALACVersion)
	{
		fprintf(stderr, "ALAC magic cookie has an unsupported version\n");
		fclose(input);
		return -1;
	}
	if (config.numChannels != caf.channelsPerFrame)
	{
		fprintf(stderr, "ALAC cookie channel count does not match desc\n");
		fclose(input);
		return -1;
	}
	if (config.frameLength != kDefaultFrameSize)
	{
		fprintf(stderr, "ALAC cookie frame length must be 4096\n");
		fclose(input);
		return -1;
	}
	if ((uint32_t)config.sampleRate != (uint32_t)caf.sampleRate)
	{
		fprintf(stderr, "ALAC cookie sample rate does not match desc\n");
		fclose(input);
		return -1;
	}

	uint32_t depthFromFlags = 0;
	switch (caf.formatFlags)
	{
		case 1: depthFromFlags = 16; break;
		case 2: depthFromFlags = 20; break;
		case 3: depthFromFlags = 24; break;
		case 4: depthFromFlags = 32; break;
		default: depthFromFlags = 0; break;
	}
	int32_t bitDepth = config.bitDepth;
	if (bitDepth != 16 && bitDepth != 24)
	{
		fprintf(stderr, "projection input must be 16 or 24 bit\n");
		fclose(input);
		return -1;
	}
	if (depthFromFlags != 0 && (int32_t)depthFromFlags != bitDepth)
	{
		fprintf(stderr, "desc format flags disagree with the ALAC bit depth\n");
		fclose(input);
		return -1;
	}
	if (caf.bitsPerChannel != 0 && (int32_t)caf.bitsPerChannel != bitDepth)
	{
		fprintf(stderr, "desc bit depth disagrees with the ALAC bit depth\n");
		fclose(input);
		return -1;
	}

	// ---- validate layout and packet directory ----
	std::vector<uint32_t> inputLabels;
	if (!DeriveInputLabels(caf, config, cookieHasTag, cookieTag, &inputLabels, &error))
	{
		fprintf(stderr, "%s\n", error.c_str());
		fclose(input);
		return -1;
	}
	if (slots.size() > inputLabels.size())
	{
		fprintf(stderr, "projection slot is out of range for the input channel count\n");
		fclose(input);
		return -1;
	}
	for (size_t i = 0; i < slots.size(); ++i)
	{
		if (slots[i] >= inputLabels.size())
		{
			fprintf(stderr, "projection slot is out of range for the input channel count\n");
			fclose(input);
			return -1;
		}
	}

	if (caf.numberPackets <= 0)
	{
		fprintf(stderr, "projection input contains no packets\n");
		fclose(input);
		return -1;
	}
	if ((int64_t)caf.packetSizes.size() != caf.numberPackets ||
	    (int64_t)caf.packetFrames.size() != caf.numberPackets)
	{
		fprintf(stderr, "packet table entry count is inconsistent\n");
		fclose(input);
		return -1;
	}

	int64_t totalBytes = 0;
	int64_t totalFrames = 0;
	for (int64_t i = 0; i < caf.numberPackets; ++i)
	{
		uint32_t bytes = caf.packetSizes[(size_t)i];
		uint32_t frames = caf.packetFrames[(size_t)i];
		if (bytes == 0 || bytes > kMaxPacketBytes)
		{
			fprintf(stderr, "packet %lld has an invalid byte size\n", (long long)i);
			fclose(input);
			return -1;
		}
		if (frames == 0 || frames > kDefaultFrameSize)
		{
			fprintf(stderr, "packet %lld has an invalid frame count\n", (long long)i);
			fclose(input);
			return -1;
		}
		if (i != caf.numberPackets - 1 && frames != kDefaultFrameSize)
		{
			fprintf(stderr, "only the last packet may be short\n");
			fclose(input);
			return -1;
		}
		if (i == caf.numberPackets - 1 && frames > kDefaultFrameSize)
		{
			fprintf(stderr, "last packet frame count is out of range\n");
			fclose(input);
			return -1;
		}
		totalBytes += bytes;
		totalFrames += frames;
	}
	if (totalBytes != caf.dataSize)
	{
		fprintf(stderr, "packet table does not close over the data chunk\n");
		fclose(input);
		return -1;
	}
	(void)totalFrames;
	if (caf.framesPerPacket == 0 || caf.numberValidFrames == 0)
	{
		fprintf(stderr, "projection input contains no valid frames\n");
		fclose(input);
		return -1;
	}

	// ---- output element layout ----
	const uint32_t outputChannels = (uint32_t)slots.size();
	std::vector<uint32_t> outputLabels;
	for (size_t i = 0; i < slots.size(); ++i)
		outputLabels.push_back(inputLabels[slots[i]]);
	const bool standardOutput = LabelsAreStandard(outputLabels);
	const uint32_t standardTag = standardOutput ? StandardTagForLabels(outputLabels) : 0;

	// ---- prepare temporary files ----
	char suffix[64];
	snprintf(suffix, sizeof(suffix), ".alacproj.%ld", (long)ALACPROJ_GETPID());
	payloadPath = std::string(outputPath) + suffix + ".data";
	finalPath = std::string(outputPath) + suffix + ".out";

	payload = fopen(payloadPath.c_str(), "w+b");
	if (payload == NULL)
	{
		fprintf(stderr, "cannot create temporary file: %s\n", strerror(errno));
		fclose(input);
		return -1;
	}

	// ---- create the decoder and encoder ----
	ALACDecoder decoder;
	int32_t status = caf.cookie.empty()
	                     ? kALAC_ParamError
	                     : decoder.Init(&caf.cookie[0], (uint32_t)caf.cookie.size());
	if (status != ALAC_noErr)
	{
		fprintf(stderr, "cannot initialise the ALAC decoder\n");
		fclose(payload);
		remove(payloadPath.c_str());
		fclose(input);
		return -1;
	}

	AudioFormatDescription outputFormat;
	memset(&outputFormat, 0, sizeof(outputFormat));
	outputFormat.mSampleRate = caf.sampleRate;
	outputFormat.mFormatID = kALACFormatAppleLossless;
	outputFormat.mFormatFlags = (bitDepth == 16) ? 1 : 3;
	outputFormat.mFramesPerPacket = kDefaultFrameSize;
	outputFormat.mChannelsPerFrame = outputChannels;

	ALACEncoder encoder;
	encoder.SetFrameSize(kDefaultFrameSize);
	if (encoder.InitializeEncoder(outputFormat) != ALAC_noErr)
	{
		fprintf(stderr, "cannot initialise the ALAC encoder\n");
		fclose(payload);
		remove(payloadPath.c_str());
		fclose(input);
		return -1;
	}

	const uint32_t bytesPerSample = BytesPerSample((uint32_t)bitDepth);
	const uint32_t sourceChannels = caf.channelsPerFrame;

	const size_t sourcePCMBytes = (size_t)kDefaultFrameSize * sourceChannels * bytesPerSample;
	const size_t outputPCMBytes = (size_t)kDefaultFrameSize * outputChannels * bytesPerSample;
	// An output packet is bounded by the input packet (a pure reorder copies
	// element bodies nearly verbatim) plus per-element and END overhead.
	const size_t maxOutPacketBytes = (size_t)kMaxPacketBytes + 65536;

	std::vector<uint8_t> sourcePCM(sourcePCMBytes);
	std::vector<uint8_t> outputPCM(outputPCMBytes);
	// BitBuffer reads up to 3 bytes past the current position, so keep a little
	// zeroed slack after the real data.
	std::vector<uint8_t> sourcePacket;
	std::vector<uint8_t> outputPacket(maxOutPacketBytes + 8);
	std::vector<ALACElementTrace> traces(kALACMaxChannels * 2);
	std::vector<uint32_t> outputPacketSizes;
	outputPacketSizes.reserve((size_t)caf.numberPackets);

	uint32_t maxFrameBytes = 0;
	int64_t packetOffset = 0;

	for (int64_t packetIndex = 0; packetIndex < caf.numberPackets; ++packetIndex)
	{
		uint32_t packetBytes = caf.packetSizes[(size_t)packetIndex];
		uint32_t packetFrames = caf.packetFrames[(size_t)packetIndex];

		if (!SeekTo(input, caf.dataOffset + packetOffset))
		{
			fprintf(stderr, "cannot seek to packet %lld\n", (long long)packetIndex);
			goto fail;
		}

		sourcePacket.assign((size_t)packetBytes + 8, 0);
		if (fread(&sourcePacket[0], 1, packetBytes, input) != packetBytes)
		{
			fprintf(stderr, "cannot read packet %lld\n", (long long)packetIndex);
			goto fail;
		}
		packetOffset += (int64_t)packetBytes;

		// Decode real PCM for every source channel, including discarded ones.
		{
			BitBuffer bits;
			BitBufferInit(&bits, &sourcePacket[0], packetBytes);
			uint32_t decodedFrames = 0;
			uint32_t traceCount = 0;
			memset(&sourcePCM[0], 0, sourcePCMBytes);
			status = decoder.DecodePacket(&bits, &sourcePCM[0], packetFrames, sourceChannels,
			                              &decodedFrames, &traces[0],
			                              (uint32_t)traces.size(), &traceCount);
			if (status != ALAC_noErr)
			{
				fprintf(stderr, "packet %lld could not be decoded\n", (long long)packetIndex);
				goto fail;
			}
			if (decodedFrames != packetFrames)
			{
				fprintf(stderr, "packet %lld frame count does not match the packet table\n",
				        (long long)packetIndex);
				goto fail;
			}
			traces.resize(traceCount);
		}

		std::vector<OutputElement> elements;
		if (!BuildOutputElements(outputChannels, slots, traces, &elements, &error))
		{
			fprintf(stderr, "%s\n", error.c_str());
			goto fail;
		}

		// Build the output PCM in output channel order.  It is only used by
		// elements that must be re-encoded, but it is bounded by one packet.
		for (uint32_t frame = 0; frame < packetFrames; ++frame)
		{
			for (uint32_t channel = 0; channel < outputChannels; ++channel)
			{
				const uint8_t *source = &sourcePCM[((size_t)frame * sourceChannels + slots[channel]) * bytesPerSample];
				uint8_t *destination = &outputPCM[((size_t)frame * outputChannels + channel) * bytesPerSample];
				CopySample(destination, source, bytesPerSample);
			}
		}

		// Compose the new packet: elements in output order, one ID_END.
		memset(&outputPacket[0], 0, outputPacket.size());
		BitBuffer outBits;
		BitBufferInit(&outBits, &outputPacket[0], (uint32_t)outputPacket.size());

		for (size_t e = 0; e < elements.size(); ++e)
		{
			const OutputElement &element = elements[e];
			// The output packet buffer has 8 bytes of slack for BitBuffer's
			// read-ahead; reject an element that would overrun it before the
			// low-level writer runs (it bounds-checks, but not the slack).
			if (BitBufferGetPosition(&outBits) / 8 + 16 > outputPacket.size())
			{
				fprintf(stderr, "packet %lld exceeds the output packet buffer\n", (long long)packetIndex);
				goto fail;
			}
			BitBufferWrite(&outBits, element.tag, 3);
			BitBufferWrite(&outBits, element.instanceTag, 4);

			if (element.reuse)
			{
				// Keep every valid bit after the tag and instance tag exactly
				// as it was, even at a new (possibly unaligned) position.
				uint32_t sourceBodyStart = element.sourceStartBit + 7;
				if (element.sourceEndBit < sourceBodyStart ||
				    element.sourceEndBit > packetBytes * 8)
				{
					fprintf(stderr, "packet %lld has an invalid source element range\n", (long long)packetIndex);
					goto fail;
				}
				uint32_t sourceBodyBits = element.sourceEndBit - sourceBodyStart;
				CopyBits(&outBits, &sourcePacket[0], packetBytes, sourceBodyStart, sourceBodyBits);
			}
			else
			{
				uint8_t *elementPCM = &outputPCM[(size_t)element.outStart * bytesPerSample];
				status = encoder.EncodeElement(&outBits, element.tag, elementPCM,
				                               outputChannels, element.outStart, packetFrames);
				if (status != ALAC_noErr)
				{
					fprintf(stderr, "packet %lld element %u could not be encoded\n",
					        (long long)packetIndex, (unsigned)e);
					goto fail;
				}
			}
		}

		BitBufferWrite(&outBits, ID_END, 3);
		BitBufferByteAlign(&outBits, true);
		uint32_t outputBytes = BitBufferGetPosition(&outBits) / 8;
		if (outputBytes == 0 || outputBytes > outputPacket.size())
		{
			fprintf(stderr, "packet %lld produced an invalid size\n", (long long)packetIndex);
			goto fail;
		}
		if (outputBytes > maxFrameBytes)
			maxFrameBytes = outputBytes;

		if (fwrite(&outputPacket[0], 1, outputBytes, payload) != outputBytes)
		{
			fprintf(stderr, "cannot write temporary payload\n");
			goto fail;
		}
		outputPacketSizes.push_back(outputBytes);
	}

	// ---- write the final CAF container ----
	finalFile = fopen(finalPath.c_str(), "w+b");
	if (finalFile == NULL)
	{
		fprintf(stderr, "cannot create output file: %s\n", strerror(errno));
		goto fail;
	}

	{
		// caff header
		std::vector<uint8_t> header;
		header.push_back('c');
		header.push_back('a');
		header.push_back('f');
		header.push_back('f');
		PutBE16(&header, k_port_CAF_FileVersion_Initial);
		PutBE16(&header, 0);
		if (!WriteAll(finalFile, &header[0], header.size()))
			goto writeFail;

		// desc
		{
			std::vector<uint8_t> desc;
			desc.reserve(32);
			PutBEDouble(&desc, caf.sampleRate);
			PutBE32(&desc, kALACFormatAppleLossless);
			PutBE32(&desc, (bitDepth == 16) ? 1u : 3u);
			PutBE32(&desc, 0);                      // bytesPerPacket (VBR)
			PutBE32(&desc, kDefaultFrameSize);
			PutBE32(&desc, outputChannels);
			PutBE32(&desc, 0);                      // bitsPerChannel
			WriteChunkHeader(&header, 0, 0);        // placeholder, replaced below
			header.clear();
			WriteChunkHeader(&header, k_port_CAF_StreamDescriptionChunkID, desc.size());
			header.insert(header.end(), desc.begin(), desc.end());
			if (!WriteAll(finalFile, &header[0], header.size()))
				goto writeFail;
		}

		// kuki
		{
			std::vector<uint8_t> cookie;
			PutBE32(&cookie, kDefaultFrameSize);            // frameLength
			cookie.push_back(0);                            // compatibleVersion
			cookie.push_back((uint8_t)bitDepth);            // bitDepth
			cookie.push_back(40);                           // pb
			cookie.push_back(10);                           // mb
			cookie.push_back(14);                           // kb
			cookie.push_back((uint8_t)outputChannels);      // numChannels
			PutBE16(&cookie, 255);                          // maxRun
			PutBE32(&cookie, maxFrameBytes);                // maxFrameBytes
			PutBE32(&cookie, 0);                            // avgBitRate
			PutBE32(&cookie, (uint32_t)caf.sampleRate);     // sampleRate

			if (standardOutput && outputChannels > 2)
			{
				PutBE32(&cookie, 24);           // 'chan' atom size
				cookie.push_back('c');
				cookie.push_back('h');
				cookie.push_back('a');
				cookie.push_back('n');
				PutBE32(&cookie, 0);            // versionFlags
				PutBE32(&cookie, standardTag);  // channelLayoutTag
				PutBE32(&cookie, 0);            // reserved1
				PutBE32(&cookie, 0);            // reserved2
			}

			header.clear();
			WriteChunkHeader(&header, k_port_CAF_MagicCookieID, cookie.size());
			header.insert(header.end(), cookie.begin(), cookie.end());
			if (!WriteAll(finalFile, &header[0], header.size()))
				goto writeFail;
		}

		// chan
		if (standardOutput && outputChannels > 2)
		{
			std::vector<uint8_t> chan;
			PutBE32(&chan, standardTag);
			PutBE32(&chan, 0);
			PutBE32(&chan, 0);
			header.clear();
			WriteChunkHeader(&header, k_port_CAF_ChannelLayoutChunkID, chan.size());
			header.insert(header.end(), chan.begin(), chan.end());
			if (!WriteAll(finalFile, &header[0], header.size()))
				goto writeFail;
		}
		else if (!standardOutput)
		{
			std::vector<uint8_t> chan;
			PutBE32(&chan, kAudioChannelLayoutTag_UseChannelDescriptions);
			PutBE32(&chan, 0);
			PutBE32(&chan, outputChannels);
			for (uint32_t i = 0; i < outputChannels; ++i)
			{
				PutBE32(&chan, outputLabels[i]);
				PutBE32(&chan, 0);              // flags
				uint32_t zero = 0;
				for (int c = 0; c < 3; ++c)
					PutBE32(&chan, zero);       // coordinates
			}
			header.clear();
			WriteChunkHeader(&header, k_port_CAF_ChannelLayoutChunkID, chan.size());
			header.insert(header.end(), chan.begin(), chan.end());
			if (!WriteAll(finalFile, &header[0], header.size()))
				goto writeFail;
		}
	}

	// pakt
	{
		std::vector<uint8_t> pakt;
		PutBE64(&pakt, (uint64_t)caf.numberPackets);
		PutBE64(&pakt, (uint64_t)caf.numberValidFrames);
		PutBE32(&pakt, (uint32_t)caf.primingFrames);
		PutBE32(&pakt, (uint32_t)caf.remainderFrames);
		for (int64_t i = 0; i < caf.numberPackets; ++i)
		{
			// Byte sizes come from the real output packets.  The output desc
			// has a non-zero framesPerPacket, so the table carries byte sizes
			// only and the frame counts stay implicit.
			PutBER32(&pakt, outputPacketSizes[(size_t)i]);
		}
		std::vector<uint8_t> header;
		WriteChunkHeader(&header, k_port_CAF_PacketTableChunkID, pakt.size());
		header.insert(header.end(), pakt.begin(), pakt.end());
		if (!WriteAll(finalFile, &header[0], header.size()))
			goto writeFail;
	}

	// data
	{
		int64_t payloadBytes = 0;
		for (size_t i = 0; i < outputPacketSizes.size(); ++i)
			payloadBytes += outputPacketSizes[i];
		std::vector<uint8_t> header;
		WriteChunkHeader(&header, k_port_CAF_AudioDataChunkID, (uint64_t)(payloadBytes + 4));
		PutBE32(&header, 0); // edit count
		if (!WriteAll(finalFile, &header[0], header.size()))
			goto writeFail;

		if (fflush(payload) != 0 || !CopyRange(payload, finalFile, 0, payloadBytes))
			goto writeFail;
	}

	if (fflush(finalFile) != 0 || fclose(finalFile) != 0)
	{
		finalFile = NULL;
		goto writeFail;
	}
	finalFile = NULL;

	if (rename(finalPath.c_str(), outputPath) != 0)
	{
		fprintf(stderr, "cannot publish output file: %s\n", strerror(errno));
		goto fail;
	}
	published = true;

	fclose(payload);
	payload = NULL;
	remove(payloadPath.c_str());
	fclose(input);
	return 0;

writeFail:
	fprintf(stderr, "cannot write output file: %s\n", strerror(errno));

fail:
	if (finalFile != NULL)
	{
		fclose(finalFile);
		finalFile = NULL;
	}
	if (payload != NULL)
	{
		fclose(payload);
		payload = NULL;
	}
	if (!published)
	{
		remove(finalPath.c_str());
		remove(payloadPath.c_str());
	}
	fclose(input);
	return -1;
}
