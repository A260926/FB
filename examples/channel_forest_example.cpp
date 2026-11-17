// Channel forest usage example for the SELA codec.
//
// Build against the production static library, for example:
//   c++ -std=c++11 -O2 -pthread -I. examples/channel_forest_example.cpp \
//       .task-build/libsela.a -o channel_forest_example
//
// The second constructor argument switches channel forest coding on. It
// defaults to false, so existing call sites keep their exact old behavior.

#include "src/include/data/wav_frame.hpp"
#include "src/include/file/sela_file.hpp"
#include "src/include/file/wav_file.hpp"
#include "src/include/frame.hpp"
#include "src/include/sela/decoder.hpp"
#include "src/include/sela/encoder.hpp"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
const size_t SAMPLES_PER_FRAME = 2048;

std::vector<std::vector<int32_t> > demoChannels()
{
    // Four correlated channels: each channel is a small offset of the
    // previous one, so difference coding across a multi-level forest wins.
    std::vector<std::vector<int32_t> > channels(4, std::vector<int32_t>(SAMPLES_PER_FRAME, 0));
    for (size_t i = 0; i < SAMPLES_PER_FRAME; i++) {
        int32_t base = (int32_t)(12000.0 * std::sin(i * 0.05));
        for (size_t channel = 0; channel < channels.size(); channel++) {
            channels[channel][i] = base + (int32_t)channel;
        }
    }
    return channels;
}

void frameLevelExample(const data::WavFrame& input)
{
    // bool channelForest = true: compare every independent root and every
    // parent-child difference candidate, then emit the globally cheapest
    // acyclic channel forest.
    data::SelaFrame encoded = frame::FrameEncoder(input, true).process();

    size_t roots = 0;
    for (const data::SelaSubFrame& subFrame : encoded.subFrames) {
        if (subFrame.subFrameType == 0) {
            ++roots;
            std::cout << "channel " << (int)subFrame.channel << " is an independent root\n";
        } else {
            std::cout << "channel " << (int)subFrame.channel << " depends on parent "
                      << (int)subFrame.parentChannelNumber << "\n";
        }
    }
    std::cout << "forest roots: " << roots << "\n";

    // The decoder validates the dependency graph and restores channels in
    // dependency order, regardless of how subframes are stored.
    data::WavFrame decoded = frame::FrameDecoder(encoded).process();
    std::cout << "frame roundtrip exact: " << (decoded.samples == input.samples ? "yes" : "no") << "\n";
}

void fileLevelExample(const data::WavFrame& input, const char* wavPath, const char* selaPath)
{
    std::vector<data::WavFrame> frames;
    frames.push_back(input);
    file::WavFile wavFile(44100, 16, (uint16_t)input.samples.size(), std::move(frames));
    {
        std::ofstream output(wavPath, std::ios::binary);
        wavFile.writeToFile(output);
    }

    {
        std::ifstream inputFile(wavPath, std::ios::binary);
        // The flag is forwarded to every encoder worker thread.
        file::SelaFile selaFile = sela::Encoder(inputFile, true).process();
        std::ofstream output(selaPath, std::ios::binary);
        selaFile.writeToFile(output);
    }

    {
        std::ifstream encodedFile(selaPath, std::ios::binary);
        // No flag is needed to decode: the subframe type and parent fields
        // describe the forest, and corrupt dependency data is reported back
        // to the caller as a data::Exception instead of killing a worker.
        file::WavFile restored = sela::Decoder(encodedFile).process();
        const data::WavFrame& decoded = restored.wavChunk.dataSubChunk.wavFrames[0];
        std::cout << "file roundtrip exact: " << (decoded.samples == input.samples ? "yes" : "no") << "\n";
    }
}
} // namespace

int main(int argc, char** argv)
{
    const char* wavPath = argc > 1 ? argv[1] : "channel_forest_demo.wav";
    const char* selaPath = argc > 2 ? argv[2] : "channel_forest_demo.sela";

    data::WavFrame input(16, demoChannels());
    frameLevelExample(input);
    fileLevelExample(input, wavPath, selaPath);
    return 0;
}
