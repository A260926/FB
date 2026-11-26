#ifndef _ENCODER_H_
#define _ENCODER_H_

#include "../data/sela_frame.hpp"
#include "../file/sela_file.hpp"
#include "../file/wav_file.hpp"

namespace sela {
class Encoder {
private:
    void readFrames();
    void processFrames(std::vector<data::SelaFrame>& encodedSelaFrames);
    std::ifstream& ifStream;
    file::WavFile wavFile;
    bool channelForest;

public:
    explicit Encoder(std::ifstream& ifStream, bool channelForest = false)
        : ifStream(ifStream)
        , wavFile()
        , channelForest(channelForest)
    {
    }
    file::SelaFile process();
};
}

#endif