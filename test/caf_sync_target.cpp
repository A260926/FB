/*
	Regression tests for syncing CAF/ALAC files while recording.

	After every successful afSyncFile the file on disk must be a valid,
	self-consistent CAF file whose packet table, data chunk and frame
	count describe exactly the PCM written so far.  Registered with the
	task driver in task_support/driver.c; no main function here.
*/

#include "task_support/check.h"

#include "audiofile.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#define SYNC_CHECK(x) do { if (!task_check(!!(x), #x, __FILE__, __LINE__)) return false; } while (0)

static const int kTrack = AF_DEFAULT_TRACK;
static const int64_t kFramesPerPacket = 4096;

/* Virtual 32-bit integer sample for the given frame and channel. */
static int32_t sampleValue(int64_t frame, int channel)
{
	return (int32_t)((((frame * 131 + channel * 997) % 40001) - 20000) * 65536);
}

static std::string tempPath()
{
	char tmpl[] = ".task-build/cafsyncXXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0)
	{
		strcpy(tmpl, "cafsyncXXXXXX");
		fd = mkstemp(tmpl);
	}
	if (fd >= 0)
		close(fd);
	return std::string(tmpl);
}

static AFfilehandle openWriter(const char *path, int bits, int channels,
	int compression)
{
	AFfilesetup setup = afNewFileSetup();
	afInitFileFormat(setup, AF_FILE_CAF);
	afInitChannels(setup, kTrack, channels);
	afInitSampleFormat(setup, kTrack, AF_SAMPFMT_TWOSCOMP, bits);
	afInitRate(setup, kTrack, 44100.0);
	afInitCompression(setup, kTrack, compression);
	AFfilehandle file = afOpenFile(path, "w", setup);
	afFreeFileSetup(setup);
	if (file)
		afSetVirtualSampleFormat(file, kTrack, AF_SAMPFMT_TWOSCOMP, 32);
	return file;
}

static bool writeSegment(AFfilehandle file, int64_t startFrame, int count,
	int channels)
{
	std::vector<int32_t> pcm(count * channels);
	for (int i = 0; i < count; i++)
		for (int c = 0; c < channels; c++)
			pcm[i * channels + c] = sampleValue(startFrame + i, c);
	return afWriteFrames(file, kTrack, pcm.data(), count) == count;
}

static bool readFileBytes(const char *path, std::vector<uint8_t> &out)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return false;
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	out.resize(size > 0 ? size : 0);
	bool ok = size == 0 || fread(&out[0], 1, size, fp) == static_cast<size_t>(size);
	fclose(fp);
	return ok;
}

