#include "../include/frame.hpp"
#include "../include/channel_forest.hpp"
#include "../include/data/exception.hpp"
#include "../include/lpc.hpp"
#include "../include/rice.hpp"

#include <functional>

namespace frame {
FrameDecoder::FrameDecoder(const data::SelaFrame& selaFrame)
    : selaFrame(selaFrame)
{
}

data::WavFrame FrameDecoder::process()
{
    const size_t channelCount = selaFrame.subFrames.size();

    // Validation phase: establish channel identity and structural legality
    // before any LPC/Rice restoration is attempted.
    std::vector<const data::SelaSubFrame*> subFrameByChannel(channelCount, 0);
    for (size_t storageIndex = 0; storageIndex < channelCount; ++storageIndex) {
        const data::SelaSubFrame& subFrame = selaFrame.subFrames[storageIndex];
        const size_t channel = static_cast<size_t>(subFrame.channel);

        if (channel >= channelCount) {
            throw data::Exception("Subframe channel number is outside 0 to N-1.");
        }
        if (subFrameByChannel[channel] != 0) {
            throw data::Exception("Subframe channel numbers must be unique.");
        }
        subFrameByChannel[channel] = &subFrame;
    }

    for (size_t channel = 0; channel < channelCount; ++channel) {
        if (subFrameByChannel[channel] == 0) {
            throw data::Exception("Subframe channel numbers must cover 0 to N-1.");
        }
    }

    std::vector<int> parent(channelCount, -1);
    for (size_t channel = 0; channel < channelCount; ++channel) {
        const data::SelaSubFrame& subFrame = *subFrameByChannel[channel];

        if (subFrame.subFrameType != 0 && subFrame.subFrameType != 1) {
            throw data::Exception("Subframe type must be 0 (independent) or 1 (dependent).");
        }

        if (subFrame.subFrameType == 1) {
            const size_t parentChannel = static_cast<size_t>(subFrame.parentChannelNumber);
            if (parentChannel >= channelCount || subFrameByChannel[parentChannel] == 0) {
                throw data::Exception("Dependent subframe parent does not exist.");
            }
            if (parentChannel == channel) {
                throw data::Exception("A subframe cannot depend on itself.");
            }
            if (subFrame.samplesPerChannel != subFrameByChannel[parentChannel]->samplesPerChannel) {
                throw data::Exception("Parent and child subframes must have the same sample count.");
            }
            parent[channel] = static_cast<int>(parentChannel);
        }
    }

    if (!isAcyclicChannelForest(parent, channelCount)) {
        throw data::Exception("Channel dependency graph must not contain a cycle.");
    }

    std::vector<std::vector<int32_t> > allSamples(channelCount, std::vector<int32_t>());
    std::vector<char> restored(channelCount, 0);

    auto decodeEncodedSignal = [&](const data::SelaSubFrame& subFrame) -> std::vector<int32_t> {
        // Stage 1 - Extract data from subFrame
        uint8_t optimumLpcOrder = subFrame.optimumLpcOrder;
        const data::RiceEncodedData reflectionData = data::RiceEncodedData(
            subFrame.reflectionCoefficientRiceParam, subFrame.optimumLpcOrder,
            std::move(subFrame.encodedReflectionCoefficients));
        const data::RiceEncodedData residueData = data::RiceEncodedData(
            subFrame.residueRiceParam, subFrame.samplesPerChannel,
            std::move(subFrame.encodedResidues));

        // Stage 2 - Decompress data
        data::RiceDecodedData decodedReflectionData = rice::RiceDecoder(reflectionData).process();
        data::RiceDecodedData decodedResidueData = rice::RiceDecoder(residueData).process();

        // Stage 3 - Generate signal samples
        const data::LpcEncodedData encodedData = data::LpcEncodedData(optimumLpcOrder,
            selaFrame.bitsPerSample, std::move(decodedReflectionData.decodedData),
            std::move(decodedResidueData.decodedData));
        return lpc::SampleGenerator(encodedData).process().samples;
    };

    // Restore on demand along parent links.  The validated acyclic graph makes
    // this terminate for any number of dependency layers and any storage order.
    std::function<void(size_t)> restoreChannel = [&](size_t channel) {
        if (restored[channel]) {
            return;
        }

        const data::SelaSubFrame& subFrame = *subFrameByChannel[channel];
        std::vector<int32_t> decodedSignal = decodeEncodedSignal(subFrame);

        if (subFrame.subFrameType == 0) {
            allSamples[channel] = std::move(decodedSignal);
            restored[channel] = 1;
            return;
        }

        const size_t parentChannel = static_cast<size_t>(subFrame.parentChannelNumber);
        restoreChannel(parentChannel);

        const std::vector<int32_t>& parentSamples = allSamples[parentChannel];
        if (decodedSignal.size() != parentSamples.size()) {
            throw data::Exception("Parent and child decoded sample counts do not match.");
        }

        // Differential signal is parent - child, so child = parent - difference.
        std::vector<int32_t> samples;
        samples.reserve(decodedSignal.size());
        for (size_t sample = 0; sample < decodedSignal.size(); ++sample) {
            samples.push_back(parentSamples[sample] - decodedSignal[sample]);
        }

        allSamples[channel] = std::move(samples);
        restored[channel] = 1;
    };

    for (size_t channel = 0; channel < channelCount; ++channel) {
        restoreChannel(channel);
    }

    return data::WavFrame(selaFrame.bitsPerSample, std::move(allSamples));
}
}
