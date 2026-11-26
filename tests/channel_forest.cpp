#include "task_support/check.h"

#include "src/include/channel_forest.hpp"
#include "src/include/data/exception.hpp"
#include "src/include/file/sela_file.hpp"
#include "src/include/file/wav_file.hpp"
#include "src/include/frame.hpp"
#include "src/include/lpc.hpp"
#include "src/include/rice.hpp"
#include "src/include/sela/decoder.hpp"
#include "src/include/sela/encoder.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace {

const size_t SAMPLES = 2048;
typedef std::vector<std::vector<int32_t> > Pcm;

#define CHECK_PCM(expectedPcm, actualPcm)                                                     \
    do {                                                                                      \
        const Pcm& expectedPcm_ = (expectedPcm);                                               \
        const Pcm& actualPcm_ = (actualPcm);                                                   \
        TASK_CHECK(expectedPcm_.size() == actualPcm_.size());                                  \
        for (size_t channel_ = 0; channel_ < expectedPcm_.size(); ++channel_) {                \
            TASK_CHECK(expectedPcm_[channel_].size() == actualPcm_[channel_].size());         \
            for (size_t sample_ = 0; sample_ < expectedPcm_[channel_].size(); ++sample_) {     \
                TASK_CHECK(expectedPcm_[channel_][sample_] == actualPcm_[channel_][sample_]); \
            }                                                                                 \
        }                                                                                     \
    } while (0)

Pcm makeCycleMaterials()
{
    Pcm pcm(4, std::vector<int32_t>(SAMPLES));
    for (size_t i = 0; i < SAMPLES; ++i) {
        const double t = static_cast<double>(i);
        const int32_t base = static_cast<int32_t>(
            11000.0 * std::sin(t * 0.031) + 6000.0 * std::cos(t * 0.017)
            + 2000.0 * std::sin(t * 0.113));
        const int32_t relatedNoise = static_cast<int32_t>(
            900.0 * std::sin(t * 0.071) + 300.0 * std::cos(t * 0.191));
        const int32_t tinyDifference = static_cast<int32_t>(
            500.0 * std::sin(t * 0.097) + 250.0 * std::cos(t * 0.211));

        pcm[0][i] = base;
        pcm[1][i] = static_cast<int32_t>(base / 2) + relatedNoise;
        pcm[2][i] = static_cast<int32_t>(-base) + relatedNoise;
        pcm[3][i] = base + tinyDifference;
    }
    return pcm;
}

double channelCorrelation(const Pcm& pcm, size_t a, size_t b)
{
    double meanA = 0.0;
    double meanB = 0.0;
    for (size_t i = 0; i < SAMPLES; ++i) {
        meanA += pcm[a][i];
        meanB += pcm[b][i];
    }
    meanA /= static_cast<double>(SAMPLES);
    meanB /= static_cast<double>(SAMPLES);

    double numerator = 0.0;
    double varianceA = 0.0;
    double varianceB = 0.0;
    for (size_t i = 0; i < SAMPLES; ++i) {
        const double da = static_cast<double>(pcm[a][i]) - meanA;
        const double db = static_cast<double>(pcm[b][i]) - meanB;
        numerator += da * db;
        varianceA += da * da;
        varianceB += db * db;
    }
    return numerator / std::sqrt(varianceA * varianceB);
}

data::SelaSubFrame encodeOriginalCandidate(const Pcm& pcm, size_t child, int parent)
{
    std::vector<int32_t> signal(SAMPLES);
    if (parent < 0) {
        signal = pcm[child];
    } else {
        for (size_t i = 0; i < SAMPLES; ++i) {
            signal[i] = pcm[static_cast<size_t>(parent)][i] - pcm[child][i];
        }
    }

    // Deliberately use the production LPC and Rice implementations rather
    // than a test-only cost approximation.
    data::LpcDecodedData lpcInput(16, std::move(signal));
    lpc::ResidueGenerator residueGenerator(lpcInput);
    data::LpcEncodedData lpcOutput = residueGenerator.process();

    data::RiceDecodedData reflectionInput(std::move(lpcOutput.quantizedReflectionCoefficients));
    rice::RiceEncoder reflectionEncoder(reflectionInput);
    data::RiceEncodedData reflectionOutput = reflectionEncoder.process();

    data::RiceDecodedData residueInput(std::move(lpcOutput.residues));
    rice::RiceEncoder residueEncoder(residueInput);
    data::RiceEncodedData residueOutput = residueEncoder.process();

    const uint8_t type = parent < 0 ? 0 : 1;
    const uint8_t parentField = parent < 0
        ? static_cast<uint8_t>(child)
        : static_cast<uint8_t>(parent);
    return data::SelaSubFrame(static_cast<uint8_t>(child), type, parentField,
        reflectionOutput, residueOutput);
}

