/*
	Regression tests for live CAF/ALAC recording with periodic afSyncFile.

	The tests exercise the provisional tail-packet path: PCM which does
	not fill a complete ALAC packet must be made readable by each sync,
	must never be counted more than once in the packet table, and must
	survive repeated syncs, continued writes and the final close without
	duplicated or dropped samples.

	Cases are registered through TASK_CHECK and driven by task_support/
	driver.c; this translation unit intentionally has no main().
*/

#include "../task_support/check.h"

#include <audiofile.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

static const int kFramesPerPacket = 4096;

// Deterministic virtual 32-bit integer PCM, frame 0 based, channel 0 based.
static int32_t pcmValue(int frame, int channel)
{
	// 64-bit intermediate then narrow: the full-scale product would
	// overflow a signed 32-bit value, but the intended result is its
	// two's-complement 32-bit representation.
	int64_t scaled = (((frame * 131LL + channel * 997LL) % 40001) - 20000) * 65536;
	return static_cast<int32_t>(scaled);
}

// What a 32-bit sample becomes after a round trip through an N-bit file.
static int32_t expectedValue(int frame, int channel, int bits)
{
	int32_t v = pcmValue(frame, channel);
	if (bits == 16)
		return static_cast<int32_t>(static_cast<uint32_t>(v) & 0xFFFF0000u);
	if (bits == 24)
		return static_cast<int32_t>(static_cast<uint32_t>(v) & 0xFFFFFF00u);
	return v;
}

static void tempPath(char *buffer, size_t size, const char *tag)
{
	const char *dir = getenv("TMPDIR");
	if (!dir || !dir[0])
		dir = "/tmp";
	snprintf(buffer, size, "%s/caf-sync-%s-%d.caf", dir, tag, (int) getpid());
}

static AFfilesetup makeALACSetup(int bits, int channels)
{
	AFfilesetup setup = afNewFileSetup();
	afInitFileFormat(setup, AF_FILE_CAF);
	afInitChannels(setup, AF_DEFAULT_TRACK, channels);
	afInitSampleFormat(setup, AF_DEFAULT_TRACK, AF_SAMPFMT_TWOSCOMP, bits);
	afInitCompression(setup, AF_DEFAULT_TRACK, AF_COMPRESSION_ALAC);
	return setup;
}

static AFfilehandle openALACWriter(const char *path, int bits, int channels)
{
	AFfilesetup setup = makeALACSetup(bits, channels);
	AFfilehandle handle = afOpenFile(path, "w", setup);
	afFreeFileSetup(setup);
	return handle;
}

static int writePcm(AFfilehandle handle, int channels, long start, long count)
{
	std::vector<int32_t> buffer(count * channels);
	for (long frame = 0; frame < count; ++frame)
		for (int channel = 0; channel < channels; ++channel)
			buffer[frame * channels + channel] = pcmValue(start + frame, channel);
	TASK_CHECK(afWriteFrames(handle, AF_DEFAULT_TRACK, buffer.data(), count) ==
		count);
	return 0;
}

static int readFrameCount(const char *path, long *count)
{
	AFfilehandle handle = afOpenFile(path, "r", 0);
	TASK_CHECK(handle != AF_NULL_FILEHANDLE);
	*count = afGetFrameCount(handle, AF_DEFAULT_TRACK);
	TASK_CHECK(afCloseFile(handle) == 0);
	return 0;
}

static int checkFrameCount(const char *path, long expected)
{
	long count = 0;
	TASK_CHECK(readFrameCount(path, &count) == 0);
	TASK_CHECK(count == expected);
	return 0;
}

