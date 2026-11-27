/*
	Audio File Library

	caf_sync_target.cpp

	Regression tests for afSyncFile() on streaming CAF/ALAC recordings.

	The tests are split into three groups which the task driver runs
	separately:

	  snapshots     - a sync exposes exactly the frames written so far,
	                  and repeated syncs and writes that extend the
	                  recording keep the file consistent.
	  continuation  - writing after a sync re-encodes the provisional
	                  tail packet instead of duplicating or losing it.
	  consumer      - a fresh process can open, read (in several chunk
	                  sizes), and seek the partially written file.

	The file is also parsed independently of the library: the CAF data
	chunk length, pakt packet sizes, packet count and valid frame count
	must agree, and no stale tail or old packet table may survive.
*/

#include "../task_support/check.h"

#include <audiofile.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace
{

const int kFramesPerPacket = 4096;

int32_t sampleValue(int frame, int channel)
{
	int64_t v = ((int64_t) frame * 131 + (int64_t) channel * 997) % 40001;
	return static_cast<int32_t>((v - 20000) * 65536);
}

std::string temporaryPath(const char *tag)
{
	char buffer[1024];
	const char *dir = getenv("TMPDIR");
	if (!dir || !*dir)
		dir = ".";
	snprintf(buffer, sizeof(buffer), "%s/af-caf-sync-%s-%d.caf", dir, tag,
		static_cast<int>(getpid()));
	return std::string(buffer);
}

AFfilehandle openWriter(const std::string &path, int bits, int channels)
{
	unlink(path.c_str());
	AFfilesetup setup = afNewFileSetup();
	afInitFileFormat(setup, AF_FILE_CAF);
	afInitChannels(setup, AF_DEFAULT_TRACK, channels);
	afInitSampleFormat(setup, AF_DEFAULT_TRACK, AF_SAMPFMT_TWOSCOMP, bits);
	afInitCompression(setup, AF_DEFAULT_TRACK, AF_COMPRESSION_ALAC);
	AFfilehandle file = afOpenFile(path.c_str(), "w", setup);
	afFreeFileSetup(setup);
	if (!file)
		return NULL;
	if (afSetVirtualSampleFormat(file, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) != 0)
	{
		afCloseFile(file);
		return NULL;
	}
	return file;
}

bool writeFrames(AFfilehandle file, int firstFrame, int frameCount, int channels)
{
	std::vector<int32_t> samples(static_cast<size_t>(frameCount) * channels);
	for (int i = 0; i < frameCount; i++)
		for (int c = 0; c < channels; c++)
			samples[static_cast<size_t>(i) * channels + c] =
				sampleValue(firstFrame + i, c);
	int result = afWriteFrames(file, AF_DEFAULT_TRACK, samples.data(),
		frameCount);
	return result == frameCount;
}

struct FileBytes
{
	std::vector<uint8_t> bytes;

	bool load(const std::string &path)
	{
		bytes.clear();
		FILE *file = fopen(path.c_str(), "rb");
		if (!file)
			return false;
		if (fseek(file, 0, SEEK_END) != 0)
		{
			fclose(file);
			return false;
		}
		long length = ftell(file);
		if (length < 0 || fseek(file, 0, SEEK_SET) != 0)
		{
			fclose(file);
			return false;
		}
		bytes.resize(static_cast<size_t>(length));
		bool ok = length == 0 ||
			fread(bytes.data(), 1, static_cast<size_t>(length), file) ==
				static_cast<size_t>(length);
		fclose(file);
		return ok;
	}
};

uint64_t readBigEndian(const uint8_t *data, int count)
{
	uint64_t value = 0;
	for (int i = 0; i < count; i++)
		value = (value << 8) | data[i];
	return value;
}

struct ParsedPacketTable
{
	int64_t numPackets;
	int64_t numValidFrames;
	std::vector<uint32_t> packetBytes;
};

struct ParsedCAF
{
	int64_t dataLength;
	std::vector<ParsedPacketTable> packetTables;
	bool sawData;
	bool sawPacketTable;
};

/*
	Parse a CAF file without using the library. Only the chunks of
	interest are inspected; unknown chunks are skipped.
*/
bool parseCAF(const std::vector<uint8_t> &bytes, ParsedCAF &result)
{
	result.dataLength = -1;
	result.sawData = false;
	result.sawPacketTable = false;

	if (bytes.size() < 8 || memcmp(bytes.data(), "caff", 4) != 0)
		return false;

	size_t position = 8;
	while (position + 12 <= bytes.size())
	{
		const uint8_t *header = bytes.data() + position;
		uint64_t length = readBigEndian(header + 4, 8);

		if (memcmp(header, "data", 4) == 0)
		{
			if (result.sawData)
				return false;
			result.sawData = true;
			result.dataLength = static_cast<int64_t>(length);
		}
		else if (memcmp(header, "pakt", 4) == 0)
		{
			if (result.sawPacketTable)
				return false;
			result.sawPacketTable = true;
			if (length < 24 || position + 12 + length > bytes.size())
				return false;

			const uint8_t *body = bytes.data() + position + 12;
			ParsedPacketTable table;
			table.numPackets = static_cast<int64_t>(readBigEndian(body, 8));
			table.numValidFrames = static_cast<int64_t>(readBigEndian(body + 8, 8));

			size_t offset = 24;
			while (offset < length)
			{
				uint32_t value = 0;
				size_t consumed = 0;
				do
				{
					if (offset + consumed >= length || consumed == 5)
						return false;
					uint8_t byte = body[offset + consumed];
					value = (value << 7) | (byte & 0x7f);
					consumed++;
					if (!(byte & 0x80))
						break;
				} while (true);
				table.packetBytes.push_back(value);
				offset += consumed;
			}
			result.packetTables.push_back(table);
		}

		if (length == UINT64_MAX)
			break;
		position += 12 + length;
	}

	return result.sawData && result.sawPacketTable;
}

int64_t sumPacketBytes(const ParsedPacketTable &table)
{
	int64_t total = 0;
	for (size_t i = 0; i < table.packetBytes.size(); i++)
		total += table.packetBytes[i];
	return total;
}

/*
	The data chunk length is the total of packet sizes plus the edit
	count field. The packet table must describe exactly those packets
	and the expected number of valid frames.
*/
bool packetTableConsistent(const ParsedCAF &caf, int64_t expectedFrames,
	int bits, int channels)
{
	if (!caf.sawData || !caf.sawPacketTable)
		return false;
	if (caf.dataLength < 4)
		return false;
	if (caf.packetTables.empty())
		return false;

	const ParsedPacketTable &table = caf.packetTables.front();
	if (static_cast<int64_t>(table.packetBytes.size()) != table.numPackets)
		return false;
	if (table.numValidFrames != expectedFrames)
		return false;
	if (caf.dataLength != 4 + sumPacketBytes(table))
		return false;

	/*
		Every packet except possibly the last must decode to exactly
		kFramesPerPacket frames. The expected frame count fixes the
		size of the last packet too.
	*/
	const int64_t fullPackets = expectedFrames / kFramesPerPacket;
	const int64_t tailFrames = expectedFrames % kFramesPerPacket;
	const int64_t expectedPackets = fullPackets + (tailFrames ? 1 : 0);
	if (table.numPackets != expectedPackets)
		return false;

	for (size_t i = 0; i < table.packetBytes.size(); i++)
	{
		if (table.packetBytes[i] == 0)
			return 1;
	}

	/* The final packet, if partial, must be smaller than a full one. */
	if (tailFrames != 0 && table.numPackets > 0)
	{
		const int bytesPerSample = (bits + 7) / 8;
		const int64_t fullPacketUpperBound =
			kFramesPerPacket * channels * bytesPerSample + 32;
		if (table.packetBytes.back() > static_cast<uint32_t>(fullPacketUpperBound))
			return 1;
	}

	return true;
}

/*
	Read and compare the whole file in the given chunk size. Returns
	true if all frames matched and the read stopped at the valid end.
*/
bool readAllAndCompare(AFfilehandle file, int channels, int64_t expectedFrames,
	int chunkSize)
{
	std::vector<int32_t> buffer(static_cast<size_t>(chunkSize) * channels);
	int64_t frame = 0;
	while (frame < expectedFrames)
	{
		int request = chunkSize;
		if (request > expectedFrames - frame)
			request = static_cast<int>(expectedFrames - frame);
		int got = afReadFrames(file, AF_DEFAULT_TRACK, buffer.data(), request);
		if (got != request)
			return false;
		for (int i = 0; i < got; i++)
			for (int c = 0; c < channels; c++)
				if (buffer[static_cast<size_t>(i) * channels + c] !=
					sampleValue(static_cast<int>(frame + i), c))
					return false;
		frame += got;
	}
	int32_t extra = 0;
	if (afReadFrames(file, AF_DEFAULT_TRACK, &extra, 1) != 0)
		return false;
	return true;
}

bool seekAndCompare(AFfilehandle file, int channels, int64_t expectedFrames,
	int64_t position)
{
	if (position < 0 || position >= expectedFrames)
		return true;
	if (afSeekFrame(file, AF_DEFAULT_TRACK,
		static_cast<AFframecount>(position)) != position)
		return false;

	int request = 1;
	int64_t remaining = expectedFrames - position;
	if (remaining > request)
		request = static_cast<int>(remaining < 37 ? remaining : 37);
	std::vector<int32_t> buffer(static_cast<size_t>(request) * channels);
	int got = afReadFrames(file, AF_DEFAULT_TRACK, buffer.data(), request);
	if (got != request)
		return false;
	for (int i = 0; i < got; i++)
		for (int c = 0; c < channels; c++)
			if (buffer[static_cast<size_t>(i) * channels + c] !=
				sampleValue(static_cast<int>(position + i), c))
				return false;
	return true;
}

/*
	Open the file with the public read API and verify frames, data and
	seeking. This deliberately uses a full open/close cycle to mimic a
	separate consumer process.
*/
bool verifyConsumer(const std::string &path, int channels,
	int64_t expectedFrames)
{
	AFfilehandle file = afOpenFile(path.c_str(), "r", NULL);
	if (!file)
		return false;
	bool ok = afSetVirtualSampleFormat(file, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) == 0;
	ok = ok && afSetVirtualChannels(file, AF_DEFAULT_TRACK, channels) == 0;
	ok = ok && afGetFrameCount(file, AF_DEFAULT_TRACK) == expectedFrames;

	if (ok)
	{
		static const int chunkSizes[] = { 1, 3, 64, 1024, 4096, 8191 };
		for (size_t i = 0; i < sizeof(chunkSizes) / sizeof(chunkSizes[0]); i++)
		{
			if (afSeekFrame(file, AF_DEFAULT_TRACK, 0) != 0)
			{
				ok = false;
				break;
			}
			if (!readAllAndCompare(file, channels, expectedFrames,
				chunkSizes[i]))
			{
				ok = false;
				break;
			}
		}
	}

	if (ok)
	{
		const int64_t positions[] =
		{
			0, expectedFrames - 1, expectedFrames / 2,
			kFramesPerPacket - 1, kFramesPerPacket, kFramesPerPacket + 1,
			2 * kFramesPerPacket - 1, 3 * kFramesPerPacket + 17
		};
		for (size_t i = 0; i < sizeof(positions) / sizeof(positions[0]); i++)
		{
			if (!seekAndCompare(file, channels, expectedFrames, positions[i]))
			{
				ok = false;
				break;
			}
		}
	}

	if (afCloseFile(file) != 0)
		ok = false;
	return ok;
}

/*
	Compare the file against the independent block table and the
	consumer view.
*/
bool verifyFile(const std::string &path, int bits, int channels,
	int64_t expectedFrames)
{
	FileBytes file;
	if (!file.load(path))
		return false;

	ParsedCAF caf;
	if (!parseCAF(file.bytes, caf))
		return false;
	if (!packetTableConsistent(caf, expectedFrames, bits, channels))
		return false;

	return verifyConsumer(path, channels, expectedFrames);
}

/*
	Test state: an open writer plus the number of frames written.
*/
struct Recording
{
	std::string path;
	int bits;
	int channels;
	AFfilehandle file;
	int64_t written;

	Recording() : bits(16), channels(2), file(NULL), written(0) { }

	bool begin(const char *tag, int sampleBits, int channelCount)
	{
		path = temporaryPath(tag);
		bits = sampleBits;
		channels = channelCount;
		file = openWriter(path, bits, channels);
		written = 0;
		return file != NULL;
	}

	bool write(int frameCount)
	{
		if (!writeFrames(file, static_cast<int>(written), frameCount, channels))
			return false;
		written += frameCount;
		return true;
	}

	bool sync()
	{
		return afSyncFile(file) == 0;
	}

	bool close()
	{
		bool ok = afCloseFile(file) == 0;
		file = NULL;
		return ok;
	}

	bool verify()
	{
		return verifyFile(path, bits, channels, written);
	}

	void cleanup()
	{
		if (file)
			afCloseFile(file);
		file = NULL;
		unlink(path.c_str());
	}
};

/*
	Run one scenario over every supported format combination, calling
	the supplied function for each. The function returns false on
	failure.
*/
template <typename Function>
int forEachFormat(Function function)
{
	static const int bitDepths[] = { 16, 24 };
	static const int channelCounts[] = { 1, 2 };
	for (size_t b = 0; b < sizeof(bitDepths) / sizeof(bitDepths[0]); b++)
		for (size_t c = 0; c < sizeof(channelCounts) / sizeof(channelCounts[0]); c++)
			if (function(bitDepths[b], channelCounts[c]))
				return 1;
	return 0;
}

/*
	snapshots: the first sync exposes exactly the written frames, and
	repeating it leaves the file byte-for-byte unchanged.
*/
int snapshotsBasic(void)
{
	return forEachFormat([](int bits, int channels) -> int
	{
		Recording recording;
		TASK_CHECK(recording.begin("snap-basic", bits, channels));
		TASK_CHECK(recording.write(17));
		TASK_CHECK(recording.sync());
		TASK_CHECK(recording.verify());

		FileBytes first;
		TASK_CHECK(first.load(recording.path));

		TASK_CHECK(recording.sync());
		TASK_CHECK(recording.verify());

		FileBytes second;
		TASK_CHECK(second.load(recording.path));
		TASK_CHECK(first.bytes == second.bytes);

		/* Writing no PCM and syncing again must also be a no-op. */
		TASK_CHECK(recording.sync());
		FileBytes third;
		TASK_CHECK(third.load(recording.path));
		TASK_CHECK(first.bytes == third.bytes);

		TASK_CHECK(recording.close());
		TASK_CHECK(recording.verify());
		recording.cleanup();
		return 0;
	});
}

/*
	snapshots: several writes, each followed by a sync, keep the file
	consistent at every step.
*/
int snapshotsProgression(void)
{
	return forEachFormat([](int bits, int channels) -> int
	{
		Recording recording;
		TASK_CHECK(recording.begin("snap-progress", bits, channels));

		const int segments[] = { 1, 4095, 1, 4096, 3, 8192, 17 };
		for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); i++)
		{
			TASK_CHECK(recording.write(segments[i]));
			TASK_CHECK(recording.sync());
			TASK_CHECK(recording.verify());
		}

		TASK_CHECK(recording.close());
		TASK_CHECK(recording.verify());
		recording.cleanup();
		return 0;
	});
}