uint64_t subFrameCost(const data::SelaSubFrame& subFrame)
{
    return 12 + 4 * (static_cast<uint64_t>(subFrame.encodedReflectionCoefficients.size())
                         + static_cast<uint64_t>(subFrame.encodedResidues.size()));
}

bool sameSubFramePayload(const data::SelaSubFrame& a, const data::SelaSubFrame& b)
{
    return a.channel == b.channel
        && a.subFrameType == b.subFrameType
        && a.parentChannelNumber == b.parentChannelNumber
        && a.reflectionCoefficientRiceParam == b.reflectionCoefficientRiceParam
        && a.reflectionCoefficientRequiredInts == b.reflectionCoefficientRequiredInts
        && a.optimumLpcOrder == b.optimumLpcOrder
        && a.encodedReflectionCoefficients == b.encodedReflectionCoefficients
        && a.residueRiceParam == b.residueRiceParam
        && a.residueRequiredInts == b.residueRequiredInts
        && a.samplesPerChannel == b.samplesPerChannel
        && a.encodedResidues == b.encodedResidues;
}

bool bruteAcyclicCodes(const std::vector<int>& codes)
{
    const size_t n = codes.size();
    for (size_t start = 0; start < n; ++start) {
        std::vector<char> seen(n, 0);
        size_t current = start;
        while (true) {
            const int code = codes[current];
            if (code == static_cast<int>(n)) {
                break;
            }
            if (code < 0 || static_cast<size_t>(code) >= n
                || static_cast<size_t>(code) == current || seen[current]) {
                return false;
            }
            seen[current] = 1;
            current = static_cast<size_t>(code);
        }
    }
    return true;
}

void bruteEnumerate(size_t channel, size_t n, std::vector<int>& codes,
    const std::vector<uint64_t>& rootCosts,
    const std::vector<std::vector<uint64_t> >& parentCosts,
    std::vector<int>& bestCodes, uint64_t& bestCost)
{
    if (channel == n) {
        if (!bruteAcyclicCodes(codes)) {
            return;
        }

        uint64_t totalCost = 0;
        for (size_t c = 0; c < n; ++c) {
            if (codes[c] == static_cast<int>(n)) {
                totalCost += rootCosts[c];
            } else {
                totalCost += parentCosts[c][static_cast<size_t>(codes[c])];
            }
        }

        if (bestCodes.empty() || totalCost < bestCost
            || (totalCost == bestCost && codes < bestCodes)) {
            bestCost = totalCost;
            bestCodes = codes;
        }
        return;
    }

    // Ascending actual tie codes: other channel ids, then root == n.
    for (int code = 0; code <= static_cast<int>(n); ++code) {
        if (code == static_cast<int>(channel)) {
            continue;
        }
        codes[channel] = code;
        bruteEnumerate(channel + 1, n, codes, rootCosts, parentCosts, bestCodes, bestCost);
    }
}

std::vector<int> independentBruteForest(
    const std::vector<uint64_t>& rootCosts,
    const std::vector<std::vector<uint64_t> >& parentCosts,
    uint64_t& bestCost)
{
    const size_t n = rootCosts.size();
    std::vector<int> codes(n, 0);
    std::vector<int> bestCodes;
    bestCost = std::numeric_limits<uint64_t>::max();
    bruteEnumerate(0, n, codes, rootCosts, parentCosts, bestCodes, bestCost);
    return bestCodes;
}

std::vector<int> selectionCodes(const frame::ChannelForestSelection& selection, size_t n)
{
    return frame::channelForestTieCodes(selection.parent, n);
}