// Read the file back through a fresh handle and verify every sample using
// one-frame, small and cross-packet sequential reads.
static int verifySamples(const char *path, int bits, int channels, long total)
{
	AFfilehandle handle = afOpenFile(path, "r", 0);
	TASK_CHECK(handle != AF_NULL_FILEHANDLE);
	TASK_CHECK(afSetVirtualSampleFormat(handle, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) == 0);
	TASK_CHECK(afGetFrameCount(handle, AF_DEFAULT_TRACK) == total);

	if (total == 0)
	{
		int32_t dummy[2];
		TASK_CHECK(afReadFrames(handle, AF_DEFAULT_TRACK, dummy, 1) == 0);
		TASK_CHECK(afCloseFile(handle) == 0);
		return 0;
	}

	static const int kSizes[] = {1, 3, 7, 100, 1024, 4095, 4096, 4097, 5000};
	long position = 0;
	size_t sizeIndex = 0;
	while (position < total)
	{
		int wanted = std::min<long>(kSizes[sizeIndex++ % 9], total - position);
		std::vector<int32_t> buffer(wanted * channels);
		long got = afReadFrames(handle, AF_DEFAULT_TRACK, buffer.data(), wanted);
		TASK_CHECK(got == wanted);
		for (long frame = 0; frame < got; ++frame)
			for (int channel = 0; channel < channels; ++channel)
				TASK_CHECK(buffer[frame * channels + channel] ==
					expectedValue(position + frame, channel, bits));
		position += got;
	}

	// Reads at the valid end terminate with zero frames.
	int32_t atEnd[2];
	TASK_CHECK(afReadFrames(handle, AF_DEFAULT_TRACK, atEnd, 1) == 0);

	TASK_CHECK(afCloseFile(handle) == 0);
	return 0;
}

// Random positioning across packet boundaries must return exact samples.
static int verifySeeks(const char *path, int bits, int channels, long total)
{
	if (total <= 0)
		return 0;
	AFfilehandle handle = afOpenFile(path, "r", 0);
	TASK_CHECK(handle != AF_NULL_FILEHANDLE);
	TASK_CHECK(afSetVirtualSampleFormat(handle, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) == 0);

	long targets[] = {0, 1, total - 1, 4095, 4096, 4097, 8191, 8192, 8193};
	for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
	{
		long target = targets[i];
		if (target >= total)
			continue;
		TASK_CHECK(afSeekFrame(handle, AF_DEFAULT_TRACK, target) == target);
		int wanted = std::min<long>(5, total - target);
		std::vector<int32_t> buffer(wanted * channels);
		TASK_CHECK(afReadFrames(handle, AF_DEFAULT_TRACK, buffer.data(), wanted) ==
			wanted);
		for (long frame = 0; frame < wanted; ++frame)
			for (int channel = 0; channel < channels; ++channel)
				TASK_CHECK(buffer[frame * channels + channel] ==
					expectedValue(target + frame, channel, bits));
	}
	TASK_CHECK(afCloseFile(handle) == 0);
	return 0;
}

static bool readWholeFile(const char *path, std::vector<unsigned char> &out)
{
	FILE *file = fopen(path, "rb");
	if (!file)
		return false;
	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	out.resize(length);
	bool ok = fread(out.data(), 1, length, file) == static_cast<size_t>(length);
	fclose(file);
	return ok;
}

static uint32_t readBER(const unsigned char *data, size_t available,
	size_t *used)
{
	uint32_t value = 0;
	size_t index = 0;
	do
	{
		unsigned char byte = data[index];
		value = (value << 7) | (byte & 0x7f);
		if (++index > 5)
			break;
	} while ((data[index - 1] & 0x80) && index < available);
	*used = index;
	return value;
}

