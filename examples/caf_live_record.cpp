/*
	caf_live_record: record CAF/ALAC audio in segments, syncing after each
	segment so that a second process can listen to the unfinished file.

	Usage:
		caf_live_record produce <path> <bits> <channels> <frames,frames,...>
		caf_live_record inspect <path> <bits> <channels> <expectedFrames>

	produce writes the given comma-separated segment frame counts of
	synthetic 32-bit integer PCM, calls afSyncFile after each segment and
	runs itself in inspect mode against the current file before continuing.

	inspect independently regenerates the expected sample sequence and
	verifies the file frame by frame, including seeks.

	Both modes exit with a non-zero status on any error.
*/

#include <audiofile.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const int kTrack = AF_DEFAULT_TRACK;

/* Virtual 32-bit integer sample for the given frame and channel. */
static int32_t sampleValue(int64_t frame, int channel)
{
	return (int32_t)((((frame * 131 + channel * 997) % 40001) - 20000) * 65536);
}

static int fail(const char *message)
{
	fprintf(stderr, "caf_live_record: %s\n", message);
	return 1;
}

static void generateFrames(int32_t *buffer, int64_t firstFrame, int count,
	int channels)
{
	for (int i = 0; i < count; i++)
		for (int c = 0; c < channels; c++)
			buffer[i * channels + c] = sampleValue(firstFrame + i, c);
}

static int checkFrames(const int32_t *buffer, int64_t firstFrame, int count,
	int channels)
{
	for (int i = 0; i < count; i++)
		for (int c = 0; c < channels; c++)
		{
			int32_t expected = sampleValue(firstFrame + i, c);
			if (buffer[i * channels + c] != expected)
			{
				fprintf(stderr,
					"caf_live_record: sample mismatch at frame %lld "
					"channel %d: got %d, expected %d\n",
					(long long) (firstFrame + i), c,
					buffer[i * channels + c], expected);
				return -1;
			}
		}
	return 0;
}

/* Run ourselves in inspect mode against the current file on disk. */
static int runInspect(const char *self, const char *path, int bits,
	int channels, int64_t totalFrames)
{
	char bitsArg[16], channelsArg[16], framesArg[32];
	snprintf(bitsArg, sizeof bitsArg, "%d", bits);
	snprintf(channelsArg, sizeof channelsArg, "%d", channels);
	snprintf(framesArg, sizeof framesArg, "%lld", (long long) totalFrames);

	pid_t pid = fork();
	if (pid < 0)
		return fail("fork failed");
	if (pid == 0)
	{
		execl(self, self, "inspect", path, bitsArg, channelsArg, framesArg,
			(char *) NULL);
		_exit(127);
	}

	int status = 0;
	if (waitpid(pid, &status, 0) < 0)
		return fail("waitpid failed");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
	{
		fprintf(stderr, "caf_live_record: inspect of %s at %lld frames failed\n",
			path, (long long) totalFrames);
		return 1;
	}
	return 0;
}

static int produce(const char *self, const char *path, int bits, int channels,
	const char *segmentsArg)
{
	AFfilesetup setup = afNewFileSetup();
	afInitFileFormat(setup, AF_FILE_CAF);
	afInitChannels(setup, kTrack, channels);
	afInitSampleFormat(setup, kTrack, AF_SAMPFMT_TWOSCOMP, bits);
	afInitRate(setup, kTrack, 44100.0);
	afInitCompression(setup, kTrack, AF_COMPRESSION_ALAC);

	AFfilehandle file = afOpenFile(path, "w", setup);
	afFreeFileSetup(setup);
	if (!file)
		return fail("could not open output file");

	if (afSetVirtualSampleFormat(file, kTrack, AF_SAMPFMT_TWOSCOMP, 32) != 0)
		return fail("could not set virtual sample format");

	char *segments = strdup(segmentsArg);
	if (!segments)
		return fail("out of memory");

	int64_t total = 0;
	int status = 0;
	for (char *token = strtok(segments, ","); token; token = strtok(NULL, ","))
	{
		char *end = NULL;
		long segment = strtol(token, &end, 10);
		if (!end || *end != '\0' || segment < 0)
		{
			fprintf(stderr, "caf_live_record: invalid segment '%s'\n", token);
			status = 1;
			break;
		}

		/* Write the segment in bounded chunks. */
		int64_t remaining = segment;
		while (remaining > 0)
		{
			int count = remaining > 1024 ? 1024 : (int) remaining;
			int32_t buffer[1024 * 2];
			generateFrames(buffer, total, count, channels);
			if (afWriteFrames(file, kTrack, buffer, count) != count)
			{
				status = fail("afWriteFrames failed");
				break;
			}
			total += count;
			remaining -= count;
		}
		if (status)
			break;

		if (afSyncFile(file) != 0)
		{
			status = fail("afSyncFile failed");
			break;
		}

		/* Let a fresh process listen to the unfinished recording. */
		if (runInspect(self, path, bits, channels, total) != 0)
		{
			status = 1;
			break;
		}
	}

	free(segments);

	if (afCloseFile(file) != 0)
		return fail("afCloseFile failed");

	return status;
}