void writeOneSelaFrame(const std::string& path, const data::SelaFrame& frame, uint8_t channels)
{
    std::vector<data::SelaFrame> frames;
    frames.push_back(frame);
    file::SelaFile selaFile(44100, 16, channels, std::move(frames));
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    selaFile.writeToFile(output);
}

uint64_t fingerprintFile(const std::string& path)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    uint64_t hash = 14695981039346656037ULL;
    char byte = 0;
    while (input.get(byte)) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(byte));
        hash *= 1099511628211ULL;
    }
    return hash;
}

file::WavFile encodeDecodeWavThroughFiles(const std::string& wavPath,
    const std::string& selaPath, const Pcm& pcm, size_t frameCount, bool forest)
{
    std::vector<data::WavFrame> wavFrames;
    for (size_t frame = 0; frame < frameCount; ++frame) {
        (void)frame;
        wavFrames.push_back(data::WavFrame(16, pcm));
    }

    file::WavFile wavFile(44100, 16, static_cast<uint16_t>(pcm.size()), std::move(wavFrames));
    {
        std::ofstream wavOutput(wavPath.c_str(), std::ios::binary | std::ios::trunc);
        wavFile.writeToFile(wavOutput);
    }

    {
        std::ifstream wavInput(wavPath.c_str(), std::ios::binary);
        sela::Encoder encoder(wavInput, forest);
        file::SelaFile encodedSela = encoder.process();
        std::ofstream selaOutput(selaPath.c_str(), std::ios::binary | std::ios::trunc);
        encodedSela.writeToFile(selaOutput);
    }

    std::ifstream selaInput(selaPath.c_str(), std::ios::binary);
    sela::Decoder decoder(selaInput);
    return decoder.process();
}

int selectorMatchesExhaustiveCostMatrices()
{
    uint64_t randomState = 0x123456789abcdef0ULL;
    for (size_t n = 1; n <= 5; ++n) {
        for (unsigned trial = 0; trial < 24; ++trial) {
            std::vector<uint64_t> rootCosts(n);
            std::vector<std::vector<uint64_t> > parentCosts(n, std::vector<uint64_t>(n));
            for (size_t c = 0; c < n; ++c) {
                randomState ^= randomState << 7;
                randomState ^= randomState >> 9;
                rootCosts[c] = 12 + (randomState % 37);
                for (size_t p = 0; p < n; ++p) {
                    randomState ^= randomState << 13;
                    randomState ^= randomState >> 7;
                    parentCosts[c][p] = 12 + (randomState % 41);
                }
            }

            uint64_t expectedCost = 0;
            std::vector<int> expected = independentBruteForest(rootCosts, parentCosts, expectedCost);
            frame::ChannelForestSelection actual =
                frame::selectOptimalChannelForest(rootCosts, parentCosts);

            TASK_CHECK(selectionCodes(actual, n) == expected);
            TASK_CHECK(actual.totalCost == expectedCost);
            TASK_CHECK(frame::isAcyclicChannelForest(actual.parent, n));
        }
    }

    // Explicit all-equal tie cases.  The lex-smallest legal vectors are chains
    // ending in a root because choosing an earlier back-edge creates a cycle.
    for (size_t n = 2; n <= 4; ++n) {
        std::vector<uint64_t> rootCosts(n, 9);
        std::vector<std::vector<uint64_t> > parentCosts(n, std::vector<uint64_t>(n, 9));
        frame::ChannelForestSelection selection =
            frame::selectOptimalChannelForest(rootCosts, parentCosts);
        std::vector<int> codes = selectionCodes(selection, n);
        for (size_t c = 0; c < n - 1; ++c) {
            TASK_CHECK(codes[c] == static_cast<int>(c + 1));
        }
        TASK_CHECK(codes[n - 1] == static_cast<int>(n));
        TASK_CHECK(selection.totalCost == static_cast<uint64_t>(n) * 9);
    }

    // Eight channels is the production limit.  This exact enumeration must be
    // fast and must apply the same lex tie rule.
    const size_t eight = 8;
    std::vector<uint64_t> rootCosts(eight, 123);
    std::vector<std::vector<uint64_t> > parentCosts(eight, std::vector<uint64_t>(eight, 123));
    frame::ChannelForestSelection eightSelection =
        frame::selectOptimalChannelForest(rootCosts, parentCosts);
    std::vector<int> eightCodes = selectionCodes(eightSelection, eight);
    for (size_t c = 0; c < eight - 1; ++c) {
        TASK_CHECK(eightCodes[c] == static_cast<int>(c + 1));
    }
    TASK_CHECK(eightCodes[eight - 1] == static_cast<int>(eight));
    TASK_CHECK(eightSelection.totalCost == eight * 123);

    return 0;
}

