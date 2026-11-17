#include "task_support/check.h"

#include "src/include/data/exception.hpp"
#include "src/include/data/lpc_decoded_data.hpp"
#include "src/include/data/lpc_encoded_data.hpp"
#include "src/include/data/rice_decoded_data.hpp"
#include "src/include/data/rice_encoded_data.hpp"
#include "src/include/data/sela_frame.hpp"
#include "src/include/data/sela_sub_frame.hpp"
#include "src/include/file/sela_file.hpp"
#include "src/include/file/wav_file.hpp"
#include "src/include/frame.hpp"
#include "src/include/frame/channel_forest.hpp"
#include "src/include/lpc.hpp"
#include "src/include/rice.hpp"
#include "src/include/sela/decoder.hpp"
#include "src/include/sela/encoder.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {
const size_t SAMPLES = 2048;

std::string tempPath(const char* name)
{
    const char* dir = std::getenv("TMPDIR");
    std::string path = (dir != 0 && dir[0] != 0) ? std::string(dir) : std::string("/tmp");
    path += "/sela_cf_";
    path += name;
    path += ".sela";
    return path;
}

std::vector<int32_t> sineWave(double frequency, double amplitude, double phase = 0.0)
{
    std::vector<int32_t> out(SAMPLES, 0);
    for (size_t i = 0; i < SAMPLES; i++) {
        out[i] = (int32_t)(amplitude * std::sin((double)i * frequency + phase));
    }
    return out;
}

std::vector<int32_t> noiseWave(uint32_t seed, int32_t amplitude)
{
    std::vector<int32_t> out(SAMPLES, 0);
    uint32_t state = seed;
    for (size_t i = 0; i < SAMPLES; i++) {
        // Deterministic LCG, keeps every test run reproducible.
        state = state * 1103515245u + 12345u;
        out[i] = (int32_t)((state >> 16) % (uint32_t)(2 * amplitude + 1)) - amplitude;
    }
    return out;
}

std::vector<int32_t> addWaves(const std::vector<int32_t>& left, const std::vector<int32_t>& right)
{
    std::vector<int32_t> out(left.size(), 0);
    for (size_t i = 0; i < left.size(); i++) {
        out[i] = left[i] + right[i];
    }
    return out;
}

std::vector<int32_t> negateWave(const std::vector<int32_t>& in)
{
    std::vector<int32_t> out(in.size(), 0);
    for (size_t i = 0; i < in.size(); i++) {
        out[i] = -in[i];
    }
    return out;
}

struct RicePair {
    data::RiceEncodedData reflection;
    data::RiceEncodedData residue;
    size_t byteCost;
};

// Costs are produced with the production LPC and Rice codecs; the forest
// selector itself is never used to generate expectations.
RicePair encodeCandidate(uint8_t bitsPerSample, const std::vector<int32_t>& signal)
{
    std::vector<int32_t> signalCopy = signal;
    data::LpcDecodedData lpcInput = data::LpcDecodedData(bitsPerSample, std::move(signalCopy));
    data::LpcEncodedData lpcOutput = lpc::ResidueGenerator(lpcInput).process();

    std::vector<int32_t> coefficients = lpcOutput.quantizedReflectionCoefficients;
    std::vector<int32_t> residues = lpcOutput.residues;
    data::RiceEncodedData reflection = rice::RiceEncoder(data::RiceDecodedData(std::move(coefficients))).process();
    data::RiceEncodedData residue = rice::RiceEncoder(data::RiceDecodedData(std::move(residues))).process();
    size_t byteCost = 12 + 4 * (reflection.encodedData.size() + residue.encodedData.size());
    return RicePair{ std::move(reflection), std::move(residue), byteCost };
}

size_t subFrameByteCost(const data::SelaSubFrame& subFrame)
{
    return 12 + 4 * (subFrame.encodedReflectionCoefficients.size() + subFrame.encodedResidues.size());
}