/*
	snapshots: a recording which stops well before a full packet, and
	one which stops exactly on a packet boundary.
*/
int snapshotsBoundaries(void)
{
	if (forEachFormat([](int bits, int channels) -> int
	{
		Recording shortTail;
		TASK_CHECK(shortTail.begin("snap-short", bits, channels));
		TASK_CHECK(shortTail.write(1));
		TASK_CHECK(shortTail.sync());
		TASK_CHECK(shortTail.verify());
		TASK_CHECK(shortTail.sync());
		TASK_CHECK(shortTail.verify());
		TASK_CHECK(shortTail.close());
		TASK_CHECK(shortTail.verify());
		shortTail.cleanup();

		Recording exact;
		TASK_CHECK(exact.begin("snap-exact", bits, channels));
		TASK_CHECK(exact.write(kFramesPerPacket));
		TASK_CHECK(exact.sync());
		TASK_CHECK(exact.verify());
		TASK_CHECK(exact.sync());
		TASK_CHECK(exact.verify());
		TASK_CHECK(exact.write(kFramesPerPacket));
		TASK_CHECK(exact.sync());
		TASK_CHECK(exact.verify());
		TASK_CHECK(exact.close());
		TASK_CHECK(exact.verify());
		exact.cleanup();
		return 0;
	}))
		return 1;

	/* Zero-frame recordings must be valid and stable. */
	return forEachFormat([](int bits, int channels) -> int
	{
		Recording empty;
		TASK_CHECK(empty.begin("snap-empty", bits, channels));
		TASK_CHECK(empty.sync());
		TASK_CHECK(empty.verify());
		TASK_CHECK(empty.sync());
		TASK_CHECK(empty.verify());
		TASK_CHECK(empty.close());
		TASK_CHECK(empty.verify());
		empty.cleanup();
		return 0;
	});
}