int audioCandidatesMatchIndependentExhaustive()
{
    Pcm pcm = makeCycleMaterials();
    data::WavFrame wavFrame(16, pcm);
    data::SelaFrame encoded = frame::FrameEncoder(wavFrame, true).process();

    // Encoding must not alter the caller's PCM.
    CHECK_PCM(pcm, wavFrame.samples);

    const size_t n = pcm.size();
    std::vector<uint64_t> rootCosts(n);
    std::vector<std::vector<uint64_t> > parentCosts(n, std::vector<uint64_t>(n));
    std::vector<size_t> rootIndex(n);
    std::vector<std::vector<size_t> > parentIndex(n, std::vector<size_t>(n));
    std::vector<data::SelaSubFrame> expectedCandidates;
    expectedCandidates.reserve(n * n);

    for (size_t child = 0; child < n; ++child) {
        rootIndex[child] = expectedCandidates.size();
        expectedCandidates.push_back(encodeOriginalCandidate(pcm, child, -1));
        rootCosts[child] = subFrameCost(expectedCandidates[rootIndex[child]]);

        for (size_t parent = 0; parent < n; ++parent) {
            if (parent == child) {
                continue;
            }
            parentIndex[child][parent] = expectedCandidates.size();
            expectedCandidates.push_back(
                encodeOriginalCandidate(pcm, child, static_cast<int>(parent)));
            parentCosts[child][parent] =
                subFrameCost(expectedCandidates[parentIndex[child][parent]]);
        }
    }

    uint64_t expectedCost = 0;
    std::vector<int> bestCodes = independentBruteForest(rootCosts, parentCosts, expectedCost);
    frame::ChannelForestSelection selection =
        frame::selectOptimalChannelForest(rootCosts, parentCosts);
    TASK_CHECK(selectionCodes(selection, n) == bestCodes);
    TASK_CHECK(selection.totalCost == expectedCost);

    TASK_CHECK(encoded.subFrames.size() == n);
    uint64_t actualTotalCost = 0;
    for (size_t child = 0; child < n; ++child) {
        const data::SelaSubFrame& actual = encoded.subFrames[child];
        TASK_CHECK(actual.channel == static_cast<uint8_t>(child));
        actualTotalCost += subFrameCost(actual);

        const int code = bestCodes[child];
        if (code == static_cast<int>(n)) {
            TASK_CHECK(actual.subFrameType == 0);
            // Independent subframes retain the legacy self-parent field.
            TASK_CHECK(actual.parentChannelNumber == static_cast<uint8_t>(child));
            const data::SelaSubFrame& expected = expectedCandidates[rootIndex[child]];
            TASK_CHECK(sameSubFramePayload(actual, expected));
        } else {
            TASK_CHECK(actual.subFrameType == 1);
            TASK_CHECK(actual.parentChannelNumber == static_cast<uint8_t>(code));
            const data::SelaSubFrame& expected =
                expectedCandidates[parentIndex[child][static_cast<size_t>(code)]];
            TASK_CHECK(sameSubFramePayload(actual, expected));
        }
    }
    TASK_CHECK(actualTotalCost == expectedCost);

    data::WavFrame decoded = frame::FrameDecoder(encoded).process();
    CHECK_PCM(pcm, decoded.samples);
    return 0;
}