static uint32_t getBE32(const uint8_t *p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
		(uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static uint64_t getBE64(const uint8_t *p)
{
	return (uint64_t(getBE32(p)) << 32) | getBE32(p + 4);
}

/* Independent parse of the CAF container structure. */
struct CafLayout
{
	CafLayout() :
		fileSize(0), dataOffset(0), audioBytes(0),
		hasPakt(false), paktStart(0), paktEnd(0),
		numPackets(0), numValidFrames(0) { }

	int64_t fileSize;
	int64_t dataOffset;	/* offset of first audio byte */
	int64_t audioBytes;	/* declared audio byte count */
	bool hasPakt;
	int64_t paktStart;	/* offset of pakt chunk header */
	int64_t paktEnd;	/* offset just past pakt chunk */
	int64_t numPackets;
	int64_t numValidFrames;
	std::vector<uint32_t> packetSizes;
};

static bool parseCaf(const std::vector<uint8_t> &bytes, CafLayout &layout)
{
	layout = CafLayout();
	layout.fileSize = bytes.size();
	if (bytes.size() < 8 || memcmp(&bytes[0], "caff", 4) != 0)
		return false;

	size_t pos = 8;
	while (pos + 12 <= bytes.size())
	{
		const uint8_t *chunk = &bytes[pos];
		uint64_t length = getBE64(chunk + 4);
		size_t contentStart = pos + 12;

		if (!memcmp(chunk, "data", 4))
		{
			layout.dataOffset = contentStart + 4; /* skip edit count */
			if (static_cast<int64_t>(length) == -1)
				layout.audioBytes = layout.fileSize - layout.dataOffset;
			else
				layout.audioBytes = static_cast<int64_t>(length) - 4;
		}
		else if (!memcmp(chunk, "pakt", 4))
		{
			if (length < 24 || contentStart + length > bytes.size())
				return false;
			const uint8_t *p = &bytes[contentStart];
			layout.hasPakt = true;
			layout.paktStart = pos;
			layout.numPackets = static_cast<int64_t>(getBE64(p));
			layout.numValidFrames = static_cast<int64_t>(getBE64(p + 8));

			size_t tpos = contentStart + 24;
			size_t tend = contentStart + static_cast<size_t>(length);
			while (tpos < tend)
			{
				uint32_t value = 0;
				size_t n = 0;
				bool complete = false;
				while (tpos + n < tend && n < 5)
				{
					uint8_t byte = bytes[tpos + n];
					n++;
					value = (value << 7) | (byte & 0x7f);
					if (!(byte & 0x80))
					{
						complete = true;
						break;
					}
				}
				if (!complete || value == 0)
					return false;
				layout.packetSizes.push_back(value);
				tpos += n;
			}
			layout.paktEnd = contentStart + length;
		}

		if (static_cast<int64_t>(length) == -1)
			pos = bytes.size();
		else
			pos = contentStart + static_cast<size_t>(length);
	}

	/* Chunks must tile the file exactly: no trailing or stale bytes. */
	return pos == bytes.size();
}

/*
	Verify the on-disk directory of a synced ALAC file: the data chunk
	length, the packet table entries, the packet count and the valid
	frame count must all agree, and nothing stale may follow the packet
	table.
*/
static bool checkLayout(const char *path, int64_t expectedFrames)
{
	std::vector<uint8_t> bytes;
	SYNC_CHECK(readFileBytes(path, bytes));

	CafLayout layout;
	SYNC_CHECK(parseCaf(bytes, layout));

	SYNC_CHECK(layout.hasPakt);
	SYNC_CHECK(layout.numValidFrames == expectedFrames);

	int64_t expectedPackets = expectedFrames > 0 ?
		(expectedFrames + kFramesPerPacket - 1) / kFramesPerPacket : 0;
	SYNC_CHECK(layout.numPackets == expectedPackets);
	SYNC_CHECK(static_cast<int64_t>(layout.packetSizes.size()) == layout.numPackets);

	int64_t sum = 0;
	for (size_t i = 0; i < layout.packetSizes.size(); i++)
	{
		SYNC_CHECK(layout.packetSizes[i] > 0);
		sum += layout.packetSizes[i];
	}
	SYNC_CHECK(sum == layout.audioBytes);

	/* The packet table must sit right after the audio data and end at EOF. */
	SYNC_CHECK(layout.dataOffset + layout.audioBytes == layout.paktStart);
	SYNC_CHECK(layout.paktEnd == layout.fileSize);
	return true;
}

/* Verify frame count and every sample by reading forward in chunks. */
static bool verifySamples(const char *path, int channels,
	int64_t expectedFrames, int chunkFrames)
{
	AFfilehandle file = afOpenFile(path, "r", NULL);
	SYNC_CHECK(file != NULL);
	SYNC_CHECK(afSetVirtualSampleFormat(file, kTrack, AF_SAMPFMT_TWOSCOMP, 32) == 0);
	SYNC_CHECK(afGetChannels(file, kTrack) == channels);
	SYNC_CHECK(afGetFrameCount(file, kTrack) == expectedFrames);

	std::vector<int32_t> buffer(chunkFrames * channels);
	int64_t frame = 0;
	while (frame < expectedFrames)
	{
		int64_t left = expectedFrames - frame;
		int want = left < chunkFrames ? static_cast<int>(left) : chunkFrames;
		int got = afReadFrames(file, kTrack, &buffer[0], want);
		SYNC_CHECK(got == want);
		for (int i = 0; i < got; i++)
			for (int c = 0; c < channels; c++)
				SYNC_CHECK(buffer[i * channels + c] == sampleValue(frame + i, c));
		frame += got;
	}
	SYNC_CHECK(afReadFrames(file, kTrack, &buffer[0], 1) == 0);
	SYNC_CHECK(afCloseFile(file) == 0);
	return true;
}

/* Verify every sample by reading one frame at a time. */
static bool verifyFrameByFrame(const char *path, int channels,
	int64_t expectedFrames)
{
	AFfilehandle file = afOpenFile(path, "r", NULL);
	SYNC_CHECK(file != NULL);
	SYNC_CHECK(afSetVirtualSampleFormat(file, kTrack, AF_SAMPFMT_TWOSCOMP, 32) == 0);
	SYNC_CHECK(afGetFrameCount(file, kTrack) == expectedFrames);

	std::vector<int32_t> one(channels);
	for (int64_t i = 0; i < expectedFrames; i++)
	{
		SYNC_CHECK(afReadFrames(file, kTrack, &one[0], 1) == 1);
		for (int c = 0; c < channels; c++)
			SYNC_CHECK(one[c] == sampleValue(i, c));
	}
	SYNC_CHECK(afReadFrames(file, kTrack, &one[0], 1) == 0);
	SYNC_CHECK(afCloseFile(file) == 0);
	return true;
}

/* Verify random seeks and reads around packet boundaries. */
static bool verifyRandomSeeks(const char *path, int channels,
	int64_t expectedFrames, int iterations)
{
	AFfilehandle file = afOpenFile(path, "r", NULL);
	SYNC_CHECK(file != NULL);
	SYNC_CHECK(afSetVirtualSampleFormat(file, kTrack, AF_SAMPFMT_TWOSCOMP, 32) == 0);

	uint32_t state = 88172645463325252ull & 0xffffffff;
	std::vector<int32_t> buffer(64 * channels);
	for (int iteration = 0; iteration < iterations && expectedFrames > 0; iteration++)
	{
		state = state * 1103515245 + 12345;
		int64_t pos = (state >> 8) % (expectedFrames + 1);
		SYNC_CHECK(afSeekFrame(file, kTrack, pos) == pos);
		int64_t left = expectedFrames - pos;
		int want = left < 37 ? static_cast<int>(left) : 37;
		if (want == 0)
		{
			SYNC_CHECK(afReadFrames(file, kTrack, &buffer[0], 1) == 0);
			continue;
		}
		SYNC_CHECK(afReadFrames(file, kTrack, &buffer[0], want) == want);
		for (int i = 0; i < want; i++)
			for (int c = 0; c < channels; c++)
				SYNC_CHECK(buffer[i * channels + c] == sampleValue(pos + i, c));
	}
	SYNC_CHECK(afCloseFile(file) == 0);
	return true;
}

static bool verifyFrameCount(const char *path, int64_t expectedFrames)
{
	AFfilehandle file = afOpenFile(path, "r", NULL);
	SYNC_CHECK(file != NULL);
	SYNC_CHECK(afGetFrameCount(file, kTrack) == expectedFrames);
	SYNC_CHECK(afCloseFile(file) == 0);
	return true;
}

/*
	snapshots: the on-disk directory must be complete and consistent
	after every single sync.
*/
static int snapshots_directory_matches_audio()
{
	std::string path = tempPath();
	const int segments[] = { 17, 4079, 1, 9000, 3 };
	AFfilehandle writer = openWriter(path.c_str(), 16, 2, AF_COMPRESSION_ALAC);
	TASK_CHECK(writer != NULL);

	int64_t total = 0;
	for (size_t i = 0; i < sizeof segments / sizeof segments[0]; i++)
	{
		TASK_CHECK(writeSegment(writer, total, segments[i], 2));
		total += segments[i];
		TASK_CHECK(afSyncFile(writer) == 0);
		TASK_CHECK(checkLayout(path.c_str(), total));
		TASK_CHECK(verifyFrameCount(path.c_str(), total));
	}
	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(checkLayout(path.c_str(), total));
	TASK_CHECK(verifyFrameCount(path.c_str(), total));

	unlink(path.c_str());
	return 0;
}

/* snapshots: syncing again with no new PCM must not change the file. */
static int snapshots_repeated_sync_byte_identical()
{
	std::string path = tempPath();
	AFfilehandle writer = openWriter(path.c_str(), 16, 1, AF_COMPRESSION_ALAC);
	TASK_CHECK(writer != NULL);

	/* One full packet plus a 100-frame tail. */
	TASK_CHECK(writeSegment(writer, 0, 4196, 1));
	TASK_CHECK(afSyncFile(writer) == 0);
	std::vector<uint8_t> first, second;
	TASK_CHECK(readFileBytes(path.c_str(), first));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(readFileBytes(path.c_str(), second));
	TASK_CHECK(first == second);
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(readFileBytes(path.c_str(), second));
	TASK_CHECK(first == second);

	/* More audio, then more no-op syncs. */
	TASK_CHECK(writeSegment(writer, 4196, 4, 1));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(readFileBytes(path.c_str(), first));
	TASK_CHECK(first != second);
	TASK_CHECK(checkLayout(path.c_str(), 4200));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(readFileBytes(path.c_str(), second));
	TASK_CHECK(first == second);

	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(checkLayout(path.c_str(), 4200));
	TASK_CHECK(verifySamples(path.c_str(), 1, 4200, 313));

	unlink(path.c_str());
	return 0;
}

/* snapshots: a sync on a packet-aligned buffer must not add a tail. */
static int snapshots_packet_aligned_sync()
{
	std::string path = tempPath();
	AFfilehandle writer = openWriter(path.c_str(), 24, 2, AF_COMPRESSION_ALAC);
	TASK_CHECK(writer != NULL);

	TASK_CHECK(writeSegment(writer, 0, 4096, 2));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(checkLayout(path.c_str(), 4096));

	std::vector<uint8_t> first, second;
	TASK_CHECK(readFileBytes(path.c_str(), first));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(readFileBytes(path.c_str(), second));
	TASK_CHECK(first == second);

	TASK_CHECK(writeSegment(writer, 4096, 4096, 2));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(checkLayout(path.c_str(), 8192));
	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(checkLayout(path.c_str(), 8192));
	TASK_CHECK(verifySamples(path.c_str(), 2, 8192, 1023));

	unlink(path.c_str());
	return 0;
}

/* continuation: a short tail followed by more than one packet of new audio. */
static int continuation_short_tail_then_many_packets()
{
	std::string path = tempPath();
	AFfilehandle writer = openWriter(path.c_str(), 16, 2, AF_COMPRESSION_ALAC);
	TASK_CHECK(writer != NULL);

	TASK_CHECK(writeSegment(writer, 0, 5, 2));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 2, 5, 3));

	TASK_CHECK(writeSegment(writer, 5, 20000, 2));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(checkLayout(path.c_str(), 20005));
	TASK_CHECK(verifySamples(path.c_str(), 2, 20005, 1523));

	TASK_CHECK(writeSegment(writer, 20005, 91, 2));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 2, 20096, 941));

	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(checkLayout(path.c_str(), 20096));
	TASK_CHECK(verifySamples(path.c_str(), 2, 20096, 20096));

	unlink(path.c_str());
	return 0;
}