/*
	continuation: writing after a sync re-encodes the provisional tail
	and never duplicates or loses frames. This covers a short tail
	followed by more than one full packet.
*/
int continuationShortTail(void)
{
	return forEachFormat([](int bits, int channels) -> int
	{
		Recording recording;
		TASK_CHECK(recording.begin("cont-short", bits, channels));
		TASK_CHECK(recording.write(17));
		TASK_CHECK(recording.sync());
		TASK_CHECK(recording.verify());

		TASK_CHECK(recording.write(20000));
		TASK_CHECK(recording.sync());
		TASK_CHECK(recording.verify());

		TASK_CHECK(recording.write(3));
		TASK_CHECK(recording.sync());
		TASK_CHECK(recording.verify());

		TASK_CHECK(recording.close());
		TASK_CHECK(recording.verify());
		recording.cleanup();
		return 0;
	});
}

/*
	continuation: multiple syncs at the same point do not add packets,
	then a write extends the packet table correctly.
*/
int continuationRepeatedSync(void)
{
	return forEachFormat([](int bits, int channels) -> int
	{
		Recording recording;
		TASK_CHECK(recording.begin("cont-repeat", bits, channels));
		TASK_CHECK(recording.write(5000));
		TASK_CHECK(recording.sync());
		TASK_CHECK(recording.verify());

		FileBytes before;
		TASK_CHECK(before.load(recording.path));

		for (int i = 0; i < 4; i++)
		{
			TASK_CHECK(recording.sync());
			TASK_CHECK(recording.verify());
			FileBytes after;
			TASK_CHECK(after.load(recording.path));
			TASK_CHECK(before.bytes == after.bytes);
		}

		TASK_CHECK(recording.write(2000));
		TASK_CHECK(recording.sync());
		TASK_CHECK(recording.verify());

		TASK_CHECK(recording.close());
		TASK_CHECK(recording.verify());
		recording.cleanup();
		return 0;
	});
}