int cycleRelatedAndAntiRelatedMaterials()
{
    Pcm pcm = makeCycleMaterials();
    TASK_CHECK(channelCorrelation(pcm, 0, 1) > 0.90);
    TASK_CHECK(channelCorrelation(pcm, 0, 2) < -0.90);

    const size_t n = pcm.size();
    std::vector<uint64_t> rootCosts(n);
    std::vector<std::vector<uint64_t> > parentCosts(n, std::vector<uint64_t>(n));
    for (size_t child = 0; child < n; ++child) {
        rootCosts[child] = subFrameCost(encodeOriginalCandidate(pcm, child, -1));
        for (size_t parent = 0; parent < n; ++parent) {
            if (parent != child) {
                parentCosts[child][parent] = subFrameCost(
                    encodeOriginalCandidate(pcm, child, static_cast<int>(parent)));
            }
        }
    }

    auto cheapestParent = [&](size_t child) {
        int best = -1;
        uint64_t bestCost = rootCosts[child];
        for (size_t parent = 0; parent < n; ++parent) {
            if (parent == child) {
                continue;
            }
            if (parentCosts[child][parent] < bestCost) {
                bestCost = parentCosts[child][parent];
                best = static_cast<int>(parent);
            }
        }
        return best;
    };

    // Channels 0 and 3 are each other's unique cheapest edge, so a naive
    // per-channel greedy choice contains a two-cycle.
    TASK_CHECK(cheapestParent(0) == 3);
    TASK_CHECK(cheapestParent(3) == 0);

    std::vector<int> greedyParent(n, -1);
    uint64_t greedyCost = 0;
    for (size_t child = 0; child < n; ++child) {
        const int parent = cheapestParent(child);
        if (parent < 0) {
            greedyCost += rootCosts[child];
        } else {
            greedyParent[child] = parent;
            greedyCost += parentCosts[child][static_cast<size_t>(parent)];
        }
    }
    TASK_CHECK(!frame::isAcyclicChannelForest(greedyParent, n));

    // A common greedy repair is to replace one edge in the cycle by a root.
    const uint64_t breakCycleAtZero =
        greedyCost - parentCosts[0][3] + rootCosts[0];
    const uint64_t breakCycleAtThree =
        greedyCost - parentCosts[3][0] + rootCosts[3];
    const uint64_t naiveRepairCost = breakCycleAtZero < breakCycleAtThree
        ? breakCycleAtZero
        : breakCycleAtThree;

    frame::ChannelForestSelection selection =
        frame::selectOptimalChannelForest(rootCosts, parentCosts);
    TASK_CHECK(selection.totalCost < naiveRepairCost);

    data::SelaFrame encoded = frame::FrameEncoder(data::WavFrame(16, pcm), true).process();
    std::vector<int> selectedParent(n, -1);
    for (size_t c = 0; c < n; ++c) {
        if (encoded.subFrames[c].subFrameType == 1) {
            selectedParent[c] = encoded.subFrames[c].parentChannelNumber;
        }
    }
    TASK_CHECK(selectedParent == selection.parent);
    TASK_CHECK(frame::isAcyclicChannelForest(selectedParent, n));
    TASK_CHECK(!(selectedParent[0] == 3 && selectedParent[3] == 0));

    data::WavFrame decoded = frame::FrameDecoder(encoded).process();
    CHECK_PCM(pcm, decoded.samples);
    return 0;
}

data::SelaFrame makeLegalMultilayerFrame(const Pcm& pcm)
{
    data::SelaFrame frame(16);
    // 3 -> 0 -> 1(root), while channel 2 is another root.
    frame.subFrames.push_back(encodeOriginalCandidate(pcm, 0, 1));
    frame.subFrames.push_back(encodeOriginalCandidate(pcm, 1, -1));
    frame.subFrames.push_back(encodeOriginalCandidate(pcm, 2, -1));
    frame.subFrames.push_back(encodeOriginalCandidate(pcm, 3, 0));
    return frame;
}

data::SelaFrame makeShuffledMultilayerFrame(const Pcm& pcm)
{
    data::SelaFrame legal = makeLegalMultilayerFrame(pcm);
    data::SelaFrame shuffled(16);
    static const size_t order[] = { 3, 0, 2, 1 };
    for (size_t index = 0; index < sizeof(order) / sizeof(order[0]); ++index) {
        shuffled.subFrames.push_back(legal.subFrames[order[index]]);
    }
    return shuffled;
}

