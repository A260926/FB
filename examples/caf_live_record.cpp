/*
	caf_live_record.cpp

	Demonstration and smoke test for live CAF/ALAC recording with periodic
	afSyncFile calls.

	produce <path> <16|24> <channels> <frame0,frame1,...>
		Creates a CAF/ALAC file, writes each segment of virtual 32-bit
		integer PCM, syncs, and spawns an inspect child which reads the
		partially written file before recording continues.

	inspect <path> <16|24> <channels> <expectedTotalFrames>
		Opens the file independently, rebuilds the expected sample
		sequence, verifies every sample through varied read sizes and
		random seeks, and exits non-zero on any mismatch.

	The virtual PCM is deterministic: for frame f and channel c,
		sample = (((f * 131 + c * 997) % 40001) - 20000) * 65536.
*/

#include <audiofile.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <algorithm>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static int32_t pcmValue(int frame, int channel)
{
	// Compute in 64 bits then narrow so the mandated full-scale formula
	// never overflows a signed 32-bit intermediate; the cast keeps the
	// intended two's-complement 32-bit value.
	int64_t scaled = (((frame * 131LL + channel * 997LL) % 40001) - 20000) * 65536;
	return static_cast<int32_t>(scaled);
}

// Value expected after a round trip through an N-bit signed file.
static int32_t expectedValue(int frame, int channel, int bits)
{
	int32_t v = pcmValue(frame, channel);
	if (bits == 16)
		return static_cast<int32_t>(static_cast<uint32_t>(v) & 0xFFFF0000u);
	if (bits == 24)
		return static_cast<int32_t>(static_cast<uint32_t>(v) & 0xFFFFFF00u);
	return v;
}

static bool getExecutablePath(std::string &path)
{
#if defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(NULL, &size);
	std::vector<char> buffer(size);
	if (_NSGetExecutablePath(buffer.data(), &size) != 0)
		return false;
	path.assign(buffer.data());
	return true;
#elif defined(__linux__)
	char buffer[4096];
	ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
	if (n <= 0)
		return false;
	buffer[n] = '\0';
	path.assign(buffer);
	return true;
#else
	return false;
#endif
}

static std::vector<long> parseSegments(const char *text)
{
	std::vector<long> segments;
	long value = 0;
	bool haveDigit = false;
	for (const char *p = text; ; ++p)
	{
		if (*p >= '0' && *p <= '9')
		{
			value = value * 10 + (*p - '0');
			haveDigit = true;
		}
		else
		{
			if (haveDigit)
			{
				segments.push_back(value);
				value = 0;
				haveDigit = false;
			}
			if (*p == '\0')
				break;
			if (*p != ',')
				return std::vector<long>();
		}
	}
	return segments;
}

static bool fillPcm(std::vector<int32_t> &buffer, int channels,
	long start, long count)
{
	buffer.resize(count * channels);
	for (long frame = 0; frame < count; ++frame)
		for (int channel = 0; channel < channels; ++channel)
			buffer[frame * channels + channel] = pcmValue(start + frame, channel);
	return true;
}

