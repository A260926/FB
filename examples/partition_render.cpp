/*
    partition_render.cpp

    Example for RubberBand::R2RenderIndex: offline, partitioned
    time-stretch rendering across independent worker processes.

    Usage:
        partition_render prepare DIR
        partition_render worker DIR FIRST COUNT OUT

    prepare generates two different 24000-frame mono tracks, analyses
    each once into a serialisable render index, and stores the
    sequential whole-track reference mix.  Any number of worker
    processes can then render arbitrary (overlapping, out-of-order)
    output ranges from the saved indices without re-analysing the
    source; the concatenated worker output is identical to the
    sequential reference.
*/

#include "rubberband/R2RenderIndex.h"
#include "rubberband/RubberBandStretcher.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using RubberBand::R2RenderIndex;
using RubberBand::RubberBandStretcher;

namespace
{

const size_t INPUT_FRAMES = 24000;
const size_t OUTPUT_FRAMES = 30000;
const double TIME_RATIO = 1.25;

const std::map<size_t, size_t> KEY_FRAMES = {
    {0, 0}, {12000, 8000}, {24000, 30000}
};

struct FileSource {
    std::vector<float> frames;
};

bool
readAt(void *context, size_t first, size_t count, float *const *output)
{
    const FileSource &s = *static_cast<const FileSource *>(context);
    if (first > s.frames.size() || count > s.frames.size() - first) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        output[0][i] = s.frames[first + i];
    }
    return true;
}

void
writeF32(const std::string &path, const std::vector<float> &data)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot open " + path + " for write");
    f.write(reinterpret_cast<const char *>(data.data()),
            std::streamsize(data.size() * sizeof(float)));
    if (!f) throw std::runtime_error("write failed: " + path);
}

std::vector<float>
readF32(const std::string &path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::streamsize bytes = f.tellg();
    if (bytes < 0 || bytes % sizeof(float) != 0) {
        throw std::runtime_error("bad file size: " + path);
    }
    f.seekg(0);
    std::vector<float> data(size_t(bytes) / sizeof(float));
    if (!f.read(reinterpret_cast<char *>(data.data()), bytes)) {
        throw std::runtime_error("read failed: " + path);
    }
    return data;
}

// Two deliberately different mono tracks.
std::vector<float>
makeTrack(unsigned which)
{
    std::vector<float> x(INPUT_FRAMES);
    for (size_t i = 0; i < INPUT_FRAMES; ++i) {
        if (which == 0) {
            x[i] = float(0.21 * std::sin(0.0576 * i) +
                         0.10 * std::cos(0.0331 * i) +
                         ((i % 4000) < 16 ? 0.30 : 0.0));
        } else {
            x[i] = float(0.17 * std::sin(0.0413 * i + 1.7) +
                         0.12 * std::cos(0.0207 * i * 1.3) +
                         ((i % 3333) < 11 ? 0.27 : 0.0));
        }
    }
    return x;
}

// Sequential whole-track R2 reference for one mono track.
std::vector<float>
sequentialReference(const std::vector<float> &source)
{
    RubberBandStretcher s(
        48000, 1,
        RubberBandStretcher::OptionEngineFaster |
        RubberBandStretcher::OptionThreadingNever |
        RubberBandStretcher::OptionPhaseIndependent,
        TIME_RATIO, 1.0);
    s.setExpectedInputDuration(source.size());
    s.setMaxProcessSize(4096);
    s.setKeyFrameMap(KEY_FRAMES);

    const float *p = source.data();
    s.study(&p, source.size(), true);

    std::vector<float> out;
    for (size_t pos = 0; pos < source.size();) {
        size_t k = std::min<size_t>(4096, source.size() - pos);
        p = source.data() + pos;
        s.process(&p, k, pos + k == source.size());
        pos += k;
        while (s.available() > 0) {
            std::vector<float> buf(s.available());
            float *q = buf.data();
            size_t got = s.retrieve(&q, buf.size());
            out.insert(out.end(), buf.begin(), buf.begin() + got);
        }
    }
    return out;
}