int shuffledMultilayerDependenciesDecode()
{
    Pcm pcm = makeCycleMaterials();
    data::SelaFrame shuffled = makeShuffledMultilayerFrame(pcm);
    TASK_CHECK(shuffled.subFrames.size() == 4);
    TASK_CHECK(shuffled.subFrames[0].channel == 3);
    TASK_CHECK(shuffled.subFrames[1].channel == 0);
    TASK_CHECK(shuffled.subFrames[2].channel == 2);
    TASK_CHECK(shuffled.subFrames[3].channel == 1);

    data::WavFrame directDecoded = frame::FrameDecoder(shuffled).process();
    CHECK_PCM(pcm, directDecoded.samples);

    const std::string selaPath = ".task-build/channel_forest_shuffled.sela";
    writeOneSelaFrame(selaPath, shuffled, 4);

    file::SelaFile loaded;
    {
        std::ifstream input(selaPath.c_str(), std::ios::binary);
        TASK_CHECK(input.good());
        loaded.readFromFile(input);
    }
    TASK_CHECK(loaded.selaFrames.size() == 1);
    TASK_CHECK(loaded.selaFrames[0].subFrames[0].channel == 3);
    data::WavFrame fileFrameDecoded = frame::FrameDecoder(loaded.selaFrames[0]).process();
    CHECK_PCM(pcm, fileFrameDecoded.samples);

    std::ifstream selaInput(selaPath.c_str(), std::ios::binary);
    sela::Decoder decoder(selaInput);
    file::WavFile decodedWav = decoder.process();
    TASK_CHECK(decodedWav.wavChunk.dataSubChunk.wavFrames.size() == 1);
    CHECK_PCM(pcm, decodedWav.wavChunk.dataSubChunk.wavFrames[0].samples);
    return 0;
}

bool decoderRejects(const data::SelaFrame& frame)
{
    try {
        frame::FrameDecoder decoder(frame);
        data::WavFrame output = decoder.process();
        (void)output;
        return false;
    } catch (const data::Exception&) {
        return true;
    } catch (...) {
        return false;
    }
}

int illegalDependencyStructuresThrow()
{
    Pcm pcm = makeCycleMaterials();
    data::SelaFrame valid = makeLegalMultilayerFrame(pcm);
    TASK_CHECK(!decoderRejects(valid));

    {
        data::SelaFrame bad = valid;
        bad.subFrames[0].channel = 201;
        TASK_CHECK(decoderRejects(bad));
    }
    {
        data::SelaFrame bad = valid;
        bad.subFrames[2].channel = bad.subFrames[0].channel; // duplicate 0, missing 2
        TASK_CHECK(decoderRejects(bad));
    }
    {
        data::SelaFrame bad = valid;
        bad.subFrames[1].subFrameType = 9;
        TASK_CHECK(decoderRejects(bad));
    }
    {
        data::SelaFrame bad = valid;
        bad.subFrames[0].parentChannelNumber = 9;
        TASK_CHECK(decoderRejects(bad));
    }
    {
        data::SelaFrame bad = valid;
        bad.subFrames[1].subFrameType = 1;
        bad.subFrames[1].parentChannelNumber = 1;
        TASK_CHECK(decoderRejects(bad));
    }
    {
        data::SelaFrame bad = valid;
        ++bad.subFrames[0].samplesPerChannel;
        TASK_CHECK(decoderRejects(bad));
    }
    {
        data::SelaFrame bad = valid;
        bad.subFrames[0].subFrameType = 1;
        bad.subFrames[0].parentChannelNumber = 1;
        bad.subFrames[1].subFrameType = 1;
        bad.subFrames[1].parentChannelNumber = 0;
        TASK_CHECK(decoderRejects(bad));
    }
    {
        data::SelaFrame bad = valid;
        bad.subFrames[0].subFrameType = 1;
        bad.subFrames[0].parentChannelNumber = 1;
        bad.subFrames[1].subFrameType = 1;
        bad.subFrames[1].parentChannelNumber = 2;
        bad.subFrames[2].subFrameType = 1;
        bad.subFrames[2].parentChannelNumber = 0;
        TASK_CHECK(decoderRejects(bad));
    }

    return 0;
}

