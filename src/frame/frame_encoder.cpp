#include "../include/frame.hpp"
#include "../include/channel_forest.hpp"
#include "../include/data/exception.hpp"
#include "../include/lpc.hpp"
#include "../include/rice.hpp"

#include <utility>

namespace frame {
FrameEncoder::FrameEncoder(const data::WavFrame& wavFrame, bool channelForest)
    : wavFrame(wavFrame)
    , channelForest(channelForest)
{
}

data::SelaFrame FrameEncoder::process()
{
    return channelForest ? processChannelForest() : processLegacy();
}

data::SelaFrame FrameEncoder::processLegacy()
{
    data::SelaFrame selaFrame = data::SelaFrame(wavFrame.bitsPerSample);
    selaFrame.subFrames.reserve(wavFrame.samples.size());

    // Foreach channel
    for (size_t i = 0; i < wavFrame.samples.size(); i++) {
        if (i == 1 && ((i + 1) == wavFrame.samples.size())) {
            //-------------------------------------Stage 1 - Generate difference signal----------------------------------------------------
            std::vector<int32_t> differenceSignal;
            differenceSignal.reserve(wavFrame.samples[i].size());
            for (size_t j = 0; j < wavFrame.samples[i].size(); j++) {
                differenceSignal.push_back(wavFrame.samples[i - 1][j] - wavFrame.samples[i][j]);
            }
            //-----------------------------------------------------------------------------------------------------------------------------

            //----------- Stage 2 - Generate residues and reflection coefficients for difference as well as actual signal------------------
            data::LpcDecodedData residueGeneratorDifferenceData = data::LpcDecodedData(wavFrame.bitsPerSample, std::move(differenceSignal));
            lpc::ResidueGenerator residueGeneratorDifference = lpc::ResidueGenerator(residueGeneratorDifferenceData);
            data::LpcEncodedData residuesDifference = residueGeneratorDifference.process();

            data::LpcDecodedData residueGeneratorActualData = data::LpcDecodedData(wavFrame.bitsPerSample, std::move(wavFrame.samples[i]));
            lpc::ResidueGenerator residueGeneratorActual = lpc::ResidueGenerator(residueGeneratorActualData);
            data::LpcEncodedData residuesActual = residueGeneratorActual.process();
            //-----------------------------------------------------------------------------------------------------------------------------

            //-----------------Stage 3 - Compress residues and reflection coefficients for difference and actual signal--------------------
            //Assign data
            data::RiceDecodedData reflectionRiceEncoderDifferenceData = data::RiceDecodedData(std::move(residuesDifference.quantizedReflectionCoefficients));
            data::RiceDecodedData residueRiceEncoderDifferenceData = data::RiceDecodedData(std::move(residuesDifference.residues));
            
            //Create Encoders
            rice::RiceEncoder reflectionRiceEncoderDifference = rice::RiceEncoder(reflectionRiceEncoderDifferenceData);
            rice::RiceEncoder residueRiceEncoderDifference = rice::RiceEncoder(residueRiceEncoderDifferenceData);

            //Process data in encoders
            data::RiceEncodedData reflectionDataDifference = reflectionRiceEncoderDifference.process();
            data::RiceEncodedData residueDataDifference = residueRiceEncoderDifference.process();

            //Assign data
            data::RiceDecodedData reflectionRiceEncoderActualData = data::RiceDecodedData(std::move(residuesActual.quantizedReflectionCoefficients));
            data::RiceDecodedData residueRiceEncoderActualData = data::RiceDecodedData(std::move(residuesActual.residues));
            
            //Create Encoders
            rice::RiceEncoder reflectionRiceEncoderActual = rice::RiceEncoder(reflectionRiceEncoderActualData);
            rice::RiceEncoder residueRiceEncoderActual = rice::RiceEncoder(residueRiceEncoderActualData);

            //Process data in encoders
            data::RiceEncodedData reflectionDataActual = reflectionRiceEncoderActual.process();
            data::RiceEncodedData residueDataActual = residueRiceEncoderActual.process();
            //-----------------------------------------------------------------------------------------------------------------------------

            // Stage 4 - Compare sizes of both types and generate subFrame
            size_t differenceDataSize = reflectionDataDifference.encodedData.size() + residueDataDifference.encodedData.size();
            size_t actualDataSize = reflectionDataActual.encodedData.size() + residueDataActual.encodedData.size();
            if (differenceDataSize < actualDataSize) {
                data::SelaSubFrame selaSubFrame = data::SelaSubFrame((uint8_t)i, (uint8_t)1, (uint8_t)(i - 1), reflectionDataDifference, residueDataDifference);
                selaFrame.subFrames.push_back(selaSubFrame);
            } else {
                data::SelaSubFrame selaSubFrame = data::SelaSubFrame((uint8_t)i, (uint8_t)0, (uint8_t)i, reflectionDataActual, residueDataActual);
                selaFrame.subFrames.push_back(selaSubFrame);
            }
        } else {
            //---------------------Stage 1 - Generate residues and reflection coefficients----------------------
            data::LpcDecodedData samples = data::LpcDecodedData(wavFrame.bitsPerSample, std::move(wavFrame.samples[i]));
            lpc::ResidueGenerator residueGenerator = lpc::ResidueGenerator(samples);
            data::LpcEncodedData residues = residueGenerator.process();
            //--------------------------------------------------------------------------------------------------

            //---------------------Stage 2 - Compress residues and reflection coefficients----------------------
            //Assign data
            data::RiceDecodedData reflectionRiceInputData = data::RiceDecodedData(std::move(residues.quantizedReflectionCoefficients));
            data::RiceDecodedData residueRiceInputData = data::RiceDecodedData(std::move(residues.residues));
            
            //Create Encoders
            rice::RiceEncoder reflectionRiceEncoder = rice::RiceEncoder(reflectionRiceInputData);
            rice::RiceEncoder residueRiceEncoder = rice::RiceEncoder(residueRiceInputData);

            //Process data in encoders
            data::RiceEncodedData reflectionRiceOutputData = reflectionRiceEncoder.process();
            data::RiceEncodedData residueRiceOutputData = residueRiceEncoder.process();
            //-------------------------------------------------------------------------------------------------

            //-----------------------------------Stage 3 - Generate Subframes----------------------------------
            data::SelaSubFrame selaSubFrame = data::SelaSubFrame((uint8_t)i, (uint8_t)0, (uint8_t)i, reflectionRiceOutputData, residueRiceOutputData);
            selaFrame.subFrames.push_back(selaSubFrame);
            //-------------------------------------------------------------------------------------------------
        }
    }

    return selaFrame;
}

namespace {
struct EncodedSignal {
    data::RiceEncodedData reflection;
    data::RiceEncodedData residue;

