#include "../include/sela/encoder.hpp"
#include "../include/data/exception.hpp"
#include "../include/frame.hpp"

#include <exception>
#include <thread>

namespace sela {

class LoopThrough {
public:
    const size_t begin;
    const size_t end;
    const std::vector<data::WavFrame>& wavFrames;
    const bool channelForest;
    LoopThrough(size_t begin, size_t end, const std::vector<data::WavFrame>& wavFrames,
        bool channelForest = false)
        : begin(begin)
        , end(end)
        , wavFrames(wavFrames)
        , channelForest(channelForest)
    {
    }
    void process(std::vector<data::SelaFrame>& selaFrames)
    {
        if (begin > end || end > wavFrames.size()) {
            throw data::Exception("Something is wrong");
        }
        selaFrames.reserve(end - begin);

        for (size_t i = begin; i < end; i++) {
            frame::FrameEncoder encoder = frame::FrameEncoder(wavFrames[i], channelForest);
            data::SelaFrame selaFrame = encoder.process();
            selaFrames.push_back(selaFrame);
        }
    }
    void processAndCapture(std::vector<data::SelaFrame>& selaFrames, std::exception_ptr& error)
    {
        try {
            process(selaFrames);
        } catch (...) {
            error = std::current_exception();
        }
    }
};

void Encoder::readFrames()
{
    wavFile.readFromFile(ifStream);
}

void Encoder::processFrames(std::vector<data::SelaFrame>& encodedSelaFrames)
{
    // Get number of threads supported by hardware
    size_t numThreads = std::thread::hardware_concurrency();
    encodedSelaFrames.reserve(wavFile.wavChunk.dataSubChunk.wavFrames.size());

    if (numThreads > 0) {
        //Initialize ThreadPool
        std::vector<std::thread> threadPool;
        threadPool.reserve(numThreads);

        //Initialize output wavFrames
        std::vector<std::vector<data::SelaFrame>> selaFrameSegments;
        std::vector<std::exception_ptr> workerErrors;
        selaFrameSegments.reserve(numThreads);
        workerErrors.reserve(numThreads);
        for (size_t i = 0; i < numThreads; i++) {
            selaFrameSegments.push_back(std::vector<data::SelaFrame>());
            workerErrors.push_back(std::exception_ptr());
        }

        const size_t framesPerThread = wavFile.wavChunk.dataSubChunk.wavFrames.size() / numThreads;

        size_t begin = 0;
        size_t end = framesPerThread;

        for (size_t i = 0; i < numThreads; i++) {
            //Create worker object
            LoopThrough data = LoopThrough(begin, end,
                wavFile.wavChunk.dataSubChunk.wavFrames, channelForest);

            //Start thread object and push thread to threadpool
            threadPool.push_back(std::thread(&LoopThrough::processAndCapture, data,
                std::ref(selaFrameSegments[i]), std::ref(workerErrors[i])));

            //Increment beginning and end
            begin += framesPerThread;
            end = ((i == numThreads - 2) ? (wavFile.wavChunk.dataSubChunk.wavFrames.size()) : (end + framesPerThread));
        }

        for (size_t i = 0; i < numThreads; i++) {
            // Wait for worker threads to finish execution
            threadPool[i].join();
        }

        for (size_t i = 0; i < numThreads; i++) {
            if (workerErrors[i]) {
                std::rethrow_exception(workerErrors[i]);
            }
        }

        for (std::vector<data::SelaFrame> segment : selaFrameSegments) {
            for (data::SelaFrame selaFrame : segment) {
                encodedSelaFrames.push_back(selaFrame);
            }
        }
    } else {
        for (data::WavFrame wavFrame : wavFile.wavChunk.dataSubChunk.wavFrames) {
            frame::FrameEncoder encoder = frame::FrameEncoder(wavFrame, channelForest);
            data::SelaFrame selaFrame = encoder.process();
            encodedSelaFrames.push_back(selaFrame);
        }
    }
}

file::SelaFile Encoder::process()
{
    std::vector<data::SelaFrame> selaFrames;
    readFrames();
    processFrames(selaFrames);
    return file::SelaFile(wavFile.wavChunk.formatSubChunk.sampleRate, wavFile.wavChunk.formatSubChunk.bitsPerSample, (uint8_t)wavFile.wavChunk.formatSubChunk.numChannels, std::move(selaFrames));
}
}