// Independent exhaustive oracle: every legal parent vector is enumerated,
// cyclic ones rejected, and the cheapest acyclic one kept with the required
// lexicographic tie-break (independent root recorded as channel count).
std::vector<uint8_t> exhaustiveForest(const std::vector<std::vector<size_t> >& costs)
{
    const size_t channelCount = costs.size();
    const size_t root = channelCount;
    std::vector<uint8_t> parents(channelCount, 0);
    std::vector<uint8_t> best(channelCount, (uint8_t)root);
    bool haveBest = false;
    size_t bestCost = 0;

    bool finished = false;
    while (!finished) {
        bool legal = true;
        for (size_t child = 0; child < channelCount; child++) {
            if (parents[child] == child) {
                legal = false;
                break;
            }
        }

        if (legal) {
            for (size_t start = 0; start < channelCount && legal; start++) {
                std::vector<bool> seen(channelCount, false);
                size_t node = start;
                while (parents[node] != root) {
                    if (seen[node]) {
                        legal = false;
                        break;
                    }
                    seen[node] = true;
                    node = parents[node];
                }
            }
        }

        if (legal) {
            size_t total = 0;
            for (size_t child = 0; child < channelCount; child++) {
                total += costs[child][parents[child]];
            }
            if (!haveBest || total < bestCost || (total == bestCost && parents < best)) {
                haveBest = true;
                bestCost = total;
                best = parents;
            }
        }

        size_t digit = 0;
        while (digit < channelCount) {
            if ((size_t)parents[digit] + 1 <= root) {
                parents[digit] = (uint8_t)(parents[digit] + 1);
                break;
            }
            parents[digit] = 0;
            digit++;
        }
        finished = digit == channelCount;
    }
    return best;
}

std::vector<std::vector<size_t> > costMatrix(const data::WavFrame& frame)
{
    const size_t channelCount = frame.samples.size();
    const size_t root = channelCount;
    std::vector<std::vector<size_t> > costs(channelCount, std::vector<size_t>(root + 1, 0));
    for (size_t child = 0; child < channelCount; child++) {
        for (size_t parent = 0; parent <= root; parent++) {
            if (parent == child) {
                costs[child][parent] = (size_t)-1;
                continue;
            }
            std::vector<int32_t> signal;
            signal.reserve(frame.samples[child].size());
            if (parent == root) {
                signal = frame.samples[child];
            } else {
                for (size_t i = 0; i < frame.samples[child].size(); i++) {
                    signal.push_back(frame.samples[parent][i] - frame.samples[child][i]);
                }
            }
            costs[child][parent] = encodeCandidate(frame.bitsPerSample, signal).byteCost;
        }
    }
    return costs;
}

data::WavFrame makeFrame(std::vector<std::vector<int32_t> > samples)
{
    return data::WavFrame(16, std::move(samples));
}