int fileEntryReturnsBadDependencyError()
{
    Pcm pcm = makeCycleMaterials();
    data::SelaFrame cycle = makeLegalMultilayerFrame(pcm);
    cycle.subFrames[0].subFrameType = 1;
    cycle.subFrames[0].parentChannelNumber = 1;
    cycle.subFrames[1].subFrameType = 1;
    cycle.subFrames[1].parentChannelNumber = 0;

    const std::string path = ".task-build/channel_forest_bad_dependency.sela";
    writeOneSelaFrame(path, cycle, static_cast<uint8_t>(pcm.size()));

    bool caughtExpectedException = false;
    try {
        std::ifstream input(path.c_str(), std::ios::binary);
        sela::Decoder decoder(input);
        file::WavFile output = decoder.process();
        (void)output;
    } catch (const data::Exception&) {
        caughtExpectedException = true;
    } catch (...) {
        caughtExpectedException = false;
    }
    TASK_CHECK(caughtExpectedException);

    // The process must still be alive to run this follow-up decode.
    data::WavFrame stillGood = frame::FrameDecoder(makeShuffledMultilayerFrame(pcm)).process();
    CHECK_PCM(pcm, stillGood.samples);
    return 0;
}

int legacyBytesAndOldLayoutsRemainReadable()
{
    const size_t channels = 3;
    Pcm pcm(channels, std::vector<int32_t>(SAMPLES));
    for (size_t i = 0; i < SAMPLES; ++i) {
        const int32_t sample = static_cast<int32_t>(
            12000.0 * std::sin(static_cast<double>(i) * 0.073)
            + 5000.0 * std::cos(static_cast<double>(i) * 0.021));
        for (size_t c = 0; c < channels; ++c) {
            pcm[c][i] = sample;
        }
    }

    data::WavFrame wavFrame(16, pcm);
    data::SelaFrame defaultEncoded = frame::FrameEncoder(wavFrame).process();
    data::SelaFrame explicitFalseEncoded = frame::FrameEncoder(wavFrame, false).process();
    TASK_CHECK(defaultEncoded.subFrames.size() == channels);

    size_t roots = 0;
    uint64_t payloadBytes = 0;
    for (size_t c = 0; c < channels; ++c) {
        TASK_CHECK(defaultEncoded.subFrames[c].channel == c);
        TASK_CHECK(defaultEncoded.subFrames[c].subFrameType == 0);
        TASK_CHECK(defaultEncoded.subFrames[c].parentChannelNumber == c);
        TASK_CHECK(sameSubFramePayload(defaultEncoded.subFrames[c],
            explicitFalseEncoded.subFrames[c]));
        roots += defaultEncoded.subFrames[c].subFrameType == 0;
        payloadBytes += subFrameCost(defaultEncoded.subFrames[c]);
    }
    TASK_CHECK(roots == 3);
    TASK_CHECK(payloadBytes == 6000);

    const std::string legacyPath = ".task-build/channel_forest_legacy.sela";
    const std::string falsePath = ".task-build/channel_forest_explicit_false.sela";
    writeOneSelaFrame(legacyPath, defaultEncoded, 3);
    writeOneSelaFrame(falsePath, explicitFalseEncoded, 3);
    TASK_CHECK(fingerprintFile(legacyPath) == fingerprintFile(falsePath));

    uint64_t expectedFingerprint = 0;
    {
        std::ifstream reference("task_support/legacy-fnv.txt");
        TASK_CHECK(reference.good());
        TASK_CHECK(reference >> expectedFingerprint);
    }
    TASK_CHECK(fingerprintFile(legacyPath) == expectedFingerprint);

    data::WavFrame decoded = frame::FrameDecoder(defaultEncoded).process();
    CHECK_PCM(pcm, decoded.samples);

    // The old two-channel path writes one independent and one differential
    // subframe.  The new decoder must keep reading that layout.
    Pcm stereo(2, std::vector<int32_t>(SAMPLES));
    for (size_t i = 0; i < SAMPLES; ++i) {
        const int32_t sample = static_cast<int32_t>(
            12000.0 * std::sin(static_cast<double>(i) * 0.073)
            + 5000.0 * std::cos(static_cast<double>(i) * 0.021));
        stereo[0][i] = sample;
        stereo[1][i] = sample;
    }
    data::SelaFrame stereoEncoded = frame::FrameEncoder(data::WavFrame(16, stereo)).process();
    TASK_CHECK(stereoEncoded.subFrames.size() == 2);
    TASK_CHECK(stereoEncoded.subFrames[0].subFrameType == 0);
    TASK_CHECK(stereoEncoded.subFrames[1].subFrameType == 1);
    TASK_CHECK(stereoEncoded.subFrames[1].parentChannelNumber == 0);
    CHECK_PCM(stereo, frame::FrameDecoder(stereoEncoded).process().samples);

    const std::string stereoPath = ".task-build/channel_forest_legacy_stereo.sela";
    writeOneSelaFrame(stereoPath, stereoEncoded, 2);
    std::ifstream stereoFileInput(stereoPath.c_str(), std::ios::binary);
    sela::Decoder stereoFileDecoder(stereoFileInput);
    file::WavFile stereoDecodedWav = stereoFileDecoder.process();
    TASK_CHECK(stereoDecodedWav.wavChunk.dataSubChunk.wavFrames.size() == 1);
    CHECK_PCM(stereo, stereoDecodedWav.wavChunk.dataSubChunk.wavFrames[0].samples);
    return 0;
}

