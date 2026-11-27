/*
	Audio File Library

	caf_live_record.cpp

	A small command-line recorder which demonstrates streaming CAF/ALAC
	recording with afSyncFile(). A second process can inspect the file
	while it is still being written.

	produce <path> <16|24> <channels> <frames,frames,...>
		Write each comma-separated segment of virtual 32-bit PCM,
		sync, and run "inspect" on the partially written file.

	inspect <path> <16|24> <channels> <expected-frames>
		Independently reconstruct the expected sample sequence,
		compare every decoded sample, and exercise seeking.

	Any mismatch makes the process exit with a non-zero status.
*/

#include <audiofile.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

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

void fail(const char *message)
{
	fprintf(stderr, "caf_live_record: %s\n", message);
	exit(1);
}

void failf(const char *format, long long a, long long b)
{
	fprintf(stderr, "caf_live_record: ");
	fprintf(stderr, format, a, b);
	fprintf(stderr, "\n");
	exit(1);
}

AFfilehandle openWriter(const char *path, int bits, int channels)
{
	AFfilesetup setup = afNewFileSetup();
	if (!setup)
		fail("could not create file setup");
	afInitFileFormat(setup, AF_FILE_CAF);
	afInitChannels(setup, AF_DEFAULT_TRACK, channels);
	afInitSampleFormat(setup, AF_DEFAULT_TRACK, AF_SAMPFMT_TWOSCOMP, bits);
	afInitCompression(setup, AF_DEFAULT_TRACK, AF_COMPRESSION_ALAC);

	AFfilehandle file = afOpenFile(path, "w", setup);
	afFreeFileSetup(setup);
	if (!file)
		fail("could not open output file");
	if (afSetVirtualSampleFormat(file, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) != 0)
	{
		fail("could not select 32-bit virtual sample format");
	}
	return file;
}

void fillSegment(std::vector<int32_t> &samples, int firstFrame, int frameCount,
	int channels)
{
	samples.resize(static_cast<size_t>(frameCount) * channels);
	for (int i = 0; i < frameCount; i++)
		for (int c = 0; c < channels; c++)
			samples[static_cast<size_t>(i) * channels + c] =
				sampleValue(firstFrame + i, c);
}

int parsePositive(const char *text, const char *what)
{
	char *end = NULL;
	errno = 0;
	long value = strtol(text, &end, 10);
	if (errno || end == text || *end || value < 0)
	{
		fprintf(stderr, "caf_live_record: invalid %s '%s'\n", what, text);
		exit(1);
	}
	return static_cast<int>(value);
}

std::vector<int> parseSegments(const char *text)
{
	std::vector<int> segments;
	std::string input(text);
	size_t position = 0;
	while (position <= input.size())
	{
		size_t comma = input.find(',', position);
		if (comma == std::string::npos)
			comma = input.size();
		std::string piece = input.substr(position, comma - position);
		if (piece.empty())
			fail("empty segment length");
		char *end = NULL;
		errno = 0;
		long value = strtol(piece.c_str(), &end, 10);
		if (errno || end == piece.c_str() || *end || value < 0)
			fail("invalid segment length");
		segments.push_back(static_cast<int>(value));
		if (comma == input.size())
			break;
		position = comma + 1;
	}
	if (segments.empty())
		fail("no segments");
	return segments;
}

std::string executablePath(const char *argv0)
{
	if (argv0 && strchr(argv0, '/'))
		return std::string(argv0);

	char buffer[4096];
	ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (length > 0)
	{
		buffer[length] = '\0';
		return std::string(buffer);
	}
	return std::string(argv0 ? argv0 : "caf_live_record");
}

