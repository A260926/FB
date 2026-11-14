// Reverse video contract tests.
//
// Build: the fixed support/run.sh target mode compiles this file (C++17) with
// support/driver.cpp and support/plm.o, which contains the PL_MPEG_IMPLEMENTATION
// for the C interface. The test side loads the supplied elementary streams from
// support/media and compares reverse output with an independent forward decode
// performed by the same library through a file buffer.
//
// Real allocation records: this translation unit defines malloc/free/realloc
// interposers. The linker resolves the plm.o references to these definitions,
// so every allocation the library makes is recorded. The records cross-check
// the space accounting (index growth, fixed pixel cache of max_cached_frames
// frames, no per-frame pixel or decoder snapshots, no copy of the compressed
// input).

#include "support/contract.hpp"
#include "pl_mpeg.h"

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#define TEST_HAVE_MALLOC_ZONE 1
#else
#include <dlfcn.h>
#endif

// -----------------------------------------------------------------------------
// Real allocation tracking (applies to the linked C implementation too)
// -----------------------------------------------------------------------------

namespace alloc_track {
struct Entry { void *ptr; size_t size; };
static Entry entries[32768];
static size_t entry_count = 0;
static size_t live_bytes = 0;
static size_t live_blocks = 0;
static size_t peak_bytes = 0;
static size_t peak_blocks = 0;
static size_t allocation_calls = 0;

static Entry *find(void *ptr) {
	for (size_t i = 0; i < entry_count; i++) {
		if (entries[i].ptr == ptr) return &entries[i];
	}
	return NULL;
}

static void record(void *ptr, size_t size) {
	if (!ptr) return;
	allocation_calls++;
	if (entry_count < sizeof(entries) / sizeof(entries[0])) {
		entries[entry_count].ptr = ptr;
		entries[entry_count].size = size;
		entry_count++;
	}
	live_bytes += size;
	live_blocks++;
	if (live_bytes > peak_bytes) peak_bytes = live_bytes;
	if (live_blocks > peak_blocks) peak_blocks = live_blocks;
}

static void unrecord(void *ptr) {
	Entry *entry = find(ptr);
	if (!entry) return;
	live_bytes -= entry->size;
	live_blocks--;
	*entry = entries[entry_count - 1];
	entry_count--;
}

static void reset_peaks() {
	peak_bytes = live_bytes;
	peak_blocks = live_blocks;
}

struct Snapshot {
	size_t live_bytes;
	size_t live_blocks;
	size_t allocation_calls;
};

static Snapshot snapshot() {
	Snapshot s;
	s.live_bytes = live_bytes;
	s.live_blocks = live_blocks;
	s.allocation_calls = allocation_calls;
	return s;
}
} // namespace alloc_track

// Underlying system allocator, bypassing the interposers.
static void *system_malloc(size_t size) {
#if defined(TEST_HAVE_MALLOC_ZONE)
	return malloc_zone_malloc(malloc_default_zone(), size ? size : 1);
#else
	typedef void *(*malloc_fn)(size_t);
	static malloc_fn real = NULL;
	if (!real) real = (malloc_fn)dlsym(RTLD_NEXT, "malloc");
	return real(size ? size : 1);
#endif
}

static void system_free(void *ptr) {
#if defined(TEST_HAVE_MALLOC_ZONE)
	malloc_zone_free(malloc_default_zone(), ptr);
#else
	typedef void (*free_fn)(void *);
	static free_fn real = NULL;
	if (!real) real = (free_fn)dlsym(RTLD_NEXT, "free");
	real(ptr);
#endif
}

static void *system_realloc(void *ptr, size_t size) {
#if defined(TEST_HAVE_MALLOC_ZONE)
	return malloc_zone_realloc(malloc_default_zone(), ptr, size ? size : 1);
#else
	typedef void *(*realloc_fn)(void *, size_t);
	static realloc_fn real = NULL;
	if (!real) real = (realloc_fn)dlsym(RTLD_NEXT, "realloc");
	return real(ptr, size ? size : 1);
#endif
}