// Independent CAF parse: one data and one pakt chunk, data length matching
// the sum of the variable-length packet sizes, valid frame count, no tail.
static int verifyBlockTable(const char *path, long expectedFrames)
{
	std::vector<unsigned char> bytes;
	TASK_CHECK(readWholeFile(path, bytes));
	const long length = static_cast<long>(bytes.size());
	TASK_CHECK(length >= 12);
	TASK_CHECK(memcmp(bytes.data(), "caff", 4) == 0);

	long offset = 8;
	int dataChunks = 0, packetTableChunks = 0;
	long dataBody = -1;
	int64_t declaredAudioBytes = -1;
	int64_t packetSizeSum = -1;
	int64_t parsedValidFrames = -1;
	int64_t parsedPackets = -1;

	while (offset + 12 <= length)
	{
		char tag[5] = {0};
		memcpy(tag, bytes.data() + offset, 4);
		int64_t chunkLength = 0;
		for (int i = 0; i < 8; ++i)
			chunkLength = (chunkLength << 8) | bytes[offset + 4 + i];
		long body = offset + 12;
		TASK_CHECK(chunkLength >= 0);

		if (!strcmp(tag, "data"))
		{
			++dataChunks;
			dataBody = body;
			// A synchronized file carries an explicit data length that
			// always includes the 4-byte edit count.
			TASK_CHECK(chunkLength >= 4);
			declaredAudioBytes = chunkLength - 4;
		}
		else if (!strcmp(tag, "pakt"))
		{
			++packetTableChunks;
			int64_t numPackets = 0, numValidFrames = 0;
			for (int i = 0; i < 8; ++i)
				numPackets = (numPackets << 8) | bytes[body + i];
			for (int i = 0; i < 8; ++i)
				numValidFrames = (numValidFrames << 8) | bytes[body + 8 + i];
			parsedPackets = numPackets;
			parsedValidFrames = numValidFrames;

			size_t position = body + 24;
			size_t end = body + chunkLength;
			int64_t sum = 0;
			int64_t entries = 0;
			while (position < end)
			{
				size_t used = 0;
				sum += readBER(bytes.data() + position, end - position, &used);
				position += used;
				++entries;
			}
			TASK_CHECK(entries == numPackets);
			packetSizeSum = sum;

			// Audio bytes occupy the region after the edit count up to
			// the single packet-table chunk header.
			TASK_CHECK(dataBody >= 0);
			TASK_CHECK(offset - (dataBody + 4) == sum);
			TASK_CHECK(declaredAudioBytes == sum);
		}

		offset = body + chunkLength;
	}

	TASK_CHECK(offset == length); // no stale chunks or trailing bytes
	TASK_CHECK(dataChunks == 1);
	TASK_CHECK(packetTableChunks == 1);
	TASK_CHECK(parsedValidFrames == expectedFrames);
	TASK_CHECK(packetSizeSum >= 0);
	TASK_CHECK(parsedPackets >= 0);
	return 0;
}

// ---------------------------------------------------------------------------
// snapshots group
// ---------------------------------------------------------------------------

static int snapshotBlockTable(void)
{
	const int bitsSet[] = {16, 24};
	const int channelSet[] = {1, 2};
	const long segments[] = {17, 4079, 1, 9000, 3};

	for (size_t b = 0; b < 2; ++b)
		for (size_t c = 0; c < 2; ++c)
		{
			char path[256];
			tempPath(path, sizeof(path), "snap-table");
			unlink(path);
			AFfilehandle handle = openALACWriter(path, bitsSet[b], channelSet[c]);
			TASK_CHECK(handle != AF_NULL_FILEHANDLE);
			TASK_CHECK(afSetVirtualSampleFormat(handle, AF_DEFAULT_TRACK,
				AF_SAMPFMT_TWOSCOMP, 32) == 0);

			long total = 0;
			for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); ++i)
			{
				TASK_CHECK(writePcm(handle, channelSet[c], total, segments[i]) == 0);
				total += segments[i];
				TASK_CHECK(afSyncFile(handle) == 0);
				TASK_CHECK(verifyBlockTable(path, total) == 0);
			}
			TASK_CHECK(afCloseFile(handle) == 0);
			TASK_CHECK(verifyBlockTable(path, total) == 0);
			unlink(path);
		}
	return 0;
}

