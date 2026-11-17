#include "../include/frame.hpp"
#include "../include/lpc.hpp"
#include "../include/rice.hpp"
#include "../include/data/exception.hpp"

#include <vector>

namespace frame {
namespace {
void fail(const std::string& message)
{
    std::string exceptionMessage = message;
    throw data::Exception(std::move(exceptionMessage));
}

data::LpcDecodedData decodeSubFrameSignal(uint8_t bitsPerSample, const data::SelaSubFrame& subFrame)
{
    // Stage 1 - Extract data from subFrame
    const data::RiceEncodedData reflectionData = data::RiceEncodedData(subFrame.reflectionCoefficientRiceParam,
        subFrame.optimumLpcOrder, std::move(subFrame.encodedReflectionCoefficients));
    const data::RiceEncodedData residueData = data::RiceEncodedData(subFrame.residueRiceParam,
        subFrame.samplesPerChannel, std::move(subFrame.encodedResidues));

    // Stage 2 - Decompress data
    data::RiceDecodedData decodedReflectionData = rice::RiceDecoder(reflectionData).process();
    data::RiceDecodedData decodedResidueData = rice::RiceDecoder(residueData).process();

    // Stage 3 - Reconstruct the encoded signal
    const data::LpcEncodedData encodedData = data::LpcEncodedData(subFrame.optimumLpcOrder, bitsPerSample,
        std::move(decodedReflectionData.decodedData), std::move(decodedResidueData.decodedData));
    return lpc::SampleGenerator(encodedData).process();
}
} // namespace

FrameDecoder::FrameDecoder(const data::SelaFrame& selaFrame)
    : selaFrame(selaFrame)
{
}

void FrameDecoder::validateSubFrames(size_t channelCount) const
{
    const std::vector<data::SelaSubFrame>& subFrames = selaFrame.subFrames;
    if (subFrames.size() != channelCount) {
        fail("Frame does not contain one subframe per channel.");
    }

    // Channel numbers must cover 0 .. N-1 exactly once.
    std::vector<bool> seen(channelCount, false);
    for (const data::SelaSubFrame& subFrame : subFrames) {
        if (subFrame.channel >= channelCount) {
            fail("Subframe channel number is out of range.");
        }
        if (seen[subFrame.channel]) {
            fail("Duplicate subframe channel number.");
        }
        seen[subFrame.channel] = true;
        if (subFrame.subFrameType != 0 && subFrame.subFrameType != 1) {
            fail("Subframe type is neither independent nor dependent.");
        }
    }

    // Index subframes by channel so storage order cannot matter below.
    std::vector<int> indexByChannel(channelCount, -1);
    for (size_t i = 0; i < subFrames.size(); i++) {
        indexByChannel[subFrames[i].channel] = (int)i;
    }

    for (const data::SelaSubFrame& subFrame : subFrames) {
        if (subFrame.subFrameType == 0) {
            continue;
        }
        // Dependent subframe: parent must exist and may not be the channel
        // itself.
        if (subFrame.parentChannelNumber >= channelCount) {
            fail("Dependent subframe refers to a missing parent channel.");
        }
        if (subFrame.parentChannelNumber == subFrame.channel) {
            fail("Dependent subframe cannot depend on itself.");
        }
        const data::SelaSubFrame& parent = subFrames[indexByChannel[subFrame.parentChannelNumber]];
        if (parent.samplesPerChannel != subFrame.samplesPerChannel) {
            fail("Dependent subframe and its parent must carry the same sample count.");
        }
    }

    // Walk parent links from every channel; revisiting a channel still open
    // means a dependency cycle, reaching an independent root is sound.
    std::vector<int> state(channelCount, 0); // 0 = untouched, 1 = open, 2 = done
    for (size_t start = 0; start < channelCount; start++) {
        size_t node = start;
        std::vector<size_t> chain;
        while (state[node] == 0) {
            state[node] = 1;
            chain.push_back(node);
            const data::SelaSubFrame& subFrame = subFrames[indexByChannel[node]];
            if (subFrame.subFrameType == 0) {
                break;
            }
            node = subFrame.parentChannelNumber;
        }
        bool hitCycle = state[node] == 1 && subFrames[indexByChannel[node]].subFrameType == 1;
        for (size_t channel : chain) {
            state[channel] = 2;
        }
        if (hitCycle) {
            fail("Subframe dependency graph contains a cycle.");
        }
    }
}

data::WavFrame FrameDecoder::process()
{
    const size_t channelCount = selaFrame.subFrames.size();
    validateSubFrames(channelCount);

    std::vector<int> indexByChannel(channelCount, -1);
    for (size_t i = 0; i < channelCount; i++) {
        indexByChannel[selaFrame.subFrames[i].channel] = (int)i;
    }

    std::vector<std::vector<int32_t> > allSamples(channelCount, std::vector<int32_t>());
    std::vector<bool> decoded(channelCount, false);

    // Restore each channel on demand. Parent chains are decoded recursively,
    // so multi-level forests work no matter how the subframes are shuffled.
    struct Restorer {
        const data::SelaFrame& frame;
        const std::vector<int>& indexByChannel;
        std::vector<std::vector<int32_t> >& allSamples;
        std::vector<bool>& decoded;

        void restore(size_t channel)
        {
            if (decoded[channel]) {
                return;
            }
            const data::SelaSubFrame& subFrame = frame.subFrames[indexByChannel[channel]];
            data::LpcDecodedData signal = decodeSubFrameSignal(frame.bitsPerSample, subFrame);

            if (subFrame.subFrameType == 0) {
                allSamples[channel] = std::move(signal.samples);
            } else {
                size_t parent = subFrame.parentChannelNumber;
                restore(parent);
                const std::vector<int32_t>& parentSamples = allSamples[parent];
                std::vector<int32_t> restored;
                restored.reserve(signal.samples.size());
                // Encoded difference is parent - child, so child = parent - difference.
                for (size_t i = 0; i < signal.samples.size(); i++) {
                    restored.push_back(parentSamples[i] - signal.samples[i]);
                }
                allSamples[channel] = std::move(restored);
            }
            decoded[channel] = true;
        }
    };

    Restorer restorer = { selaFrame, indexByChannel, allSamples, decoded };
    for (size_t channel = 0; channel < channelCount; channel++) {
        restorer.restore(channel);
    }

    return data::WavFrame(selaFrame.bitsPerSample, std::move(allSamples));
}
}