extern "C" void *malloc(size_t size) {
	static int inside = 0;
	if (inside) return system_malloc(size);
	inside = 1;
	void *ptr = system_malloc(size);
	inside = 0;
	alloc_track::record(ptr, size);
	return ptr;
}

extern "C" void free(void *ptr) {
	if (!ptr) return;
	alloc_track::unrecord(ptr);
	system_free(ptr);
}

extern "C" void *realloc(void *ptr, size_t size) {
	if (!ptr) return malloc(size);
	static int inside = 0;
	if (inside) return system_realloc(ptr, size);
	inside = 1;
	void *grown = system_realloc(ptr, size);
	inside = 0;
	if (!grown) return NULL;
	alloc_track::Entry *entry = alloc_track::find(ptr);
	if (entry) {
		alloc_track::live_bytes += size - entry->size;
		entry->ptr = grown;
		entry->size = size;
		alloc_track::allocation_calls++;
		if (alloc_track::live_bytes > alloc_track::peak_bytes) {
			alloc_track::peak_bytes = alloc_track::live_bytes;
		}
	}
	else {
		alloc_track::record(grown, size);
	}
	return grown;
}

extern "C" void *calloc(size_t count, size_t size) {
	size_t total = count * size;
	void *ptr = malloc(total);
	if (ptr) memset(ptr, 0, total);
	return ptr;
}

// -----------------------------------------------------------------------------
// Media loading and reference decoding
// -----------------------------------------------------------------------------

static std::string g_media_dir;

static std::string media_path(const char *name) {
	std::string path = g_media_dir;
	path += "/";
	path += name;
	return path;
}

static std::vector<uint8_t> load_file(const char *name) {
	std::string path = media_path(name);
	FILE *fh = fopen(path.c_str(), "rb");
	if (!fh) {
		std::fprintf(stderr, "cannot open %s\n", path.c_str());
		std::exit(43);
	}
	if (fseek(fh, 0, SEEK_END) != 0) std::exit(43);
	long size = ftell(fh);
	if (size <= 0) std::exit(43);
	rewind(fh);
	std::vector<uint8_t> bytes((size_t)size);
	if (fread(bytes.data(), 1, bytes.size(), fh) != bytes.size()) std::exit(43);
	fclose(fh);
	return bytes;
}

// Decode a raw elementary stream forward using a file-backed buffer. This is
// the independent reference used for the pixel checks.
static void forward_decode_file(
	const char *name, std::vector<plm_frame_t> *frames,
	std::vector<std::vector<uint8_t> > *pixels, double *framerate
) {
	std::string path = media_path(name);
	plm_buffer_t *buffer = plm_buffer_create_with_filename(path.c_str());
	UNIT_CHECK(buffer != NULL);
	plm_video_t *video = plm_video_create_with_buffer(buffer, TRUE);
	UNIT_CHECK(video != NULL);
	if (!plm_video_has_header(video)) {
		plm_video_destroy(video);
		std::exit(43);
	}
	*framerate = plm_video_get_framerate(video);

	plm_frame_t *frame;
	while ((frame = plm_video_decode(video)) != NULL) {
		if (frames->size() >= 4096) break;
		size_t luma = (size_t)frame->y.width * frame->y.height;
		size_t chroma = (size_t)frame->cr.width * frame->cr.height;
		std::vector<uint8_t> copy(luma + 2 * chroma);
		memcpy(copy.data(), frame->y.data, luma);
		memcpy(copy.data() + luma, frame->cr.data, chroma);
		memcpy(copy.data() + luma + chroma, frame->cb.data, chroma);
		frames->push_back(*frame);
		pixels->push_back(copy);
	}
	plm_video_destroy(video);
}

// Repeat a complete elementary stream. Running the forward decoder over the
// repeated bytes hits the historical end-of-stream behaviour at each internal
// boundary, so only frames returned by the single-clip reference are used; the
// repeat exists to create many groups for the segmentation/budget checks.
static std::vector<uint8_t> repeat_stream(
	const std::vector<uint8_t> &source, int copies
) {
	std::vector<uint8_t> out;
	out.reserve(source.size() * (size_t)copies);
	for (int i = 0; i < copies; i++) {
		out.insert(out.end(), source.begin(), source.end());
	}
	return out;
}