/*
	Run "inspect" in a child process and require it to succeed.
*/
void runInspect(const std::string &self, const char *path, int bits,
	int channels, long long expectedFrames)
{
	char framesBuffer[64];
	snprintf(framesBuffer, sizeof(framesBuffer), "%lld", expectedFrames);
	char bitsBuffer[16];
	snprintf(bitsBuffer, sizeof(bitsBuffer), "%d", bits);
	char channelsBuffer[16];
	snprintf(channelsBuffer, sizeof(channelsBuffer), "%d", channels);

	const char *argv[] =
	{
		self.c_str(),
		"inspect",
		path,
		bitsBuffer,
		channelsBuffer,
		framesBuffer,
		NULL
	};

	pid_t child = fork();
	if (child < 0)
		fail("fork failed");
	if (child == 0)
	{
		execv(self.c_str(), const_cast<char * const *>(argv));
		execvp(self.c_str(), const_cast<char * const *>(argv));
		_exit(127);
	}

	int status = 0;
	while (waitpid(child, &status, 0) < 0)
	{
		if (errno != EINTR)
			fail("waitpid failed");
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
	{
		fprintf(stderr, "caf_live_record: inspect failed for %lld frames\n",
			expectedFrames);
		exit(1);
	}
}

void checkSample(int framesRead, const int32_t *samples,
	int firstFrame, int channels)
{
	if (framesRead < 0)
		fail("afReadFrames failed");
	for (int i = 0; i < framesRead; i++)
	{
		for (int c = 0; c < channels; c++)
		{
			int32_t expected = sampleValue(firstFrame + i, c);
			int32_t actual = samples[static_cast<size_t>(i) * channels + c];
			if (actual != expected)
			{
				fprintf(stderr,
					"caf_live_record: sample mismatch at frame %d channel %d: "
					"got %d expected %d\n",
					firstFrame + i, c, actual, expected);
				exit(1);
			}
		}
	}
}

/*
	Verify the whole file using reads of varying granularity. Returns
	the number of frames which could be read.
*/
long long verifySequential(AFfilehandle file, int channels,
	long long expectedFrames)
{
	static const int chunkSizes[] = { 1, 2, 7, 64, 1023, 4096, 17, 8191 };
	const int chunkCount = sizeof(chunkSizes) / sizeof(chunkSizes[0]);

	std::vector<int32_t> buffer;
	long long frame = 0;
	int chunkIndex = 0;
	while (frame < expectedFrames)
	{
		int request = chunkSizes[chunkIndex % chunkCount];
		chunkIndex++;
		long long remaining = expectedFrames - frame;
		if (request > remaining)
			request = static_cast<int>(remaining);

		buffer.resize(static_cast<size_t>(request) * channels);
		int got = afReadFrames(file, AF_DEFAULT_TRACK, buffer.data(), request);
		if (got != request)
			failf("short read at frame %lld: got %lld", frame, got);
		checkSample(got, buffer.data(), static_cast<int>(frame), channels);
		frame += got;
	}

	/* Reading at the valid end must not produce more samples. */
	int32_t extra[2 * 8];
	int got = afReadFrames(file, AF_DEFAULT_TRACK, extra, 8);
	if (got != 0)
		failf("read past end returned %lld frames", got, 0);

	return frame;
}

/*
	Seek to assorted positions and compare the decoded samples.
*/
void verifySeeking(AFfilehandle file, int channels, long long expectedFrames)
{
	if (expectedFrames == 0)
		return;

	std::vector<long long> positions;
	positions.push_back(0);
	positions.push_back(expectedFrames - 1);
	positions.push_back(expectedFrames / 2);
	positions.push_back(kFramesPerPacket - 1);
	positions.push_back(kFramesPerPacket);
	positions.push_back(kFramesPerPacket + 1);
	positions.push_back(2 * kFramesPerPacket - 1);
	positions.push_back(3 * kFramesPerPacket + 17);
	positions.push_back(expectedFrames / 3);

	std::vector<int32_t> buffer(static_cast<size_t>(64) * channels);
	for (size_t i = 0; i < positions.size(); i++)
	{
		long long position = positions[i];
		if (position < 0 || position >= expectedFrames)
			continue;

		AFframecount actual = afSeekFrame(file, AF_DEFAULT_TRACK,
			static_cast<AFframecount>(position));
		if (actual != position)
			failf("seek to %lld returned %lld", position, actual);

		long long remaining = expectedFrames - position;
		int request = remaining < 64 ? static_cast<int>(remaining) : 64;
		int got = afReadFrames(file, AF_DEFAULT_TRACK, buffer.data(), request);
		if (got != request)
			failf("short read after seek to %lld: got %lld", position, got);
		checkSample(got, buffer.data(), static_cast<int>(position),
			channels);
	}
}

int inspectMode(const char *path, int bits, int channels,
	long long expectedFrames)
{
	AFfilehandle file = afOpenFile(path, "r", NULL);
	if (!file)
		fail("could not open file for inspection");
	if (afSetVirtualSampleFormat(file, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) != 0)
		fail("could not select 32-bit virtual sample format");
	if (afSetVirtualChannels(file, AF_DEFAULT_TRACK, channels) != 0)
		fail("could not select virtual channel count");

	AFframecount frameCount = afGetFrameCount(file, AF_DEFAULT_TRACK);
	if (frameCount != expectedFrames)
	{
		fprintf(stderr,
			"caf_live_record: frame count %lld does not match expected %lld\n",
			(long long) frameCount, expectedFrames);
		exit(1);
	}

	long long read = verifySequential(file, channels, expectedFrames);
	if (read != expectedFrames)
		failf("read %lld of %lld frames", read, expectedFrames);

	/* Go back to the beginning and exercise seeking. */
	if (afSeekFrame(file, AF_DEFAULT_TRACK, 0) != 0)
		fail("could not rewind");
	verifySeeking(file, channels, expectedFrames);

	if (afCloseFile(file) != 0)
		fail("could not close inspected file");
	(void) bits;
	return 0;
}

int produceMode(const char *path, int bits, int channels,
	const std::vector<int> &segments, const std::string &self)
{
	AFfilehandle file = openWriter(path, bits, channels);

	std::vector<int32_t> samples;
	long long written = 0;
	for (size_t i = 0; i < segments.size(); i++)
	{
		int frameCount = segments[i];
		fillSegment(samples, static_cast<int>(written), frameCount, channels);

		int result = frameCount == 0 ? 0 :
			afWriteFrames(file, AF_DEFAULT_TRACK, samples.data(), frameCount);
		if (result != frameCount)
			failf("wrote %lld of %lld frames", result, frameCount);
		written += frameCount;

		if (afSyncFile(file) != 0)
			fail("afSyncFile failed");

		runInspect(self, path, bits, channels, written);
	}

	if (afCloseFile(file) != 0)
		fail("afCloseFile failed");

	/* One last check after closing. */
	runInspect(self, path, bits, channels, written);
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr,
			"usage: %s produce <path> <16|24> <channels> <frames,...>\n"
			"       %s inspect <path> <16|24> <channels> <expected-frames>\n",
			argv[0], argv[0]);
		return 1;
	}

	const std::string mode(argv[1]);
	if (mode == "produce")
	{
		if (argc != 6)
			fail("produce requires 4 arguments");
		const char *path = argv[2];
		int bits = parsePositive(argv[3], "sample width");
		int channels = parsePositive(argv[4], "channel count");
		if (bits != 16 && bits != 24)
			fail("sample width must be 16 or 24");
		if (channels != 1 && channels != 2)
			fail("channel count must be 1 or 2");
		std::vector<int> segments = parseSegments(argv[5]);
		return produceMode(path, bits, channels, segments,
			executablePath(argv[0]));
	}
	else if (mode == "inspect")
	{
		if (argc != 6)
			fail("inspect requires 4 arguments");
		const char *path = argv[2];
		int bits = parsePositive(argv[3], "sample width");
		int channels = parsePositive(argv[4], "channel count");
		long long expectedFrames = strtoll(argv[5], NULL, 10);
		if (expectedFrames < 0)
			fail("expected frame count must not be negative");
		if (bits != 16 && bits != 24)
			fail("sample width must be 16 or 24");
		if (channels != 1 && channels != 2)
			fail("channel count must be 1 or 2");
		return inspectMode(path, bits, channels, expectedFrames);
	}

	fprintf(stderr, "caf_live_record: unknown mode '%s'\n", argv[1]);
	return 1;
}