    EncodedSignal(data::RiceEncodedData&& reflection, data::RiceEncodedData&& residue)
        : reflection(std::move(reflection))
        , residue(std::move(residue))
    {
    }
};

EncodedSignal encodeChannelForestSignal(uint8_t bitsPerSample, std::vector<int32_t> signal)
{
    data::LpcDecodedData lpcInput(bitsPerSample, std::move(signal));
    lpc::ResidueGenerator residueGenerator(lpcInput);
    data::LpcEncodedData lpcOutput = residueGenerator.process();

    data::RiceDecodedData reflectionInput(std::move(lpcOutput.quantizedReflectionCoefficients));
    rice::RiceEncoder reflectionEncoder(reflectionInput);
    data::RiceEncodedData reflectionOutput = reflectionEncoder.process();

    data::RiceDecodedData residueInput(std::move(lpcOutput.residues));
    rice::RiceEncoder residueEncoder(residueInput);
    data::RiceEncodedData residueOutput = residueEncoder.process();

    return EncodedSignal(std::move(reflectionOutput), std::move(residueOutput));
}

uint64_t channelForestCandidateCost(const data::RiceEncodedData& reflection,
    const data::RiceEncodedData& residue)
{
    return 12 + 4 * (static_cast<uint64_t>(reflection.encodedData.size())
                         + static_cast<uint64_t>(residue.encodedData.size()));
}
}

data::SelaFrame FrameEncoder::processChannelForest()
{
    const size_t channelCount = wavFrame.samples.size();
    if (channelCount < 1 || channelCount > 8) {
        throw data::Exception("Channel forest mode supports 1 to 8 channels.");
    }
    if (wavFrame.bitsPerSample != 16) {
        throw data::Exception("Channel forest mode supports signed 16-bit PCM only.");
    }
    for (size_t channel = 0; channel < channelCount; ++channel) {
        if (wavFrame.samples[channel].size() != 2048) {
            throw data::Exception("Channel forest mode requires 2048 samples per channel and frame.");
        }
    }

    std::vector<uint64_t> rootCosts(channelCount, 0);
    std::vector<std::vector<uint64_t> > parentCosts(
        channelCount, std::vector<uint64_t>(channelCount, 0));
    std::vector<size_t> rootCandidateIndex(channelCount, 0);
    std::vector<std::vector<size_t> > parentCandidateIndex(
        channelCount, std::vector<size_t>(channelCount, 0));

    // There is one independent candidate and channelCount - 1 dependent
    // candidates for every child.
    std::vector<data::SelaSubFrame> candidates;
    candidates.reserve(channelCount * channelCount);

    for (size_t child = 0; child < channelCount; ++child) {
        EncodedSignal independent = encodeChannelForestSignal(
            wavFrame.bitsPerSample, wavFrame.samples[child]);
        rootCosts[child] = channelForestCandidateCost(independent.reflection, independent.residue);
        rootCandidateIndex[child] = candidates.size();
        candidates.emplace_back(static_cast<uint8_t>(child), 0, static_cast<uint8_t>(child),
            independent.reflection, independent.residue);

        for (size_t parent = 0; parent < channelCount; ++parent) {
            if (parent == child) {
                continue;
            }

            std::vector<int32_t> differenceSignal;
            differenceSignal.reserve(wavFrame.samples[child].size());
            for (size_t sample = 0; sample < wavFrame.samples[child].size(); ++sample) {
                // Differential signal is always parent - child.
                differenceSignal.push_back(
                    wavFrame.samples[parent][sample] - wavFrame.samples[child][sample]);
            }

            EncodedSignal dependent = encodeChannelForestSignal(
                wavFrame.bitsPerSample, std::move(differenceSignal));
            parentCosts[child][parent] = channelForestCandidateCost(
                dependent.reflection, dependent.residue);
            parentCandidateIndex[child][parent] = candidates.size();
            candidates.emplace_back(static_cast<uint8_t>(child), 1,
                static_cast<uint8_t>(parent), dependent.reflection, dependent.residue);
        }
    }

    ChannelForestSelection selection = selectOptimalChannelForest(rootCosts, parentCosts);

    data::SelaFrame selaFrame = data::SelaFrame(wavFrame.bitsPerSample);
    selaFrame.subFrames.reserve(channelCount);
    for (size_t child = 0; child < channelCount; ++child) {
        const int parent = selection.parent[child];
        const size_t candidateIndex = parent < 0
            ? rootCandidateIndex[child]
            : parentCandidateIndex[child][static_cast<size_t>(parent)];
        selaFrame.subFrames.push_back(candidates[candidateIndex]);
    }

    return selaFrame;
}

}