static unsigned long long plane_hash(const plm_frame_t *frame) {
	unsigned long long h = 1469598103934665603ULL;
	plm_plane_t planes[3] = {frame->y, frame->cr, frame->cb};
	for (int c = 0; c < 3; c++) {
		for (unsigned y = 0; y < planes[c].height; y++) {
			for (unsigned x = 0; x < planes[c].width; x++) {
				h ^= planes[c].data[y * planes[c].width + x];
				h *= 1099511628211ULL;
			}
		}
	}
	return h;
}

// Compare the visible Y/Cr/Cb areas, dimensions and (separately) time.
static bool frame_pixels_match(
	const plm_frame_t *frame, const plm_frame_t *reference,
	const std::vector<uint8_t> &reference_pixels
) {
	if (frame->width != reference->width) return false;
	if (frame->height != reference->height) return false;
	size_t luma = (size_t)reference->y.width * reference->y.height;
	size_t chroma = (size_t)reference->cr.width * reference->cr.height;
	return
		memcmp(frame->y.data, reference_pixels.data(), luma) == 0 &&
		memcmp(frame->cr.data, reference_pixels.data() + luma, chroma) == 0 &&
		memcmp(
			frame->cb.data, reference_pixels.data() + luma + chroma, chroma
		) == 0;
}

static double frame_time(size_t display_frame, double framerate) {
	return (double)display_frame / framerate;
}

static size_t display_from_time(const plm_frame_t *frame, double framerate) {
	return (size_t)(frame->time * framerate + 0.5);
}

struct Clip {
	std::vector<uint8_t> bytes;
	std::vector<plm_frame_t> reference;
	std::vector<std::vector<uint8_t> > reference_pixels;
	std::vector<unsigned long long> reference_hashes;
	double framerate;
};

// Hash of a stored plane copy (the reference frame descriptors themselves point
// into the destroyed forward decoder, so hashes are computed while it is live).
static unsigned long long pixels_hash(
	const plm_frame_t *shape, const std::vector<uint8_t> &pixels
) {
	unsigned long long h = 1469598103934665603ULL;
	size_t luma = (size_t)shape->y.width * shape->y.height;
	size_t chroma = (size_t)shape->cr.width * shape->cr.height;
	size_t sizes[3] = {luma, chroma, chroma};
	size_t offsets[3] = {0, luma, luma + chroma};
	for (int c = 0; c < 3; c++) {
		for (size_t i = 0; i < sizes[c]; i++) {
			h ^= pixels[offsets[c] + i];
			h *= 1099511628211ULL;
		}
	}
	return h;
}

static Clip *load_clip_cached(const char *name) {
	static std::vector<Clip *> clips;
	static std::vector<std::string> names;
	for (size_t i = 0; i < names.size(); i++) {
		if (names[i] == name) return clips[i];
	}
	Clip *clip = new Clip();
	clip->bytes = load_file(name);
	forward_decode_file(
		name, &clip->reference, &clip->reference_pixels, &clip->framerate
	);
	for (size_t i = 0; i < clip->reference.size(); i++) {
		clip->reference_hashes.push_back(
			pixels_hash(&clip->reference[i], clip->reference_pixels[i])
		);
	}
	names.push_back(name);
	clips.push_back(clip);
	return clip;
}

static size_t clip_frame_data_size(const Clip *clip) {
	size_t luma = (size_t)clip->reference[0].y.width * clip->reference[0].y.height;
	size_t chroma = (size_t)clip->reference[0].cr.width *
		clip->reference[0].cr.height;
	return luma + 2 * chroma;
}

// =============================================================================
// index group
// =============================================================================

static void index_counts_open_gop() {
	Clip *clip = load_clip_cached("open.m1v");
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), 9
	);
	UNIT_CHECK(reverse != NULL);
	UNIT_CHECK(plm_video_reverse_get_count(reverse) == clip->reference.size());
	plm_video_reverse_destroy(reverse);
}