/* continuation: writes that land exactly on packet boundaries. */
static int continuation_exact_fill_then_continue()
{
	std::string path = tempPath();
	AFfilehandle writer = openWriter(path.c_str(), 24, 1, AF_COMPRESSION_ALAC);
	TASK_CHECK(writer != NULL);

	TASK_CHECK(writeSegment(writer, 0, 4096, 1));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 1, 4096, 4096));

	TASK_CHECK(writeSegment(writer, 4096, 4096, 1));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 1, 8192, 2048));

	TASK_CHECK(writeSegment(writer, 8192, 1, 1));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(checkLayout(path.c_str(), 8193));
	TASK_CHECK(verifySamples(path.c_str(), 1, 8193, 4097));

	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 1, 8193, 8193));

	unlink(path.c_str());
	return 0;
}

/* continuation: a recording with no frames at all. */
static int continuation_zero_frame_recording()
{
	std::string path = tempPath();
	AFfilehandle writer = openWriter(path.c_str(), 16, 2, AF_COMPRESSION_ALAC);
	TASK_CHECK(writer != NULL);
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(verifyFrameCount(path.c_str(), 0));
	TASK_CHECK(checkLayout(path.c_str(), 0));
	unlink(path.c_str());

	/* Closing without an explicit sync must behave the same. */
	path = tempPath();
	writer = openWriter(path.c_str(), 16, 2, AF_COMPRESSION_ALAC);
	TASK_CHECK(writer != NULL);
	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(verifyFrameCount(path.c_str(), 0));
	TASK_CHECK(checkLayout(path.c_str(), 0));
	unlink(path.c_str());

	/* Syncing an empty recording must not disturb later writes. */
	path = tempPath();
	writer = openWriter(path.c_str(), 16, 2, AF_COMPRESSION_ALAC);
	TASK_CHECK(writer != NULL);
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(verifyFrameCount(path.c_str(), 0));
	TASK_CHECK(writeSegment(writer, 0, 100, 2));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(checkLayout(path.c_str(), 100));
	TASK_CHECK(verifySamples(path.c_str(), 2, 100, 7));
	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 2, 100, 100));
	unlink(path.c_str());
	return 0;
}

