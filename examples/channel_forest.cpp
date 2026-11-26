// Example: encode a 16-bit PCM WAV file with the optimal channel forest and
// optionally decode the resulting SeLa file back to WAV.
//
// Build after running task_support/build.sh:
//   c++ -std=c++11 -O2 -pthread -I. \
//     examples/channel_forest.cpp .task-build/libsela.a \
//     -o .task-build/channel_forest_example
//
// Run:
//   .task-build/channel_forest_example input.wav output.sela decoded.wav

#include "src/include/data/exception.hpp"
#include "src/include/file/sela_file.hpp"
#include "src/include/file/wav_file.hpp"
#include "src/include/sela/decoder.hpp"
#include "src/include/sela/encoder.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: " << argv[0] << " input.wav output.sela [decoded.wav]\n";
        return 2;
    }

    try {
        std::ifstream wavInput(argv[1], std::ios::binary);
        if (!wavInput) {
            std::cerr << "Cannot open input WAV: " << argv[1] << '\n';
            return 2;
        }

        // The second argument defaults to false in both APIs.  Pass true to
        // compare every independent and cross-channel differential candidate
        // and encode the minimum-cost acyclic channel forest.
        sela::Encoder encoder(wavInput, true);
        file::SelaFile selaFile = encoder.process();

        {
            std::ofstream selaOutput(argv[2], std::ios::binary | std::ios::trunc);
            if (!selaOutput) {
                std::cerr << "Cannot open output SeLa file: " << argv[2] << '\n';
                return 2;
            }
            selaFile.writeToFile(selaOutput);
        }

        if (argc == 4) {
            std::ifstream selaInput(argv[2], std::ios::binary);
            if (!selaInput) {
                std::cerr << "Cannot reopen SeLa file: " << argv[2] << '\n';
                return 2;
            }

            // Decoding reconstructs arbitrary legal parent layers.  Structural
            // dependency errors are reported as data::Exception.
            sela::Decoder decoder(selaInput);
            file::WavFile decodedWav = decoder.process();

            std::ofstream wavOutput(argv[3], std::ios::binary | std::ios::trunc);
            if (!wavOutput) {
                std::cerr << "Cannot open output WAV: " << argv[3] << '\n';
                return 2;
            }
            decodedWav.writeToFile(wavOutput);
        }
    } catch (const data::Exception& error) {
        std::cerr << "SELA error: " << error.exceptionMessage << '\n';
        return 1;
    }

    return 0;
}