static void index_stats_zero_pictures_after_create() {
	Clip *clip = load_clip_cached("open.m1v");
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), 9
	);
	UNIT_CHECK(reverse != NULL);
	plm_video_reverse_stats_t stats;
	memset(&stats, 0x7f, sizeof(stats));
	plm_video_reverse_get_stats(reverse, &stats);
	UNIT_CHECK(stats.pictures_decoded == 0);
	UNIT_CHECK(stats.index_bytes > 0);
	UNIT_CHECK(stats.index_bytes <= 8 * clip->bytes.size());
	UNIT_CHECK(stats.cached_frames_peak == 0);
	plm_video_reverse_destroy(reverse);
}

static void index_bytes_bounded_and_stable() {
	Clip *clip = load_clip_cached("closed.m1v");
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), 8
	);
	UNIT_CHECK(reverse != NULL);
	plm_video_reverse_stats_t before, after;
	plm_video_reverse_get_stats(reverse, &before);
	UNIT_CHECK(before.index_bytes <= 8 * clip->bytes.size());
	plm_frame_t *frame;
	while ((frame = plm_video_reverse_next(reverse)) != NULL) { (void)frame; }
	plm_video_reverse_get_stats(reverse, &after);
	UNIT_CHECK(after.index_bytes == before.index_bytes);
	UNIT_CHECK(after.index_bytes <= 8 * clip->bytes.size());
	plm_video_reverse_destroy(reverse);
}

static void index_capacity_requires_g() {
	Clip *clip = load_clip_cached("open.m1v");
	// G is 9 display frames for the supplied open clip (one I every 9 frames).
	UNIT_CHECK(plm_video_reverse_create(clip->bytes.data(), clip->bytes.size(), 8) == NULL);
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), 9
	);
	UNIT_CHECK(reverse != NULL);
	UNIT_CHECK(plm_video_reverse_get_count(reverse) == clip->reference.size());
	plm_video_reverse_destroy(reverse);
}

static void index_rejects_bad_arguments() {
	Clip *clip = load_clip_cached("ip.m1v");
	UNIT_CHECK(plm_video_reverse_create(NULL, clip->bytes.size(), 9) == NULL);
	UNIT_CHECK(plm_video_reverse_create(clip->bytes.data(), 0, 9) == NULL);
	UNIT_CHECK(plm_video_reverse_create(clip->bytes.data(), clip->bytes.size(), 0) == NULL);
}

static void index_seek_out_of_range_leaves_cursor() {
	Clip *clip = load_clip_cached("open.m1v");
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), 9
	);
	UNIT_CHECK(reverse != NULL);
	size_t count = plm_video_reverse_get_count(reverse);
	plm_frame_t *first = plm_video_reverse_next(reverse);
	UNIT_CHECK(first != NULL);
	UNIT_CHECK(plm_video_reverse_seek(reverse, count) == 0);
	UNIT_CHECK(plm_video_reverse_seek(reverse, count + 100) == 0);
	plm_frame_t *second = plm_video_reverse_next(reverse);
	UNIT_CHECK(second != NULL);
	UNIT_CHECK(second->time == frame_time(count - 2, clip->framerate));
	plm_video_reverse_destroy(reverse);
}

static void index_multiple_instances_independent() {
	Clip *a = load_clip_cached("open.m1v");
	Clip *b = load_clip_cached("ip.m1v");
	plm_video_reverse_t *ra = plm_video_reverse_create(a->bytes.data(), a->bytes.size(), 9);
	plm_video_reverse_t *rb = plm_video_reverse_create(b->bytes.data(), b->bytes.size(), 9);
	UNIT_CHECK(ra != NULL);
	UNIT_CHECK(rb != NULL);
	size_t count_a = plm_video_reverse_get_count(ra);
	size_t count_b = plm_video_reverse_get_count(rb);
	UNIT_CHECK(count_a == a->reference.size());
	UNIT_CHECK(count_b == b->reference.size());
	for (size_t i = 0; i < 5; i++) {
		plm_frame_t *fa = plm_video_reverse_next(ra);
		plm_frame_t *fb = plm_video_reverse_next(rb);
		UNIT_CHECK(fa != NULL && fb != NULL);
		UNIT_CHECK(fa->time == frame_time(count_a - 1 - i, a->framerate));
		UNIT_CHECK(fb->time == frame_time(count_b - 1 - i, b->framerate));
	}
	plm_video_reverse_destroy(ra);
	plm_video_reverse_destroy(rb);
}