// Spawn an inspect child against the current file and wait for it.
static bool runInspector(const char *path, int bits, int channels,
	long expectedTotal)
{
	std::string executable;
	if (!getExecutablePath(executable))
	{
		fprintf(stderr, "could not determine executable path\n");
		return false;
	}

	pid_t pid = fork();
	if (pid < 0)
	{
		perror("fork");
		return false;
	}
	if (pid == 0)
	{
		char totalText[32];
		snprintf(totalText, sizeof(totalText), "%ld", expectedTotal);
		char bitsText[8], channelsText[8];
		snprintf(bitsText, sizeof(bitsText), "%d", bits);
		snprintf(channelsText, sizeof(channelsText), "%d", channels);
		execl(executable.c_str(), executable.c_str(), "inspect", path,
			bitsText, channelsText, totalText, (char *) NULL);
		perror("execl");
		_exit(127);
	}

	int status = 0;
	if (waitpid(pid, &status, 0) < 0)
	{
		perror("waitpid");
		return false;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
	{
		fprintf(stderr, "inspect child failed (status %d) at %ld frames\n",
			status, expectedTotal);
		return false;
	}
	return true;
}

static int produce(const char *path, int bits, int channels,
	const std::vector<long> &segments)
{
	AFfilesetup setup = afNewFileSetup();
	afInitFileFormat(setup, AF_FILE_CAF);
	afInitChannels(setup, AF_DEFAULT_TRACK, channels);
	afInitSampleFormat(setup, AF_DEFAULT_TRACK, AF_SAMPFMT_TWOSCOMP, bits);
	afInitCompression(setup, AF_DEFAULT_TRACK, AF_COMPRESSION_ALAC);
	AFfilehandle handle = afOpenFile(path, "w", setup);
	afFreeFileSetup(setup);
	if (!handle)
	{
		fprintf(stderr, "afOpenFile for writing failed\n");
		return 1;
	}
	if (afSetVirtualSampleFormat(handle, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) != 0)
	{
		fprintf(stderr, "afSetVirtualSampleFormat failed\n");
		return 1;
	}

	long total = 0;
	for (size_t i = 0; i < segments.size(); ++i)
	{
		long count = segments[i];
		if (count > 0)
		{
			std::vector<int32_t> pcm;
			fillPcm(pcm, channels, total, count);
			if (afWriteFrames(handle, AF_DEFAULT_TRACK, pcm.data(), count) != count)
			{
				fprintf(stderr, "afWriteFrames failed at segment %zu\n", i);
				return 1;
			}
		}
		total += count;

		if (afSyncFile(handle) != 0)
		{
			fprintf(stderr, "afSyncFile failed at segment %zu\n", i);
			return 1;
		}

		// A separate process must see exactly the frames written so far.
		if (!runInspector(path, bits, channels, total))
			return 1;
	}

	if (afCloseFile(handle) != 0)
	{
		fprintf(stderr, "afCloseFile failed\n");
		return 1;
	}
	return 0;
}

static int inspect(const char *path, int bits, int channels, long expectedTotal)
{
	AFfilehandle handle = afOpenFile(path, "r", 0);
	if (!handle)
	{
		fprintf(stderr, "inspect: could not open %s\n", path);
		return 1;
	}
	if (afSetVirtualSampleFormat(handle, AF_DEFAULT_TRACK,
		AF_SAMPFMT_TWOSCOMP, 32) != 0)
	{
		fprintf(stderr, "inspect: afSetVirtualSampleFormat failed\n");
		return 1;
	}

	long frameCount = afGetFrameCount(handle, AF_DEFAULT_TRACK);
	if (frameCount != expectedTotal)
	{
		fprintf(stderr, "inspect: frame count %ld != expected %ld\n",
			frameCount, expectedTotal);
		return 1;
	}

	// Sequential reads with one-frame, small and cross-packet sizes.
	static const int kSizes[] = {1, 3, 7, 100, 1024, 4095, 4096, 4097, 5000};
	long position = 0;
	size_t sizeIndex = 0;
	while (position < expectedTotal)
	{
		int wanted = std::min<long>(kSizes[sizeIndex++ % 9],
			expectedTotal - position);
		std::vector<int32_t> buffer(wanted * channels);
		long got = afReadFrames(handle, AF_DEFAULT_TRACK, buffer.data(), wanted);
		if (got != wanted)
		{
			fprintf(stderr, "inspect: short read at frame %ld (%ld/%d)\n",
				position, got, wanted);
			return 1;
		}
		for (long frame = 0; frame < got; ++frame)
			for (int channel = 0; channel < channels; ++channel)
			{
				int32_t want = expectedValue(position + frame, channel, bits);
				int32_t have = buffer[frame * channels + channel];
				if (have != want)
				{
					fprintf(stderr,
						"inspect: sample mismatch frame %ld channel %d: "
						"got %d want %d\n",
						position + frame, channel, have, want);
					return 1;
				}
			}
		position += got;
	}

	// Reading at the valid end must terminate immediately.
	int32_t atEnd[2];
	if (afReadFrames(handle, AF_DEFAULT_TRACK, atEnd, 1) != 0)
	{
		fprintf(stderr, "inspect: read past valid end did not return 0\n");
		return 1;
	}

	// Random positioning across packet boundaries.
	long targets[] = {0, 1, expectedTotal - 1, 4095, 4096, 4097,
		8191, 8192, 8193};
	for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
	{
		long target = targets[i];
		if (target < 0 || target >= expectedTotal)
			continue;
		if (afSeekFrame(handle, AF_DEFAULT_TRACK, target) != target)
		{
			fprintf(stderr, "inspect: seek to %ld failed\n", target);
			return 1;
		}
		int wanted = std::min<long>(5, expectedTotal - target);
		std::vector<int32_t> buffer(wanted * channels);
		if (afReadFrames(handle, AF_DEFAULT_TRACK, buffer.data(), wanted) != wanted)
		{
			fprintf(stderr, "inspect: post-seek read failed at %ld\n", target);
			return 1;
		}
		for (long frame = 0; frame < wanted; ++frame)
			for (int channel = 0; channel < channels; ++channel)
			{
				int32_t want = expectedValue(target + frame, channel, bits);
				if (buffer[frame * channels + channel] != want)
				{
					fprintf(stderr,
						"inspect: seek sample mismatch at %ld channel %d\n",
						target + frame, channel);
					return 1;
				}
			}
	}

	if (afCloseFile(handle) != 0)
	{
		fprintf(stderr, "inspect: afCloseFile failed\n");
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		fprintf(stderr,
			"usage:\n"
			"  %s produce <path> <16|24> <channels> <f0,f1,...>\n"
			"  %s inspect <path> <16|24> <channels> <expectedTotalFrames>\n",
			argv[0], argv[0]);
		return 1;
	}

	const char *mode = argv[1];
	if (!strcmp(mode, "produce"))
	{
		if (argc != 6)
		{
			fprintf(stderr, "produce requires path, bits, channels, segments\n");
			return 1;
		}
		const char *path = argv[2];
		int bits = atoi(argv[3]);
		int channels = atoi(argv[4]);
		if ((bits != 16 && bits != 24) || (channels != 1 && channels != 2))
		{
			fprintf(stderr, "bits must be 16 or 24 and channels 1 or 2\n");
			return 1;
		}
		std::vector<long> segments = parseSegments(argv[5]);
		if (segments.empty())
		{
			fprintf(stderr, "could not parse segment frame counts: %s\n", argv[5]);
			return 1;
		}
		return produce(path, bits, channels, segments);
	}
	else if (!strcmp(mode, "inspect"))
	{
		if (argc != 6)
		{
			fprintf(stderr,
				"inspect requires path, bits, channels, expectedTotalFrames\n");
			return 1;
		}
		const char *path = argv[2];
		int bits = atoi(argv[3]);
		int channels = atoi(argv[4]);
		long expectedTotal = atol(argv[5]);
		if ((bits != 16 && bits != 24) || (channels != 1 && channels != 2))
		{
			fprintf(stderr, "bits must be 16 or 24 and channels 1 or 2\n");
			return 1;
		}
		if (expectedTotal < 0)
		{
			fprintf(stderr, "expected total frame count must be non-negative\n");
			return 1;
		}
		return inspect(path, bits, channels, expectedTotal);
	}

	fprintf(stderr, "unknown mode %s\n", mode);
	return 1;
}