uint64_t fnv1a(const std::string& path)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    uint64_t hash = 14695981039346656037ULL;
    char byte;
    while (file.get(byte)) {
        hash ^= (uint64_t)(unsigned char)byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string writeSelaFile(const std::vector<data::SelaFrame>& frames, uint8_t channels, const char* name)
{
    std::string path = tempPath(name);
    std::vector<data::SelaFrame> owned = frames;
    file::SelaFile selaFile(44100, 16, channels, std::move(owned));
    std::ofstream output(path.c_str(), std::ios::binary);
    selaFile.writeToFile(output);
    output.close();
    return path;
}

std::string writeWavFile(const std::vector<data::WavFrame>& frames, uint8_t channels, const char* name)
{
    std::string path = tempPath(name);
    std::vector<data::WavFrame> owned = frames;
    file::WavFile wavFile(44100, 16, channels, std::move(owned));
    std::ofstream output(path.c_str(), std::ios::binary);
    wavFile.writeToFile(output);
    output.close();
    return path;
}

bool throwsDataExceptionFrame(const data::SelaFrame& frame)
{
    try {
        frame::FrameDecoder decoder(frame);
        decoder.process();
    } catch (const data::Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

bool throwsDataExceptionFile(const std::string& path)
{
    try {
        std::ifstream input(path.c_str(), std::ios::binary);
        sela::Decoder decoder(input);
        decoder.process();
    } catch (const data::Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

data::SelaFrame copyFrameWith(const data::SelaFrame& frame,
    void (*mutate)(std::vector<data::SelaSubFrame>&))
{
    // SelaSubFrame keeps const payload vectors, so it is copy-constructible
    // but not copy-assignable; build the mutated frame by copying elements.
    std::vector<data::SelaSubFrame> copied = frame.subFrames;
    mutate(copied);
    data::SelaFrame copy(frame.bitsPerSample);
    for (size_t i = 0; i < copied.size(); i++) {
        copy.subFrames.push_back(copied[i]);
    }
    return copy;
}

// --- Shared channel material: correlated, anti-correlated and cyclic trap ---
std::vector<data::WavFrame> referenceFrames()
{
    std::vector<data::WavFrame> frames;

    // Two strongly correlated channels.
    std::vector<int32_t> base = sineWave(0.073, 12000.0);
    std::vector<int32_t> quiet = noiseWave(7, 40);
    frames.push_back(makeFrame(std::vector<std::vector<int32_t> > { base, addWaves(base, quiet) }));

    // Three channels containing an anti-correlated channel.
    std::vector<int32_t> other = sineWave(0.041, 9000.0, 1.7);
    frames.push_back(makeFrame(std::vector<std::vector<int32_t> > {
        base, other, addWaves(negateWave(base), noiseWave(11, 60)) }));

    // Cyclic trap: each channel is one step away from the next, so every
    // greedy cheapest edge participates in a 0 -> 1 -> 2 -> 0 cycle.
    std::vector<int32_t> a = sineWave(0.05, 11000.0);
    std::vector<int32_t> b = addWaves(a, std::vector<int32_t>(SAMPLES, 1));
    std::vector<int32_t> c = addWaves(b, std::vector<int32_t>(SAMPLES, 1));
    frames.push_back(makeFrame(std::vector<std::vector<int32_t> > { a, b, c }));

    // Four channel chain plus an anti-correlated branch.
    std::vector<int32_t> d = addWaves(c, std::vector<int32_t>(SAMPLES, 1));
    frames.push_back(makeFrame(std::vector<std::vector<int32_t> > {
        a, b, c, addWaves(negateWave(a), noiseWave(13, 25)) }));
    (void)d;

    // Pseudo-random uncorrelated material.
    std::vector<std::vector<int32_t> > randomMaterial;
    for (uint32_t channel = 0; channel < 4; channel++) {
        randomMaterial.push_back(noiseWave(101u + channel * 97u, 15000));
    }
    frames.push_back(makeFrame(randomMaterial));
    return frames;
}
} // namespace

// =============================== optimal ====================================

static int optimal_synthetic_matrices()
{
    // One channel only has the independent root.
    std::vector<std::vector<size_t> > single(1, std::vector<size_t>(2, 0));
    single[0][0] = (size_t)-1;
    single[0][1] = 50;
    std::vector<uint8_t> singlePick = frame::selectChannelForest(single);
    TASK_CHECK(singlePick.size() == 1 && singlePick[0] == 1);

    // Hand-built matrices with deliberate ties; the lexicographically
    // smallest legal parent vector must win. The exhaustive oracle is the
    // independent reference for the exact vector.
    std::vector<std::vector<size_t> > tied(3, std::vector<size_t>(4, 100));
    for (size_t child = 0; child < 3; child++) {
        tied[child][child] = (size_t)-1;
    }
    std::vector<uint8_t> tiedPick = frame::selectChannelForest(tied);
    std::vector<uint8_t> tiedExpect = exhaustiveForest(tied);
    TASK_CHECK(tiedPick == tiedExpect);
    {
        // Independently verify the tie rule on the oracle result itself:
        // every acyclic vector with the same total cost is >= the winner.
        bool smallest = true;
        std::vector<uint8_t> probe(3, 0);
        bool finished = false;
        while (!finished) {
            bool legal = true;
            for (size_t child = 0; child < 3; child++) {
                if (probe[child] == child) {
                    legal = false;
                }
            }
            for (size_t start = 0; start < 3 && legal; start++) {
                std::vector<bool> seen(3, false);
                size_t node = start;
                while (probe[node] != 3) {
                    if (seen[node]) {
                        legal = false;
                        break;
                    }
                    seen[node] = true;
                    node = probe[node];
                }
            }
            if (legal) {
                size_t total = 0;
                for (size_t child = 0; child < 3; child++) {
                    total += tied[child][probe[child]];
                }
                if (total == 300 && probe < tiedExpect) {
                    smallest = false;
                }
            }
            size_t digit = 0;
            while (digit < 3) {
                if ((size_t)probe[digit] + 1 <= 3) {
                    probe[digit] = (uint8_t)(probe[digit] + 1);
                    break;
                }
                probe[digit] = 0;
                digit++;
            }
            finished = digit == 3;
        }
        TASK_CHECK(smallest);
    }

    // Cheapest edges form a cycle; the oracle and selector must break it
    // the same way, and the winner must itself be acyclic.
    std::vector<std::vector<size_t> > cyclic(3, std::vector<size_t>(4, 100));
    for (size_t child = 0; child < 3; child++) {
        cyclic[child][child] = (size_t)-1;
    }
    cyclic[0][1] = 10; // 0 wants parent 1
    cyclic[1][2] = 10; // 1 wants parent 2
    cyclic[2][0] = 10; // 2 wants parent 0 -> cycle, one edge must give way
    std::vector<uint8_t> cyclicPick = frame::selectChannelForest(cyclic);
    TASK_CHECK(cyclicPick == exhaustiveForest(cyclic));
    {
        size_t winnerCost = 0;
        for (size_t child = 0; child < 3; child++) {
            winnerCost += cyclic[child][cyclicPick[child]];
        }
        TASK_CHECK(winnerCost == 120);
        // Acyclicity check for the returned parent vector.
        for (size_t start = 0; start < 3; start++) {
            std::vector<bool> seen(3, false);
            size_t node = start;
            while (cyclicPick[node] != 3) {
                TASK_CHECK(!seen[node]);
                seen[node] = true;
                node = cyclicPick[node];
            }
        }
    }
    return 0;
}

static int optimal_exhaustive_signals()
{
    std::vector<data::WavFrame> frames = referenceFrames();
    for (size_t caseIndex = 0; caseIndex < frames.size(); caseIndex++) {
        const data::WavFrame& wavFrame = frames[caseIndex];
        std::vector<std::vector<size_t> > costs = costMatrix(wavFrame);
        std::vector<uint8_t> expected = exhaustiveForest(costs);
        std::vector<uint8_t> selected = frame::selectChannelForest(costs);
        TASK_CHECK(selected == expected);

        data::SelaFrame encoded = frame::FrameEncoder(wavFrame, true).process();
        TASK_CHECK(encoded.subFrames.size() == wavFrame.samples.size());

        size_t expectedTotal = 0;
        size_t observedTotal = 0;
        for (size_t child = 0; child < wavFrame.samples.size(); child++) {
            const data::SelaSubFrame& subFrame = encoded.subFrames[child];
            TASK_CHECK(subFrame.channel == child);
            TASK_CHECK(subFrame.parentChannelNumber == selected[child] ||
                (selected[child] == wavFrame.samples.size() && subFrame.subFrameType == 0));
            if (selected[child] == wavFrame.samples.size()) {
                TASK_CHECK(subFrame.subFrameType == 0);
                TASK_CHECK(subFrame.parentChannelNumber == child);
            } else {
                TASK_CHECK(subFrame.subFrameType == 1);
                TASK_CHECK(subFrame.parentChannelNumber == selected[child]);
            }

            // Payload provenance: bytes on disk must match a fresh LPC/Rice
            // encoding of the exact winning candidate signal.
            std::vector<int32_t> signal;
            signal.reserve(wavFrame.samples[child].size());
            if (subFrame.subFrameType == 0) {
                signal = wavFrame.samples[child];
            } else {
                for (size_t i = 0; i < SAMPLES; i++) {
                    signal.push_back(wavFrame.samples[subFrame.parentChannelNumber][i] - wavFrame.samples[child][i]);
                }
            }
            RicePair pair = encodeCandidate(wavFrame.bitsPerSample, signal);
            TASK_CHECK(subFrame.encodedReflectionCoefficients == pair.reflection.encodedData);
            TASK_CHECK(subFrame.encodedResidues == pair.residue.encodedData);
            TASK_CHECK(subFrame.reflectionCoefficientRiceParam == pair.reflection.optimumRiceParam);
            TASK_CHECK(subFrame.residueRiceParam == pair.residue.optimumRiceParam);
            TASK_CHECK(subFrame.optimumLpcOrder == pair.reflection.dataCount);

            expectedTotal += costs[child][selected[child]];
            observedTotal += subFrameByteCost(subFrame);
        }
        TASK_CHECK(expectedTotal == observedTotal);
    }
    return 0;
}

static int optimal_trap_and_input_limits()
{
    std::vector<data::WavFrame> frames = referenceFrames();
    const data::WavFrame& trap = frames[2];
    data::SelaFrame encoded = frame::FrameEncoder(trap, true).process();
    size_t dependent = 0;
    for (const data::SelaSubFrame& subFrame : encoded.subFrames) {
        dependent += subFrame.subFrameType == 1 ? 1 : 0;
    }
    // Greedy edges would form a cycle, so at least one difference edge has
    // to survive alongside an independent root.
    TASK_CHECK(dependent >= 1);
    TASK_CHECK(frame::FrameDecoder(encoded).process().samples == trap.samples);

    auto expectThrow = [](data::WavFrame bad) {
        try {
            frame::FrameEncoder(bad, true).process();
        } catch (const data::Exception&) {
            return true;
        } catch (...) {
            return false;
        }
        return false;
    };

    std::vector<std::vector<int32_t> > nine(9, std::vector<int32_t>(SAMPLES, 0));
    TASK_CHECK(expectThrow(makeFrame(nine)));

    std::vector<std::vector<int32_t> > shortFrame(2, std::vector<int32_t>(1024, 0));
    TASK_CHECK(expectThrow(makeFrame(shortFrame)));

    data::WavFrame wrongBits(24, std::vector<std::vector<int32_t> >(2, std::vector<int32_t>(SAMPLES, 0)));
    TASK_CHECK(expectThrow(wrongBits));
    return 0;
}

// ============================== dependencies ================================

static int dependencies_shuffled_multilevel()
{
    // Deterministic multi-level chain: 0 is a root, 1 = 0 + 1, 2 = 1 + 1,
    // so the cheapest forest has depth two; channel 3 is anti-correlated.
    std::vector<int32_t> a = sineWave(0.061, 10000.0);
    std::vector<int32_t> b = addWaves(a, std::vector<int32_t>(SAMPLES, 1));
    std::vector<int32_t> c = addWaves(b, std::vector<int32_t>(SAMPLES, 1));
    std::vector<int32_t> anti = addWaves(negateWave(a), noiseWave(23, 30));
    data::WavFrame wavFrame = makeFrame(std::vector<std::vector<int32_t> > { a, b, c, anti });

    data::SelaFrame encoded = frame::FrameEncoder(wavFrame, true).process();
    std::vector<uint8_t> parents;
    size_t maxDepth = 0;
    for (const data::SelaSubFrame& subFrame : encoded.subFrames) {
        parents.push_back(subFrame.subFrameType == 0 ? (uint8_t)encoded.subFrames.size() : subFrame.parentChannelNumber);
    }
    for (size_t start = 0; start < parents.size(); start++) {
        size_t depth = 0;
        size_t node = start;
        while (parents[node] != parents.size()) {
            node = parents[node];
            depth++;
        }
        maxDepth = maxDepth > depth ? maxDepth : depth;
    }
    TASK_CHECK(maxDepth >= 2);

    // Shuffle the stored subframes; decoding must not rely on storage order
    // and the output must still be ordered by channel number.
    data::SelaFrame shuffled(encoded.bitsPerSample);
    shuffled.subFrames.push_back(encoded.subFrames[3]);
    shuffled.subFrames.push_back(encoded.subFrames[1]);
    shuffled.subFrames.push_back(encoded.subFrames[0]);
    shuffled.subFrames.push_back(encoded.subFrames[2]);
    data::WavFrame decoded = frame::FrameDecoder(shuffled).process();
    TASK_CHECK(decoded.samples.size() == wavFrame.samples.size());
    for (size_t channel = 0; channel < wavFrame.samples.size(); channel++) {
        TASK_CHECK(decoded.samples[channel] == wavFrame.samples[channel]);
    }

    // Same shuffled frame through the file entry point.
    std::string path = writeSelaFile(std::vector<data::SelaFrame> { shuffled }, 4, "shuffled");
    std::ifstream input(path.c_str(), std::ios::binary);
    file::WavFile output = sela::Decoder(input).process();
    TASK_CHECK(output.wavChunk.dataSubChunk.wavFrames.size() == 1);
    TASK_CHECK(output.wavChunk.dataSubChunk.wavFrames[0].samples == wavFrame.samples);
    std::remove(path.c_str());
    return 0;
}

static int dependencies_illegal_structures()
{
    std::vector<int32_t> a = sineWave(0.05, 8000.0);
    std::vector<int32_t> b = addWaves(a, noiseWave(31, 20));
    std::vector<int32_t> c = addWaves(b, noiseWave(37, 20));
    data::WavFrame wavFrame = makeFrame(std::vector<std::vector<int32_t> > { a, b, c });
    data::SelaFrame good = frame::FrameEncoder(wavFrame, true).process();

    // Duplicate channel number: 0 .. N-1 is no longer covered exactly once.
    data::SelaFrame duplicate = copyFrameWith(good, [](std::vector<data::SelaSubFrame>& sub) {
        sub[2].channel = sub[1].channel;
    });
    TASK_CHECK(throwsDataExceptionFrame(duplicate));

    // Illegal subframe type.
    data::SelaFrame badType = copyFrameWith(good, [](std::vector<data::SelaSubFrame>& sub) {
        sub[0].subFrameType = 2;
    });
    TASK_CHECK(throwsDataExceptionFrame(badType));

    // Dependent subframe pointing at a parent that does not exist.
    data::SelaFrame missingParent = copyFrameWith(good, [](std::vector<data::SelaSubFrame>& sub) {
        sub[0].subFrameType = 1;
        sub[0].parentChannelNumber = 200;
    });
    TASK_CHECK(throwsDataExceptionFrame(missingParent));

    // Self dependency.
    data::SelaFrame selfParent = copyFrameWith(good, [](std::vector<data::SelaSubFrame>& sub) {
        sub[1].subFrameType = 1;
        sub[1].parentChannelNumber = 1;
    });
    TASK_CHECK(throwsDataExceptionFrame(selfParent));

    // Parent and child disagree on sample count.
    data::SelaFrame mismatched = copyFrameWith(good, [](std::vector<data::SelaSubFrame>& sub) {
        sub[2].subFrameType = 1;
        sub[2].parentChannelNumber = 0;
        sub[2].samplesPerChannel = (uint16_t)(sub[2].samplesPerChannel + 1);
    });
    TASK_CHECK(throwsDataExceptionFrame(mismatched));

    // Pure dependency cycle 0 -> 1 -> 2 -> 0.
    data::SelaFrame cyclic = copyFrameWith(good, [](std::vector<data::SelaSubFrame>& sub) {
        for (size_t i = 0; i < sub.size(); i++) {
            sub[i].subFrameType = 1;
            sub[i].parentChannelNumber = (uint8_t)((i + 1) % sub.size());
        }
    });
    TASK_CHECK(throwsDataExceptionFrame(cyclic));

    // Every invalid frame must also be rejected through the file entry.
    const data::SelaFrame* invalid[] = { &duplicate, &badType, &missingParent, &selfParent, &mismatched, &cyclic };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        std::string path = writeSelaFile(std::vector<data::SelaFrame> { *invalid[i] }, 3, "invalid");
        TASK_CHECK(throwsDataExceptionFile(path));
        std::remove(path.c_str());
    }
    return 0;
}

static int dependencies_worker_exception_propagation()
{
    // Multiple complete frames, one of them carrying a bad dependency: the
    // worker thread must hand the exception back instead of terminating.
    std::vector<data::WavFrame> goodFrames;
    for (int frame = 0; frame < 3; frame++) {
        std::vector<int32_t> a = sineWave(0.03 + frame * 0.01, 7000.0);
        std::vector<int32_t> b = addWaves(a, noiseWave(41u + frame, 15));
        goodFrames.push_back(makeFrame(std::vector<std::vector<int32_t> > { a, b }));
    }
    std::vector<data::SelaFrame> encoded;
    for (const data::WavFrame& frame : goodFrames) {
        encoded.push_back(frame::FrameEncoder(frame, true).process());
    }
    encoded[1].subFrames[0].subFrameType = 1;
    encoded[1].subFrames[0].parentChannelNumber = 1;
    encoded[1].subFrames[1].subFrameType = 1;
    encoded[1].subFrames[1].parentChannelNumber = 0;

    std::string path = writeSelaFile(encoded, 2, "workers");
    TASK_CHECK(throwsDataExceptionFile(path));
    std::remove(path.c_str());
    return 0;
}

// ============================= compatibility ================================

static int compatibility_legacy_bytes()
{
    // Exactly the frame the baseline tool fingerprints; flag-off output has
    // to stay byte identical to the pre-change library.
    std::vector<std::vector<int32_t> > samples(3, std::vector<int32_t>(SAMPLES));
    for (size_t i = 0; i < SAMPLES; i++) {
        for (size_t channel = 0; channel < 3; channel++) {
            samples[channel][i] = (int32_t)(12000 * std::sin(i * 0.073) + 5000 * std::cos(i * 0.021));
        }
    }
    data::WavFrame baselineFrame = makeFrame(samples);
    data::SelaFrame legacyFrame = frame::FrameEncoder(baselineFrame, false).process();
    for (const data::SelaSubFrame& subFrame : legacyFrame.subFrames) {
        TASK_CHECK(subFrame.subFrameType == 0);
        TASK_CHECK(subFrame.parentChannelNumber == subFrame.channel);
    }
    std::string path = writeSelaFile(std::vector<data::SelaFrame> { legacyFrame }, 3, "legacy");
    TASK_CHECK(fnv1a(path) == 11882950384908559994ULL);
    std::remove(path.c_str());

    // Two identical channels: the legacy path still attempts one difference
    // subframe and picks it when it is smaller.
    std::vector<int32_t> wave = sineWave(0.08, 12000.0);
    data::WavFrame identical = makeFrame(std::vector<std::vector<int32_t> > { wave, wave });
    data::SelaFrame legacyTwo = frame::FrameEncoder(identical, false).process();
    TASK_CHECK(legacyTwo.subFrames[0].subFrameType == 0);
    TASK_CHECK(legacyTwo.subFrames[1].subFrameType == 1);
    TASK_CHECK(legacyTwo.subFrames[1].parentChannelNumber == 0);

    // Uncorrelated stereo falls back to two independent subframes.
    data::WavFrame unrelated = makeFrame(std::vector<std::vector<int32_t> > {
        sineWave(0.02, 12000.0), noiseWave(59, 12000) });
    data::SelaFrame legacyIndependent = frame::FrameEncoder(unrelated, false).process();
    TASK_CHECK(legacyIndependent.subFrames[0].subFrameType == 0);
    TASK_CHECK(legacyIndependent.subFrames[1].subFrameType == 0);

    // Flag on and flag off both decode back to the same PCM. With two
    // identical channels the difference is a zero signal, so the forest
    // contains exactly one dependent edge; which channel is the child is
    // decided by the specified lexicographic tie rule, not assumed here.
    data::SelaFrame forestFrame = frame::FrameEncoder(identical, true).process();
    {
        std::vector<std::vector<size_t> > forestCosts = costMatrix(identical);
        std::vector<uint8_t> expectedParents = exhaustiveForest(forestCosts);
        size_t dependent = 0;
        for (size_t child = 0; child < expectedParents.size(); child++) {
            const data::SelaSubFrame& subFrame = forestFrame.subFrames[child];
            if (expectedParents[child] == expectedParents.size()) {
                TASK_CHECK(subFrame.subFrameType == 0);
            } else {
                TASK_CHECK(subFrame.subFrameType == 1);
                TASK_CHECK(subFrame.parentChannelNumber == expectedParents[child]);
                dependent++;
            }
        }
        TASK_CHECK(dependent == 1);
    }
    TASK_CHECK(frame::FrameDecoder(forestFrame).process().samples == identical.samples);
    TASK_CHECK(frame::FrameDecoder(legacyTwo).process().samples == identical.samples);
    return 0;
}

static int compatibility_old_files_new_decoder()
{
    // Old independent file (three channels) and old stereo difference file.
    std::vector<int32_t> a = sineWave(0.044, 11000.0);
    std::vector<int32_t> b = sineWave(0.029, 8000.0, 0.6);
    std::vector<int32_t> c = addWaves(a, noiseWave(67, 50));
    data::WavFrame three = makeFrame(std::vector<std::vector<int32_t> > { a, b, c });
    data::WavFrame stereo = makeFrame(std::vector<std::vector<int32_t> > { a, addWaves(a, noiseWave(71, 20)) });

    data::SelaFrame oldThree = frame::FrameEncoder(three, false).process();
    data::SelaFrame oldStereo = frame::FrameEncoder(stereo, false).process();
    TASK_CHECK(oldStereo.subFrames[1].subFrameType == 1);

    std::string threePath = writeSelaFile(std::vector<data::SelaFrame> { oldThree }, 3, "oldthree");
    std::string stereoPath = writeSelaFile(std::vector<data::SelaFrame> { oldStereo }, 2, "oldstereo");

    std::ifstream threeInput(threePath.c_str(), std::ios::binary);
    file::WavFile threeOut = sela::Decoder(threeInput).process();
    TASK_CHECK(threeOut.wavChunk.dataSubChunk.wavFrames[0].samples == three.samples);

    std::ifstream stereoInput(stereoPath.c_str(), std::ios::binary);
    file::WavFile stereoOut = sela::Decoder(stereoInput).process();
    TASK_CHECK(stereoOut.wavChunk.dataSubChunk.wavFrames[0].samples == stereo.samples);

    std::remove(threePath.c_str());
    std::remove(stereoPath.c_str());
    return 0;
}

static int compatibility_full_roundtrip_one_to_eight()
{
    for (uint8_t channelCount = 1; channelCount <= 8; channelCount++) {
        std::vector<data::WavFrame> wavFrames;
        for (int frameIndex = 0; frameIndex < 2; frameIndex++) {
            std::vector<std::vector<int32_t> > channels;
            for (uint8_t channel = 0; channel < channelCount; channel++) {
                std::vector<int32_t> wave = sineWave(0.02 + 0.007 * channel, 9000.0, channel * 0.4);
                if ((channel + frameIndex) % 2 == 0) {
                    wave = addWaves(wave, noiseWave(200u + channel * 13u + frameIndex, 35));
                }
                channels.push_back(wave);
            }
            wavFrames.push_back(makeFrame(channels));
        }

        std::string wavPath = writeWavFile(wavFrames, channelCount, "roundtrip");
        std::ifstream input(wavPath.c_str(), std::ios::binary);
        file::SelaFile selaFile = sela::Encoder(input, true).process();

        std::string selaPath = tempPath("encoded");
        std::ofstream output(selaPath.c_str(), std::ios::binary);
        selaFile.writeToFile(output);
        output.close();

        std::ifstream encodedInput(selaPath.c_str(), std::ios::binary);
        file::WavFile decodedFile = sela::Decoder(encodedInput).process();
        const std::vector<data::WavFrame>& decoded = decodedFile.wavChunk.dataSubChunk.wavFrames;
        TASK_CHECK(decoded.size() == wavFrames.size());
        for (size_t frame = 0; frame < wavFrames.size(); frame++) {
            TASK_CHECK(decoded[frame].samples.size() == wavFrames[frame].samples.size());
            for (size_t channel = 0; channel < wavFrames[frame].samples.size(); channel++) {
                TASK_CHECK(decoded[frame].samples[channel] == wavFrames[frame].samples[channel]);
            }
        }
        std::remove(wavPath.c_str());
        std::remove(selaPath.c_str());
    }
    return 0;
}

extern const struct task_case task_cases[] = {
    { "optimal", "synthetic_matrices", optimal_synthetic_matrices },
    { "optimal", "exhaustive_signals", optimal_exhaustive_signals },
    { "optimal", "trap_and_input_limits", optimal_trap_and_input_limits },
    { "dependencies", "shuffled_multilevel", dependencies_shuffled_multilevel },
    { "dependencies", "illegal_structures", dependencies_illegal_structures },
    { "dependencies", "worker_exception_propagation", dependencies_worker_exception_propagation },
    { "compatibility", "legacy_bytes", compatibility_legacy_bytes },
    { "compatibility", "old_files_new_decoder", compatibility_old_files_new_decoder },
    { "compatibility", "full_roundtrip_1_to_8", compatibility_full_roundtrip_one_to_eight },
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