// =============================================================================
// pixels group
// =============================================================================

static void pixels_reverse_matches_forward_closed() {
	Clip *clip = load_clip_cached("closed.m1v");
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), 8
	);
	UNIT_CHECK(reverse != NULL);
	size_t count = plm_video_reverse_get_count(reverse);
	UNIT_CHECK(count == clip->reference.size());
	for (size_t i = 0; i < count; i++) {
		plm_frame_t *frame = plm_video_reverse_next(reverse);
		UNIT_CHECK(frame != NULL);
		size_t display = display_from_time(frame, clip->framerate);
		UNIT_CHECK(display == count - 1 - i);
		UNIT_CHECK(frame_pixels_match(
			frame, &clip->reference[display], clip->reference_pixels[display]
		));
		UNIT_CHECK(frame->time == frame_time(display, clip->framerate));
	}
	UNIT_CHECK(plm_video_reverse_next(reverse) == NULL);
	UNIT_CHECK(plm_video_reverse_next(reverse) == NULL);
	plm_video_reverse_destroy(reverse);
}

static void pixels_open_gop_leading_b_pictures() {
	Clip *clip = load_clip_cached("open.m1v");
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), 9
	);
	UNIT_CHECK(reverse != NULL);
	size_t count = plm_video_reverse_get_count(reverse);
	for (size_t i = 0; i < count; i++) {
		plm_frame_t *frame = plm_video_reverse_next(reverse);
		UNIT_CHECK(frame != NULL);
		size_t display = display_from_time(frame, clip->framerate);
		UNIT_CHECK(display == count - 1 - i);
		UNIT_CHECK(frame_pixels_match(
			frame, &clip->reference[display], clip->reference_pixels[display]
		));
	}
	plm_video_reverse_destroy(reverse);
}

static void pixels_cross_i_boundary_backwards() {
	Clip *clip = load_clip_cached("open.m1v");
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), 9
	);
	UNIT_CHECK(reverse != NULL);
	size_t count = plm_video_reverse_get_count(reverse);
	for (size_t boundary = 8; boundary + 2 < count; boundary += 9) {
		UNIT_CHECK(plm_video_reverse_seek(reverse, boundary + 1) == 1);
		for (size_t step = 0; step < 4; step++) {
			plm_frame_t *frame = plm_video_reverse_next(reverse);
			UNIT_CHECK(frame != NULL);
			size_t display = display_from_time(frame, clip->framerate);
			UNIT_CHECK(display == boundary + 1 - step);
			UNIT_CHECK(frame_pixels_match(
				frame, &clip->reference[display], clip->reference_pixels[display]
			));
		}
	}
	plm_video_reverse_destroy(reverse);
}

static void pixels_seek_repeated_across_segments() {
	Clip *clip = load_clip_cached("open.m1v");
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), 9
	);
	UNIT_CHECK(reverse != NULL);
	size_t count = plm_video_reverse_get_count(reverse);
	size_t probes[] = {
		count - 1, count / 2, 0, count / 3, count - 2, 1, count / 4, count / 2
	};
	for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
		size_t target = probes[p];
		UNIT_CHECK(plm_video_reverse_seek(reverse, target) == 1);
		plm_frame_t *frame = plm_video_reverse_next(reverse);
		UNIT_CHECK(frame != NULL);
		UNIT_CHECK(frame_pixels_match(
			frame, &clip->reference[target], clip->reference_pixels[target]
		));
	}
	plm_video_reverse_destroy(reverse);
}