/* continuation: a genuine one-frame tail that must survive to close. */
static int continuation_real_short_tail()
{
	std::string path = tempPath();
	AFfilehandle writer = openWriter(path.c_str(), 24, 2, AF_COMPRESSION_ALAC);
	TASK_CHECK(writer != NULL);

	TASK_CHECK(writeSegment(writer, 0, 4097, 2));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 2, 4097, 1000));

	TASK_CHECK(writeSegment(writer, 4097, 2, 2));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 2, 4099, 2050));

	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(checkLayout(path.c_str(), 4099));
	TASK_CHECK(verifySamples(path.c_str(), 2, 4099, 4099));

	unlink(path.c_str());
	return 0;
}

/*
	consumer: whatever read pattern a listener uses, after each sync it
	must see exactly the PCM prefix written so far.
*/
static int consumer_read_patterns_match_written_prefix()
{
	std::string path = tempPath();
	const int segments[] = { 17, 4079, 1, 9000, 3 };
	AFfilehandle writer = openWriter(path.c_str(), 16, 2, AF_COMPRESSION_ALAC);
	TASK_CHECK(writer != NULL);

	int64_t total = 0;
	for (size_t i = 0; i < sizeof segments / sizeof segments[0]; i++)
	{
		TASK_CHECK(writeSegment(writer, total, segments[i], 2));
		total += segments[i];
		TASK_CHECK(afSyncFile(writer) == 0);
		TASK_CHECK(verifySamples(path.c_str(), 2, total, 773));
		TASK_CHECK(verifyRandomSeeks(path.c_str(), 2, total, 64));
	}
	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 2, total, 1));
	TASK_CHECK(verifyFrameByFrame(path.c_str(), 2, total));
	TASK_CHECK(verifyRandomSeeks(path.c_str(), 2, total, 256));

	unlink(path.c_str());
	return 0;
}