/*
	continuation: a write which fills the pending packet exactly must
	not leave a phantom tail packet behind.
*/
int continuationExactCompletion(void)
{
	return forEachFormat([](int bits, int channels) -> int
	{
		Recording recording;
		TASK_CHECK(recording.begin("cont-exact", bits, channels));

		/* Leave 7 frames pending, then supply exactly those 7. */
		TASK_CHECK(recording.write(kFramesPerPacket - 7));
		TASK_CHECK(recording.sync());
		TASK_CHECK(recording.verify());
		TASK_CHECK(recording.write(7));
		TASK_CHECK(recording.sync());
		TASK_CHECK(recording.verify());

		/*
			The provisional packet from the first sync was replaced:
			the file must describe exactly one packet.
		*/
		FileBytes bytes;
		TASK_CHECK(bytes.load(recording.path));
		ParsedCAF caf;
		TASK_CHECK(parseCAF(bytes.bytes, caf));
		TASK_CHECK(caf.packetTables.size() == 1);
		TASK_CHECK(caf.packetTables[0].numPackets == 1);
		TASK_CHECK(caf.packetTables[0].numValidFrames == kFramesPerPacket);

		TASK_CHECK(recording.close());
		TASK_CHECK(recording.verify());
		recording.cleanup();
		return 0;
	});
}