static void pixels_exhaust_then_seek_rearm() {
	Clip *clip = load_clip_cached("open.m1v");
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), 9
	);
	UNIT_CHECK(reverse != NULL);
	size_t count = plm_video_reverse_get_count(reverse);
	while (plm_video_reverse_next(reverse) != NULL) { }
	UNIT_CHECK(plm_video_reverse_next(reverse) == NULL);
	UNIT_CHECK(plm_video_reverse_seek(reverse, count - 1) == 1);
	plm_frame_t *frame = plm_video_reverse_next(reverse);
	UNIT_CHECK(frame != NULL);
	UNIT_CHECK(frame_pixels_match(
		frame, &clip->reference[count - 1], clip->reference_pixels[count - 1]
	));
	UNIT_CHECK(plm_video_reverse_seek(reverse, 0) == 1);
	frame = plm_video_reverse_next(reverse);
	UNIT_CHECK(frame != NULL);
	UNIT_CHECK(display_from_time(frame, clip->framerate) == 0);
	UNIT_CHECK(plm_video_reverse_next(reverse) == NULL);
	plm_video_reverse_destroy(reverse);
}

static void pixels_interleaved_instances() {
	Clip *a = load_clip_cached("open.m1v");
	Clip *b = load_clip_cached("closed.m1v");
	plm_video_reverse_t *ra = plm_video_reverse_create(a->bytes.data(), a->bytes.size(), 9);
	plm_video_reverse_t *rb = plm_video_reverse_create(b->bytes.data(), b->bytes.size(), 8);
	UNIT_CHECK(ra != NULL);
	UNIT_CHECK(rb != NULL);
	size_t count_a = plm_video_reverse_get_count(ra);
	size_t count_b = plm_video_reverse_get_count(rb);
	size_t steps = count_a < count_b ? count_a : count_b;
	for (size_t i = 0; i < steps; i++) {
		plm_frame_t *fa = plm_video_reverse_next(ra);
		plm_frame_t *fb = plm_video_reverse_next(rb);
		UNIT_CHECK(fa != NULL && fb != NULL);
		size_t da = display_from_time(fa, a->framerate);
		size_t db = display_from_time(fb, b->framerate);
		UNIT_CHECK(da == count_a - 1 - i);
		UNIT_CHECK(db == count_b - 1 - i);
		UNIT_CHECK(plane_hash(fa) == a->reference_hashes[da]);
		UNIT_CHECK(plane_hash(fb) == b->reference_hashes[db]);
	}
	UNIT_CHECK(plm_video_reverse_seek(ra, 3) == 1);
	UNIT_CHECK(plm_video_reverse_seek(rb, 2) == 1);
	plm_frame_t *fa = plm_video_reverse_next(ra);
	plm_frame_t *fb = plm_video_reverse_next(rb);
	UNIT_CHECK(fa && fb);
	UNIT_CHECK(display_from_time(fa, a->framerate) == 3);
	UNIT_CHECK(display_from_time(fb, b->framerate) == 2);
	plm_video_reverse_destroy(ra);
	plm_video_reverse_destroy(rb);
}

static void pixels_repeated_closed_stream() {
	Clip *clip = load_clip_cached("closed.m1v");
	std::vector<uint8_t> repeated = repeat_stream(clip->bytes, 4);
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		repeated.data(), repeated.size(), 8
	);
	UNIT_CHECK(reverse != NULL);
	size_t count = plm_video_reverse_get_count(reverse);
	UNIT_CHECK(count == clip->reference.size() * 4);
	for (size_t i = 0; i < count; i++) {
		plm_frame_t *frame = plm_video_reverse_next(reverse);
		UNIT_CHECK(frame != NULL);
		size_t display = display_from_time(frame, clip->framerate);
		size_t source = display % clip->reference.size();
		UNIT_CHECK(frame_pixels_match(
			frame, &clip->reference[source], clip->reference_pixels[source]
		));
	}
	plm_video_reverse_destroy(reverse);
}

// =============================================================================
// budget group
// =============================================================================

static void budget_full_reverse_bound() {
	Clip *clip = load_clip_cached("open.m1v");
	const size_t capacity = 9;
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), capacity
	);
	UNIT_CHECK(reverse != NULL);
	size_t count = plm_video_reverse_get_count(reverse);
	plm_video_reverse_stats_t before, after;
	plm_video_reverse_get_stats(reverse, &before);
	UNIT_CHECK(before.pictures_decoded == 0);
	int returned = 0;
	while (plm_video_reverse_next(reverse) != NULL) returned++;
	plm_video_reverse_get_stats(reverse, &after);
	UNIT_CHECK((size_t)returned == count);
	UNIT_CHECK(after.pictures_decoded <= 4 * count + 4 * capacity);
	UNIT_CHECK(after.cached_frames_peak <= capacity);
	UNIT_CHECK(after.cached_frames_peak > 0);
	plm_video_reverse_destroy(reverse);
}