/* consumer: uncompressed CAF must behave as before, syncs included. */
static int consumer_uncompressed_pcm_regression()
{
	std::string path = tempPath();
	AFfilehandle writer = openWriter(path.c_str(), 16, 2, AF_COMPRESSION_NONE);
	TASK_CHECK(writer != NULL);

	TASK_CHECK(writeSegment(writer, 0, 3000, 2));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 2, 3000, 1000));

	TASK_CHECK(writeSegment(writer, 3000, 6001, 2));
	TASK_CHECK(afSyncFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 2, 9001, 4501));

	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(verifySamples(path.c_str(), 2, 9001, 9001));
	TASK_CHECK(verifyRandomSeeks(path.c_str(), 2, 9001, 128));

	unlink(path.c_str());
	return 0;
}

/* consumer: ALAC without any sync must round-trip as before. */
static int consumer_alac_roundtrip_without_sync()
{
	std::string path = tempPath();
	AFfilehandle writer = openWriter(path.c_str(), 16, 2, AF_COMPRESSION_ALAC);
	TASK_CHECK(writer != NULL);
	TASK_CHECK(writeSegment(writer, 0, 9001, 2));
	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(checkLayout(path.c_str(), 9001));
	TASK_CHECK(verifySamples(path.c_str(), 2, 9001, 1024));
	TASK_CHECK(verifyRandomSeeks(path.c_str(), 2, 9001, 128));

	unlink(path.c_str());
	return 0;
}

const struct task_case task_cases[] =
{
	{ "snapshots", "directory_matches_audio", snapshots_directory_matches_audio },
	{ "snapshots", "repeated_sync_byte_identical", snapshots_repeated_sync_byte_identical },
	{ "snapshots", "packet_aligned_sync", snapshots_packet_aligned_sync },
	{ "continuation", "short_tail_then_many_packets", continuation_short_tail_then_many_packets },
	{ "continuation", "exact_fill_then_continue", continuation_exact_fill_then_continue },
	{ "continuation", "zero_frame_recording", continuation_zero_frame_recording },
	{ "continuation", "real_short_tail", continuation_real_short_tail },
	{ "consumer", "read_patterns_match_written_prefix", consumer_read_patterns_match_written_prefix },
	{ "consumer", "uncompressed_pcm_regression", consumer_uncompressed_pcm_regression },
	{ "consumer", "alac_roundtrip_without_sync", consumer_alac_roundtrip_without_sync }
};

const size_t task_case_count = sizeof task_cases / sizeof task_cases[0];