int fileEntryPassesForestSettingToWorkers()
{
    Pcm pcm = makeCycleMaterials();
    file::WavFile decodedWav = encodeDecodeWavThroughFiles(
        ".task-build/channel_forest_multi.wav",
        ".task-build/channel_forest_multi.sela", pcm, 3, true);

    TASK_CHECK(decodedWav.wavChunk.formatSubChunk.numChannels == 4);
    TASK_CHECK(decodedWav.wavChunk.dataSubChunk.wavFrames.size() == 3);
    for (size_t frame = 0; frame < 3; ++frame) {
        CHECK_PCM(pcm, decodedWav.wavChunk.dataSubChunk.wavFrames[frame].samples);
    }
    return 0;
}

int channelLimitsOneAndEight()
{
    for (size_t channelCount = 1; channelCount <= 8; channelCount += 7) {
        Pcm pcm(channelCount, std::vector<int32_t>(SAMPLES));
        for (size_t c = 0; c < channelCount; ++c) {
            for (size_t i = 0; i < SAMPLES; ++i) {
                const double t = static_cast<double>(i);
                pcm[c][i] = static_cast<int32_t>(
                    9000.0 * std::sin(t * (0.021 + 0.006 * static_cast<double>(c)))
                    + 3000.0 * std::cos(t * (0.013 + 0.004 * static_cast<double>(c)))
                    + 1000.0 * std::sin(t * 0.17 + static_cast<double>(c)));
            }
        }

        data::SelaFrame encoded = frame::FrameEncoder(data::WavFrame(16, pcm), true).process();
        TASK_CHECK(encoded.subFrames.size() == channelCount);
        std::vector<int> parent(channelCount, -1);
        for (size_t c = 0; c < channelCount; ++c) {
            TASK_CHECK(encoded.subFrames[c].channel == c);
            if (encoded.subFrames[c].subFrameType == 1) {
                parent[c] = encoded.subFrames[c].parentChannelNumber;
            }
        }
        TASK_CHECK(frame::isAcyclicChannelForest(parent, channelCount));
        CHECK_PCM(pcm, frame::FrameDecoder(encoded).process().samples);
    }
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    { "optimal", "selector_matches_independent_exhaustive", selectorMatchesExhaustiveCostMatrices },
    { "optimal", "audio_candidates_match_independent_exhaustive", audioCandidatesMatchIndependentExhaustive },
    { "optimal", "cycle_related_and_anti_related_materials", cycleRelatedAndAntiRelatedMaterials },
    { "dependencies", "shuffled_multilayer_dependencies_decode", shuffledMultilayerDependenciesDecode },
    { "dependencies", "illegal_dependency_structures_throw", illegalDependencyStructuresThrow },
    { "dependencies", "file_entry_returns_bad_dependency_error", fileEntryReturnsBadDependencyError },
    { "compatibility", "legacy_bytes_and_old_layouts_remain_readable", legacyBytesAndOldLayoutsRemainReadable },
    { "compatibility", "file_entry_passes_forest_setting_to_workers", fileEntryPassesForestSettingToWorkers },
    { "compatibility", "channel_limits_one_and_eight", channelLimitsOneAndEight },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