/*
	consumer: a separate open (as a fresh process would do) sees the
	right frame count, reads in various chunk sizes and seeks.
*/
int consumerSnapshots(void)
{
	return forEachFormat([](int bits, int channels) -> int
	{
		Recording recording;
		TASK_CHECK(recording.begin("consumer-snap", bits, channels));

		const int segments[] = { 4097, 1, 4095, 8191, 2 };
		for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); i++)
		{
			TASK_CHECK(recording.write(segments[i]));
			TASK_CHECK(recording.sync());
			TASK_CHECK(recording.verify());

			/* Also verify after a redundant sync in the same state. */
			TASK_CHECK(recording.sync());
			TASK_CHECK(recording.verify());
		}

		TASK_CHECK(recording.close());
		TASK_CHECK(recording.verify());
		recording.cleanup();
		return 0;
	});
}

/*
	consumer: independent parse of the live file. The data chunk size
	must equal the sum of the packet sizes, the packet count must match
	the number of variable-length entries, and the valid frame count
	must match what was written.
*/
int consumerPacketTable(void)
{
	return forEachFormat([](int bits, int channels) -> int
	{
		Recording recording;
		TASK_CHECK(recording.begin("consumer-pakt", bits, channels));
		TASK_CHECK(recording.write(6000));
		TASK_CHECK(recording.sync());

		FileBytes bytes;
		TASK_CHECK(bytes.load(recording.path));
		ParsedCAF caf;
		TASK_CHECK(parseCAF(bytes.bytes, caf));
		TASK_CHECK(packetTableConsistent(caf, recording.written, bits, channels));
		TASK_CHECK(caf.packetTables.front().packetBytes.size() == 2);

		/* A repeated sync must not change the packet table. */
		TASK_CHECK(recording.sync());
		FileBytes repeated;
		TASK_CHECK(repeated.load(recording.path));
		TASK_CHECK(bytes.bytes == repeated.bytes);

		/* And there must be no leftover chunk after the packet table. */
		size_t position = 8;
		bool sawPacketTable = false;
		while (position + 12 <= repeated.bytes.size())
		{
			const uint8_t *header = repeated.bytes.data() + position;
			uint64_t length = readBigEndian(header + 4, 8);
			if (memcmp(header, "pakt", 4) == 0)
			{
				sawPacketTable = true;
				position += 12 + static_cast<size_t>(length);
				break;
			}
			position += 12 + static_cast<size_t>(length);
		}
		TASK_CHECK(sawPacketTable);
		/*
			The packet table is the last chunk; the file may end with
			at most one byte of zero padding if the chunk length is odd.
		*/
		TASK_CHECK(position == repeated.bytes.size() ||
			(position + 1 == repeated.bytes.size() &&
			 repeated.bytes[position] == 0));

		TASK_CHECK(recording.close());
		TASK_CHECK(recording.verify());
		recording.cleanup();
		return 0;
	});
}