static int snapshotIdempotentSync(void)
{
	const long segments[] = {17, 4079, 1, 9000, 3};
	char path[256];
	tempPath(path, sizeof(path), "snap-idem");
	unlink(path);

	AFfilehandle handle = openALACWriter(path, 16, 2);
	TASK_CHECK(handle != AF_NULL_FILEHANDLE);
	TASK_CHECK(afSetVirtualSampleFormat(handle, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) == 0);

	long total = 0;
	for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); ++i)
	{
		TASK_CHECK(writePcm(handle, 2, total, segments[i]) == 0);
		total += segments[i];
		TASK_CHECK(afSyncFile(handle) == 0);

		std::vector<unsigned char> before;
		TASK_CHECK(readWholeFile(path, before));
		TASK_CHECK(checkFrameCount(path, total) == 0);

		// Repeated syncs without new PCM must not touch a single byte.
		TASK_CHECK(afSyncFile(handle) == 0);
		std::vector<unsigned char> after;
		TASK_CHECK(readWholeFile(path, after));
		TASK_CHECK(after == before);
		TASK_CHECK(checkFrameCount(path, total) == 0);
		TASK_CHECK(verifyBlockTable(path, total) == 0);
	}
	TASK_CHECK(afCloseFile(handle) == 0);
	unlink(path);
	return 0;
}

// ---------------------------------------------------------------------------
// continuation group
// ---------------------------------------------------------------------------

static int continuationShortTail(void)
{
	const long segments[] = {17, 4079, 1, 9000, 3};
	const int bitsSet[] = {16, 24};
	const int channelSet[] = {1, 2};

	for (size_t b = 0; b < 2; ++b)
		for (size_t c = 0; c < 2; ++c)
		{
			char path[256];
			tempPath(path, sizeof(path), "cont-tail");
			unlink(path);
			AFfilehandle handle = openALACWriter(path, bitsSet[b], channelSet[c]);
			TASK_CHECK(handle != AF_NULL_FILEHANDLE);
			TASK_CHECK(afSetVirtualSampleFormat(handle, AF_DEFAULT_TRACK,
				AF_SAMPFMT_TWOSCOMP, 32) == 0);

			long total = 0;
			for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); ++i)
			{
				TASK_CHECK(writePcm(handle, channelSet[c], total, segments[i]) == 0);
				total += segments[i];
				TASK_CHECK(afSyncFile(handle) == 0);
				TASK_CHECK(checkFrameCount(path, total) == 0);
				TASK_CHECK(verifySamples(path, bitsSet[b], channelSet[c], total) == 0);
				TASK_CHECK(verifyBlockTable(path, total) == 0);
			}
			TASK_CHECK(afCloseFile(handle) == 0);
			TASK_CHECK(checkFrameCount(path, total) == 0);
			TASK_CHECK(verifySamples(path, bitsSet[b], channelSet[c], total) == 0);
			TASK_CHECK(verifySeeks(path, bitsSet[b], channelSet[c], total) == 0);
			unlink(path);
		}
	return 0;
}