static void budget_seek_bound() {
	Clip *clip = load_clip_cached("open.m1v");
	const size_t capacity = 9;
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), capacity
	);
	UNIT_CHECK(reverse != NULL);
	size_t count = plm_video_reverse_get_count(reverse);
	size_t probes[] = { count - 1, count / 2, 0, count / 3, count - 2, count - 5 };
	for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
		plm_video_reverse_stats_t before, after;
		plm_video_reverse_get_stats(reverse, &before);
		UNIT_CHECK(plm_video_reverse_seek(reverse, probes[p]) == 1);
		plm_frame_t *frame = plm_video_reverse_next(reverse);
		UNIT_CHECK(frame != NULL);
		plm_video_reverse_get_stats(reverse, &after);
		size_t delta = after.pictures_decoded - before.pictures_decoded;
		UNIT_CHECK(delta <= 4 * capacity + 4);
		UNIT_CHECK(delta > 0);
	}
	plm_video_reverse_destroy(reverse);
}

static void budget_consecutive_next_no_redecode() {
	Clip *clip = load_clip_cached("open.m1v");
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), 9
	);
	UNIT_CHECK(reverse != NULL);
	size_t count = plm_video_reverse_get_count(reverse);
	// Pick a frame in the middle of an I-anchored segment (I pictures display
	// at 0, 9, 18, 27, 36 for the supplied open clip) so stepping backwards
	// stays inside the same cached segment.
	UNIT_CHECK(plm_video_reverse_seek(reverse, 22) == 1);
	plm_video_reverse_stats_t before, after;
	plm_video_reverse_get_stats(reverse, &before);
	for (int i = 0; i < 4; i++) {
		UNIT_CHECK(plm_video_reverse_next(reverse) != NULL);
	}
	plm_video_reverse_get_stats(reverse, &after);
	UNIT_CHECK(after.pictures_decoded == before.pictures_decoded);
	UNIT_CHECK(count > 22);
	plm_video_reverse_destroy(reverse);
}

static void budget_repeated_stream_bound() {
	Clip *clip = load_clip_cached("open.m1v");
	std::vector<uint8_t> repeated = repeat_stream(clip->bytes, 6);
	const size_t capacity = 9;
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		repeated.data(), repeated.size(), capacity
	);
	UNIT_CHECK(reverse != NULL);
	size_t count = plm_video_reverse_get_count(reverse);
	UNIT_CHECK(count >= 200);
	plm_video_reverse_stats_t before, after;
	plm_video_reverse_get_stats(reverse, &before);
	UNIT_CHECK(before.pictures_decoded == 0);
	size_t returned = 0;
	while (plm_video_reverse_next(reverse) != NULL) returned++;
	plm_video_reverse_get_stats(reverse, &after);
	UNIT_CHECK(returned == count);
	UNIT_CHECK(after.pictures_decoded <= 4 * count + 4 * capacity);
	// Many groups: a per-group rebuild is much cheaper than replaying per frame.
	UNIT_CHECK(after.pictures_decoded < 4 * count);
	plm_video_reverse_destroy(reverse);
}

