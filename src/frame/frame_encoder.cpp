#include "../include/frame.hpp"
#include "../include/frame/channel_forest.hpp"
#include "../include/lpc.hpp"
#include "../include/rice.hpp"
#include "../include/data/exception.hpp"

namespace frame {
namespace {
// One fully encoded candidate: the reflection and residue payloads are the
// real LPC + Rice encoder outputs, never a simplified stand-in.
struct EncodedCandidate {
    data::RiceEncodedData reflectionData;
    data::RiceEncodedData residueData;
    size_t byteCost;
};

EncodedCandidate encodeSignal(uint8_t bitsPerSample, std::vector<int32_t> signal)
{
    // Stage 1 - Generate residues and reflection coefficients
    data::LpcDecodedData residueGeneratorInput = data::LpcDecodedData(bitsPerSample, std::move(signal));
    lpc::ResidueGenerator residueGenerator = lpc::ResidueGenerator(residueGeneratorInput);
    data::LpcEncodedData residues = residueGenerator.process();

    // Stage 2 - Compress residues and reflection coefficients with Rice
    data::RiceDecodedData reflectionRiceInput = data::RiceDecodedData(std::move(residues.quantizedReflectionCoefficients));
    data::RiceDecodedData residueRiceInput = data::RiceDecodedData(std::move(residues.residues));
    data::RiceEncodedData reflectionData = rice::RiceEncoder(reflectionRiceInput).process();
    data::RiceEncodedData residueData = rice::RiceEncoder(residueRiceInput).process();

    // 12 fixed header bytes plus the two 32-bit payload arrays.
    size_t byteCost = 12 + 4 * (reflectionData.encodedData.size() + residueData.encodedData.size());
    return EncodedCandidate{ std::move(reflectionData), std::move(residueData), byteCost };
}
} // namespace

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

data::SelaFrame FrameEncoder::processChannelForest()
{
    const size_t channelCount = wavFrame.samples.size();
    if (channelCount < 1 || channelCount > 8) {
        const std::string exceptionMessage = "Channel forest supports 1 to 8 channels per frame.";
        throw data::Exception(std::move(exceptionMessage));
    }
    if (wavFrame.bitsPerSample != 16) {
        const std::string exceptionMessage = "Channel forest supports only 16 bit signed PCM.";
        throw data::Exception(std::move(exceptionMessage));
    }
    for (size_t i = 0; i < channelCount; i++) {
        if (wavFrame.samples[i].size() != 2048) {
            const std::string exceptionMessage = "Channel forest frames must carry 2048 samples per channel.";
            throw data::Exception(std::move(exceptionMessage));
        }
    }

    // candidates[child][parent]: parent < channelCount is a difference
    // candidate against that parent (difference = parent - child), the last
    // slot is the independent root candidate.
    const size_t root = channelCount;
    std::vector<std::vector<EncodedCandidate> > candidates(channelCount, std::vector<EncodedCandidate>());
    std::vector<std::vector<size_t> > costs(channelCount, std::vector<size_t>(root + 1, 0));

    for (size_t child = 0; child < channelCount; child++) {
        candidates[child].reserve(root + 1);
        for (size_t parent = 0; parent <= root; parent++) {
            if (parent == child) {
                // A channel cannot be its own parent; the slot is never
                // selected, so no candidate payload is produced.
                costs[child][parent] = (size_t)-1;
                data::RiceEncodedData emptyReflection(0, 0, std::vector<uint32_t>());
                data::RiceEncodedData emptyResidue(0, 0, std::vector<uint32_t>());
                candidates[child].push_back(EncodedCandidate{ std::move(emptyReflection), std::move(emptyResidue), (size_t)-1 });
                continue;
            }

            std::vector<int32_t> signal;
            signal.reserve(wavFrame.samples[child].size());
            if (parent == root) {
                signal = wavFrame.samples[child];
            } else {
                // Difference coding keeps the established parent - child sign.
                for (size_t j = 0; j < wavFrame.samples[child].size(); j++) {
                    signal.push_back(wavFrame.samples[parent][j] - wavFrame.samples[child][j]);
                }
            }
            EncodedCandidate candidate = encodeSignal(wavFrame.bitsPerSample, std::move(signal));
            costs[child][parent] = candidate.byteCost;
            candidates[child].push_back(std::move(candidate));
        }
    }

    // Globally minimum acyclic forest, with the specified tie-break rule.
    std::vector<uint8_t> selectedParent = selectChannelForest(costs);

    // Subframes are emitted in channel order; payloads come straight from
    // the winning candidate objects.
    data::SelaFrame selaFrame = data::SelaFrame(wavFrame.bitsPerSample);
    selaFrame.subFrames.reserve(channelCount);
    for (size_t child = 0; child < channelCount; child++) {
        uint8_t parent = selectedParent[child];
        const EncodedCandidate& candidate = candidates[child][parent];
        if (parent == root) {
            // Independent root: parent field keeps its legacy meaning.
            data::SelaSubFrame selaSubFrame = data::SelaSubFrame((uint8_t)child, (uint8_t)0, (uint8_t)child,
                candidate.reflectionData, candidate.residueData);
            selaFrame.subFrames.push_back(selaSubFrame);
        } else {
            data::SelaSubFrame selaSubFrame = data::SelaSubFrame((uint8_t)child, (uint8_t)1, parent,
                candidate.reflectionData, candidate.residueData);
            selaFrame.subFrames.push_back(selaSubFrame);
        }
    }
    return selaFrame;
}

}