static int continuationBoundaries(void)
{
	char path[256];

	// Writes that land exactly on packet boundaries must never produce a
	// provisional tail.
	tempPath(path, sizeof(path), "cont-exact");
	unlink(path);
	AFfilehandle handle = openALACWriter(path, 16, 2);
	TASK_CHECK(handle != AF_NULL_FILEHANDLE);
	TASK_CHECK(afSetVirtualSampleFormat(handle, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) == 0);
	const long exact[] = {kFramesPerPacket, kFramesPerPacket, kFramesPerPacket * 2};
	long total = 0;
	for (size_t i = 0; i < 3; ++i)
	{
		TASK_CHECK(writePcm(handle, 2, total, exact[i]) == 0);
		total += exact[i];
		TASK_CHECK(afSyncFile(handle) == 0);
		TASK_CHECK(checkFrameCount(path, total) == 0);
		TASK_CHECK(verifyBlockTable(path, total) == 0);
	}
	TASK_CHECK(afCloseFile(handle) == 0);
	TASK_CHECK(verifySamples(path, 16, 2, total) == 0);
	unlink(path);

	// A short tail followed by enough PCM to cross several packets.
	tempPath(path, sizeof(path), "cont-grow");
	unlink(path);
	handle = openALACWriter(path, 24, 1);
	TASK_CHECK(handle != AF_NULL_FILEHANDLE);
	TASK_CHECK(afSetVirtualSampleFormat(handle, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) == 0);
	TASK_CHECK(writePcm(handle, 1, 0, 5) == 0);
	TASK_CHECK(afSyncFile(handle) == 0);
	TASK_CHECK(checkFrameCount(path, 5) == 0);
	TASK_CHECK(writePcm(handle, 1, 5, 20000) == 0);
	TASK_CHECK(afSyncFile(handle) == 0);
	TASK_CHECK(checkFrameCount(path, 20005) == 0);
	TASK_CHECK(afCloseFile(handle) == 0);
	TASK_CHECK(verifySamples(path, 24, 1, 20005) == 0);
	TASK_CHECK(verifyBlockTable(path, 20005) == 0);
	unlink(path);

	// Zero-frame recording: syncs and close expose no audio.
	tempPath(path, sizeof(path), "cont-zero");
	unlink(path);
	handle = openALACWriter(path, 16, 1);
	TASK_CHECK(handle != AF_NULL_FILEHANDLE);
	TASK_CHECK(afSyncFile(handle) == 0);
	TASK_CHECK(checkFrameCount(path, 0) == 0);
	TASK_CHECK(afSyncFile(handle) == 0);
	TASK_CHECK(checkFrameCount(path, 0) == 0);
	TASK_CHECK(afCloseFile(handle) == 0);
	TASK_CHECK(checkFrameCount(path, 0) == 0);
	TASK_CHECK(verifySamples(path, 16, 1, 0) == 0);
	TASK_CHECK(verifyBlockTable(path, 0) == 0);
	unlink(path);
	return 0;
}

// Normal full-packet ALAC plus an uncompressed CAF PCM round trip must keep
// behaving exactly as before the live-sync changes.
static int continuationNormalPcm(void)
{
	char path[256];
	tempPath(path, sizeof(path), "cont-normal-alac");
	unlink(path);
	const long total = 82421;
	AFfilehandle handle = openALACWriter(path, 16, 1);
	TASK_CHECK(handle != AF_NULL_FILEHANDLE);
	TASK_CHECK(afSetVirtualSampleFormat(handle, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) == 0);
	TASK_CHECK(writePcm(handle, 1, 0, total) == 0);
	TASK_CHECK(afCloseFile(handle) == 0);
	TASK_CHECK(checkFrameCount(path, total) == 0);
	TASK_CHECK(verifySamples(path, 16, 1, total) == 0);
	TASK_CHECK(verifySeeks(path, 16, 1, total) == 0);
	unlink(path);

	// Uncompressed CAF PCM is untouched by the ALAC tail logic.
	tempPath(path, sizeof(path), "cont-normal-pcm");
	unlink(path);
	AFfilesetup setup = afNewFileSetup();
	afInitFileFormat(setup, AF_FILE_CAF);
	afInitChannels(setup, AF_DEFAULT_TRACK, 2);
	afInitSampleFormat(setup, AF_DEFAULT_TRACK, AF_SAMPFMT_TWOSCOMP, 16);
	handle = afOpenFile(path, "w", setup);
	afFreeFileSetup(setup);
	TASK_CHECK(handle != AF_NULL_FILEHANDLE);
	std::vector<int16_t> pcm(9001 * 2);
	for (long frame = 0; frame < 9001; ++frame)
		for (int channel = 0; channel < 2; ++channel)
			pcm[frame * 2 + channel] = (int16_t)(pcmValue(frame, channel) >> 16);
	TASK_CHECK(afWriteFrames(handle, AF_DEFAULT_TRACK, pcm.data(), 9001) == 9001);
	TASK_CHECK(afCloseFile(handle) == 0);
	TASK_CHECK(checkFrameCount(path, 9001) == 0);
	handle = afOpenFile(path, "r", 0);
	TASK_CHECK(handle != AF_NULL_FILEHANDLE);
	TASK_CHECK(afSeekFrame(handle, AF_DEFAULT_TRACK, 4097) == 4097);
	int16_t pair[2];
	TASK_CHECK(afReadFrames(handle, AF_DEFAULT_TRACK, pair, 1) == 1);
	TASK_CHECK(pair[0] == pcm[8194] && pair[1] == pcm[8195]);
	TASK_CHECK(afCloseFile(handle) == 0);
	unlink(path);
	return 0;
}