int
runPrepare(const std::string &dir)
{
    for (unsigned track = 0; track < 2; ++track) {
        std::vector<float> source = makeTrack(track);
        writeF32(dir + "/track" + std::to_string(track) + ".f32", source);

        FileSource fs;
        fs.frames = source;
        std::shared_ptr<const R2RenderIndex> index = R2RenderIndex::build(
            48000, 1, INPUT_FRAMES, TIME_RATIO, 4096, KEY_FRAMES,
            readAt, &fs);
        if (!index) {
            std::fprintf(stderr, "prepare: index build failed\n");
            return 1;
        }

        std::ofstream idx(dir + "/track" + std::to_string(track) + ".idx",
                          std::ios::binary | std::ios::trunc);
        if (!idx) {
            std::fprintf(stderr, "prepare: cannot write index\n");
            return 1;
        }
        index->save(idx);
    }

    // Sequential reference: stretch each track fully, then mix.
    std::vector<float> ref0 = sequentialReference(makeTrack(0));
    std::vector<float> ref1 = sequentialReference(makeTrack(1));
    if (ref0.size() != OUTPUT_FRAMES || ref1.size() != OUTPUT_FRAMES) {
        std::fprintf(stderr, "prepare: unexpected reference length\n");
        return 1;
    }
    std::vector<float> mix(OUTPUT_FRAMES);
    for (size_t i = 0; i < OUTPUT_FRAMES; ++i) {
        mix[i] = 0.6f * ref0[i] + 0.4f * ref1[i];
    }
    writeF32(dir + "/reference-mix.f32", mix);
    std::printf("prepare: wrote 2 tracks, 2 indices and reference mix "
                "(%zu frames) to %s\n", OUTPUT_FRAMES, dir.c_str());
    return 0;
}

int
runWorker(const std::string &dir, size_t first, size_t count,
          const std::string &outPath)
{
    FileSource sources[2];
    std::shared_ptr<const R2RenderIndex> indices[2];

    for (unsigned track = 0; track < 2; ++track) {
        sources[track].frames =
            readF32(dir + "/track" + std::to_string(track) + ".f32");
        std::ifstream idx(dir + "/track" + std::to_string(track) + ".idx",
                          std::ios::binary);
        if (!idx) {
            std::fprintf(stderr, "worker: cannot open index %u\n", track);
            return 1;
        }
        indices[track] = R2RenderIndex::load(idx);
        if (!indices[track]) {
            std::fprintf(stderr, "worker: invalid index %u\n", track);
            return 1;
        }
    }

    std::vector<float> rendered[2];
    for (unsigned track = 0; track < 2; ++track) {
        rendered[track].assign(count, 0.f);
        float *dst = rendered[track].data();
        if (!indices[track]->render(first, count, readAt,
                                    &sources[track], &dst)) {
            std::fprintf(stderr, "worker: render failed for track %u\n",
                         track);
            return 1;
        }
    }

    std::vector<float> mix(count);
    for (size_t i = 0; i < count; ++i) {
        mix[i] = 0.6f * rendered[0][i] + 0.4f * rendered[1][i];
    }
    writeF32(outPath, mix);
    std::printf("worker: rendered [%zu, %zu) (%zu frames) to %s\n",
                first, first + count, count, outPath.c_str());
    return 0;
}

} // namespace

int
main(int argc, char **argv)
{
    try {
        if (argc == 3 && std::string(argv[1]) == "prepare") {
            return runPrepare(argv[2]);
        }
        if (argc == 6 && std::string(argv[1]) == "worker") {
            size_t first = std::stoull(argv[3]);
            size_t count = std::stoull(argv[4]);
            return runWorker(argv[2], first, count, argv[5]);
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "partition_render: %s\n", e.what());
        return 1;
    }
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s prepare DIR\n"
                 "  %s worker DIR FIRST COUNT OUT\n", argv[0], argv[0]);
    return 2;
}