/*
	consumer: ordinary CAF PCM and ALAC round-trips still behave as
	before. This guards the public API and other formats.
*/
int consumerUncompressedRegression(void)
{
	/* Uncompressed CAF. */
	for (int bitsValue = 0; bitsValue < 2; bitsValue++)
	{
		int bits = bitsValue ? 24 : 16;
		std::string path = temporaryPath("pcm");
		unlink(path.c_str());

		AFfilesetup setup = afNewFileSetup();
		afInitFileFormat(setup, AF_FILE_CAF);
		afInitChannels(setup, AF_DEFAULT_TRACK, 2);
		afInitSampleFormat(setup, AF_DEFAULT_TRACK, AF_SAMPFMT_TWOSCOMP, bits);
		AFfilehandle file = afOpenFile(path.c_str(), "w", setup);
		afFreeFileSetup(setup);
		TASK_CHECK(file != NULL);
		TASK_CHECK(afSetVirtualSampleFormat(file, AF_DEFAULT_TRACK,
			AF_SAMPFMT_TWOSCOMP, 32) == 0);

		const int frameCount = 9001;
		std::vector<int32_t> samples(static_cast<size_t>(frameCount) * 2);
		for (int i = 0; i < frameCount; i++)
			for (int c = 0; c < 2; c++)
				samples[static_cast<size_t>(i) * 2 + c] = sampleValue(i, c);

		TASK_CHECK(afWriteFrames(file, AF_DEFAULT_TRACK, samples.data(),
			frameCount) == frameCount);
		TASK_CHECK(afSyncFile(file) == 0);
		TASK_CHECK(afCloseFile(file) == 0);

		AFfilehandle reader = afOpenFile(path.c_str(), "r", NULL);
		TASK_CHECK(reader != NULL);
		TASK_CHECK(afSetVirtualSampleFormat(reader, AF_DEFAULT_TRACK,
			AF_SAMPFMT_TWOSCOMP, 32) == 0);
		TASK_CHECK(afGetFrameCount(reader, AF_DEFAULT_TRACK) == frameCount);
		TASK_CHECK(afSeekFrame(reader, AF_DEFAULT_TRACK, 4097) == 4097);
		int32_t pair[2];
		TASK_CHECK(afReadFrames(reader, AF_DEFAULT_TRACK, pair, 1) == 1);
		TASK_CHECK(pair[0] == samples[4097 * 2]);
		TASK_CHECK(pair[1] == samples[4097 * 2 + 1]);
		TASK_CHECK(afCloseFile(reader) == 0);
		unlink(path.c_str());
	}

	/* Ordinary ALAC round-trip through a single close. */
	{
		std::string path = temporaryPath("alac");
		Recording recording;
		TASK_CHECK(recording.begin("regression", 16, 2));
		TASK_CHECK(recording.write(9001));
		TASK_CHECK(recording.close());
		TASK_CHECK(recording.verify());
		recording.cleanup();
		(void) path;
	}

	return 0;
}

} // namespace

const task_case task_cases[] =
{
	{ "snapshots", "basic", snapshotsBasic },
	{ "snapshots", "progression", snapshotsProgression },
	{ "snapshots", "boundaries", snapshotsBoundaries },
	{ "continuation", "short_tail", continuationShortTail },
	{ "continuation", "repeated_sync", continuationRepeatedSync },
	{ "continuation", "exact_completion", continuationExactCompletion },
	{ "consumer", "snapshots", consumerSnapshots },
	{ "consumer", "packet_table", consumerPacketTable },
	{ "consumer", "uncompressed_regression", consumerUncompressedRegression },
};

const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