static int inspect(const char *path, int bits, int channels, int64_t expected)
{
	AFfilehandle file = afOpenFile(path, "r", NULL);
	if (!file)
		return fail("could not open input file");

	if (afSetVirtualSampleFormat(file, kTrack, AF_SAMPFMT_TWOSCOMP, 32) != 0)
		return fail("could not set virtual sample format");

	if (afGetChannels(file, kTrack) != channels)
		return fail("channel count mismatch");

	int sampleFormat = 0, sampleWidth = 0;
	afGetSampleFormat(file, kTrack, &sampleFormat, &sampleWidth);
	if (sampleFormat != AF_SAMPFMT_TWOSCOMP || sampleWidth != bits)
		return fail("sample format mismatch");

	if (afGetFrameCount(file, kTrack) != expected)
	{
		fprintf(stderr, "caf_live_record: frame count is %lld, expected %lld\n",
			(long long) afGetFrameCount(file, kTrack), (long long) expected);
		return 1;
	}

	/* Read the whole stream forward in irregular small chunks. */
	const int kChunk = 997;
	int32_t buffer[kChunk * 2];
	int64_t frame = 0;
	while (frame < expected)
	{
		int want = expected - frame < kChunk ? (int) (expected - frame) : kChunk;
		int got = afReadFrames(file, kTrack, buffer, want);
		if (got != want)
			return fail("short read while scanning forward");
		if (checkFrames(buffer, frame, got, channels) != 0)
			return 1;
		frame += got;
	}
	if (afReadFrames(file, kTrack, buffer, 1) != 0)
		return fail("read past end did not report end of stream");

	/* Read the whole stream again one frame at a time. */
	if (expected > 0 && afSeekFrame(file, kTrack, 0) != 0)
		return fail("seek to start failed");
	for (int64_t i = 0; i < expected; i++)
	{
		int32_t one[2];
		if (afReadFrames(file, kTrack, one, 1) != 1)
			return fail("frame-at-a-time read failed");
		if (checkFrames(one, i, 1, channels) != 0)
			return 1;
	}

	/* Spot-check random seeks, including a seek to the end. */
	uint32_t state = 12345;
	for (int iteration = 0; iteration < 512 && expected > 0; iteration++)
	{
		state = state * 1103515245 + 12345;
		int64_t pos = (state >> 8) % (expected + 1);
		if (afSeekFrame(file, kTrack, pos) != pos)
			return fail("seek failed");
		int want = expected - pos < 13 ? (int) (expected - pos) : 13;
		if (want == 0)
		{
			if (afReadFrames(file, kTrack, buffer, 1) != 0)
				return fail("read at end did not report end of stream");
			continue;
		}
		if (afReadFrames(file, kTrack, buffer, want) != want)
			return fail("read after seek failed");
		if (checkFrames(buffer, pos, want, channels) != 0)
			return 1;
	}

	if (afCloseFile(file) != 0)
		return fail("afCloseFile failed");

	return 0;
}

static int usage()
{
	fprintf(stderr,
		"usage:\n"
		"  caf_live_record produce <path> <bits> <channels> <frames,frames,...>\n"
		"  caf_live_record inspect <path> <bits> <channels> <expectedFrames>\n");
	return 2;
}

int main(int argc, char **argv)
{
	if (argc == 6 && !strcmp(argv[1], "produce"))
	{
		int bits = atoi(argv[3]);
		int channels = atoi(argv[4]);
		if ((bits != 16 && bits != 24) || (channels != 1 && channels != 2))
			return fail("bits must be 16 or 24 and channels 1 or 2");
		return produce(argv[0], argv[2], bits, channels, argv[5]);
	}
	if (argc == 6 && !strcmp(argv[1], "inspect"))
	{
		int bits = atoi(argv[3]);
		int channels = atoi(argv[4]);
		if ((bits != 16 && bits != 24) || (channels != 1 && channels != 2))
			return fail("bits must be 16 or 24 and channels 1 or 2");
		char *end = NULL;
		long long expected = strtoll(argv[5], &end, 10);
		if (!end || *end != '\0' || expected < 0)
			return fail("invalid expected frame count");
		return inspect(argv[2], bits, channels, expected);
	}
	return usage();
}