static void budget_capacity_peak_and_allocations() {
	Clip *clip = load_clip_cached("closed.m1v");
	const size_t capacity = 8;
	alloc_track::Snapshot start = alloc_track::snapshot();
	alloc_track::reset_peaks();
	plm_video_reverse_t *reverse = plm_video_reverse_create(
		clip->bytes.data(), clip->bytes.size(), capacity
	);
	UNIT_CHECK(reverse != NULL);
	alloc_track::Snapshot created = alloc_track::snapshot();
	plm_video_reverse_stats_t stats;
	plm_video_reverse_get_stats(reverse, &stats);
	UNIT_CHECK(stats.pictures_decoded == 0);
	UNIT_CHECK(stats.cached_frames_peak == 0);

	size_t on_create = created.live_bytes - start.live_bytes;
	UNIT_CHECK(created.allocation_calls > start.allocation_calls);
	size_t extra_allocation_bytes = on_create - (
		clip_frame_data_size(clip) * (capacity + 3)
	);

	size_t before_drain_allocations = alloc_track::allocation_calls;
	int returned = 0;
	while (plm_video_reverse_next(reverse) != NULL) returned++;
	size_t after_drain_allocations = alloc_track::allocation_calls;
	plm_video_reverse_get_stats(reverse, &stats);
	UNIT_CHECK((size_t)returned == plm_video_reverse_get_count(reverse));
	UNIT_CHECK(stats.cached_frames_peak <= capacity);
	UNIT_CHECK(stats.cached_frames_peak > 0);

	// No allocation at all happens while reversing: the cache and the decoder's
	// three work frames were created up front, and no pixels are allocated per
	// picture.
	UNIT_CHECK(after_drain_allocations == before_drain_allocations);

	// Beyond the pixel cache (capacity frames) and the three work frames, only
	// per-picture/per-group index entries and small structs exist. A second copy
	// of the compressed input would exceed one full stream length here.
	size_t index_and_groups_limit =
		clip->reference.size() * 128 + 2048;
	UNIT_CHECK(extra_allocation_bytes < clip->bytes.size());
	UNIT_CHECK(extra_allocation_bytes < index_and_groups_limit);

	plm_video_reverse_destroy(reverse);
	alloc_track::Snapshot destroyed = alloc_track::snapshot();
	UNIT_CHECK(destroyed.live_bytes == start.live_bytes);
	UNIT_CHECK(destroyed.live_blocks == start.live_blocks);
}

// =============================================================================
// registration
// =============================================================================

unit_case unit_cases[] = {
	{"index", "index_counts_open_gop", index_counts_open_gop},
	{"index", "index_stats_zero_pictures_after_create", index_stats_zero_pictures_after_create},
	{"index", "index_bytes_bounded_and_stable", index_bytes_bounded_and_stable},
	{"index", "index_capacity_requires_g", index_capacity_requires_g},
	{"index", "index_rejects_bad_arguments", index_rejects_bad_arguments},
	{"index", "index_seek_out_of_range_leaves_cursor", index_seek_out_of_range_leaves_cursor},
	{"index", "index_multiple_instances_independent", index_multiple_instances_independent},
	{"pixels", "pixels_reverse_matches_forward_closed", pixels_reverse_matches_forward_closed},
	{"pixels", "pixels_open_gop_leading_b_pictures", pixels_open_gop_leading_b_pictures},
	{"pixels", "pixels_cross_i_boundary_backwards", pixels_cross_i_boundary_backwards},
	{"pixels", "pixels_seek_repeated_across_segments", pixels_seek_repeated_across_segments},
	{"pixels", "pixels_exhaust_then_seek_rearm", pixels_exhaust_then_seek_rearm},
	{"pixels", "pixels_interleaved_instances", pixels_interleaved_instances},
	{"pixels", "pixels_repeated_closed_stream", pixels_repeated_closed_stream},
	{"budget", "budget_full_reverse_bound", budget_full_reverse_bound},
	{"budget", "budget_seek_bound", budget_seek_bound},
	{"budget", "budget_consecutive_next_no_redecode", budget_consecutive_next_no_redecode},
	{"budget", "budget_repeated_stream_bound", budget_repeated_stream_bound},
	{"budget", "budget_capacity_peak_and_allocations", budget_capacity_peak_and_allocations},
};

std::size_t unit_case_count = sizeof(unit_cases) / sizeof(unit_cases[0]);

// The fixed driver has no media argument; resolve the directory relative to the
// repository root that support/run.sh uses as its working directory.
static struct MediaDirInit {
	MediaDirInit() {
		const char *override = getenv("PLM_REVERSE_MEDIA");
		g_media_dir = override ? override : "support/media";
	}
} media_dir_init;