// ---------------------------------------------------------------------------
// consumer group
// ---------------------------------------------------------------------------

static int consumerReads(void)
{
	// A separate "consumer" only ever opens the file for reading, exactly
	// as an audition process would while recording continues.
	const long segments[] = {17, 4079, 1, 9000, 3};
	const int bitsSet[] = {16, 24};
	const int channelSet[] = {1, 2};

	for (size_t b = 0; b < 2; ++b)
		for (size_t c = 0; c < 2; ++c)
		{
			char path[256];
			tempPath(path, sizeof(path), "consumer-read");
			unlink(path);
			AFfilehandle writer = openALACWriter(path, bitsSet[b], channelSet[c]);
			TASK_CHECK(writer != AF_NULL_FILEHANDLE);
			TASK_CHECK(afSetVirtualSampleFormat(writer, AF_DEFAULT_TRACK,
				AF_SAMPFMT_TWOSCOMP, 32) == 0);
			long total = 0;
			for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); ++i)
			{
				TASK_CHECK(writePcm(writer, channelSet[c], total, segments[i]) == 0);
				total += segments[i];
				TASK_CHECK(afSyncFile(writer) == 0);
				TASK_CHECK(verifySamples(path, bitsSet[b], channelSet[c], total) == 0);
			}
			TASK_CHECK(afCloseFile(writer) == 0);
			TASK_CHECK(verifySamples(path, bitsSet[b], channelSet[c], total) == 0);
			unlink(path);
		}
	return 0;
}

static int consumerSeeks(void)
{
	char path[256];
	tempPath(path, sizeof(path), "consumer-seek");
	unlink(path);
	const long segments[] = {17, 4079, 1, 9000, 3};
	AFfilehandle writer = openALACWriter(path, 24, 2);
	TASK_CHECK(writer != AF_NULL_FILEHANDLE);
	TASK_CHECK(afSetVirtualSampleFormat(writer, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) == 0);
	long total = 0;
	for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); ++i)
	{
		TASK_CHECK(writePcm(writer, 2, total, segments[i]) == 0);
		total += segments[i];
		TASK_CHECK(afSyncFile(writer) == 0);
		TASK_CHECK(verifySeeks(path, 24, 2, total) == 0);
	}
	TASK_CHECK(afCloseFile(writer) == 0);
	TASK_CHECK(verifySeeks(path, 24, 2, total) == 0);
	TASK_CHECK(verifyBlockTable(path, total) == 0);
	unlink(path);
	return 0;
}

// C-linkage wrappers so the C driver's task_case table can reference the
// C++ test functions.
extern "C" {
static int run_snapshot_block_table(void) { return snapshotBlockTable(); }
static int run_snapshot_idempotent_sync(void) { return snapshotIdempotentSync(); }
static int run_continuation_short_tail(void) { return continuationShortTail(); }
static int run_continuation_boundaries(void) { return continuationBoundaries(); }
static int run_continuation_normal_pcm(void) { return continuationNormalPcm(); }
static int run_consumer_reads(void) { return consumerReads(); }
static int run_consumer_seeks(void) { return consumerSeeks(); }
}

const struct task_case task_cases[] =
{
	{"snapshots", "block_table", run_snapshot_block_table},
	{"snapshots", "idempotent_sync", run_snapshot_idempotent_sync},

	{"continuation", "short_tail_continuation", run_continuation_short_tail},
	{"continuation", "boundary_conditions", run_continuation_boundaries},
	{"continuation", "normal_pcm_regression", run_continuation_normal_pcm},

	{"consumer", "consumer_reads", run_consumer_reads},
	{"consumer", "consumer_seeks", run_consumer_seeks},
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
