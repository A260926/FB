#include "../include/sela/decoder.hpp"
#include "../include/data/exception.hpp"
#include "../include/frame.hpp"

#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace sela {

class LoopThrough {
public:
    const size_t begin;
    const size_t end;
    const std::vector<data::SelaFrame>& selaFrames;
    std::mutex& errorMutex;
    std::vector<std::exception_ptr>& errors;
    LoopThrough(size_t begin, size_t end, const std::vector<data::SelaFrame>& selaFrames,
        std::mutex& errorMutex, std::vector<std::exception_ptr>& errors)
        : begin(begin)
        , end(end)
        , selaFrames(selaFrames)
        , errorMutex(errorMutex)
        , errors(errors)
    {
    }
    void process(std::vector<data::WavFrame>& wavFrames)
    {
        try {
            // Empty ranges are normal when there are fewer frames than
            // worker threads; such a worker simply contributes nothing.
            if (begin >= end || end > selaFrames.size()) {
                return;
            }
            wavFrames.reserve(end - begin);

            for (size_t i = begin; i < end; i++) {
                frame::FrameDecoder decoder = frame::FrameDecoder(selaFrames[i]);
                data::WavFrame wavFrame = decoder.process();
                wavFrames.push_back(wavFrame);
            }
        } catch (...) {
            // Hand the failure back to the joining thread instead of
            // terminating the whole process inside this worker.
            std::lock_guard<std::mutex> lock(errorMutex);
            errors.push_back(std::current_exception());
        }
    }
};

void Decoder::readFrames()
{
    selaFile.readFromFile(ifStream);
}

void Decoder::processFrames(std::vector<data::WavFrame>& decodedWavFrames)
{
    // Get number of threads supported by hardware
    size_t numThreads = std::thread::hardware_concurrency();
    decodedWavFrames.reserve(selaFile.selaFrames.size());

    std::mutex errorMutex;
    std::vector<std::exception_ptr> errors;

    if (numThreads > 0) {
        //Initialize ThreadPool
        std::vector<std::thread> threadPool;
        threadPool.reserve(numThreads);

        //Initialize output wavFrames
        std::vector<std::vector<data::WavFrame>> wavFrameSegments;
        wavFrameSegments.reserve(numThreads);
        for (size_t i = 0; i < numThreads; i++) {
            wavFrameSegments.push_back(std::vector<data::WavFrame>());
        }

        const size_t framesPerThread = selaFile.selaFrames.size() / numThreads;

        size_t begin = 0;
        size_t end = framesPerThread;
        for (size_t i = 0; i < numThreads; i++) {
            //Create worker object
            LoopThrough data = LoopThrough(begin, end, selaFile.selaFrames, errorMutex, errors);

            //Start thread object and push thread to threadpool
            threadPool.push_back(std::thread(&LoopThrough::process, data, std::ref(wavFrameSegments[i])));

            //Increment beginning and end
            begin += framesPerThread;
            end = ((i == numThreads - 2) ? (selaFile.selaFrames.size()) : (end + framesPerThread));
        }

        for (size_t i = 0; i < numThreads; i++) {
            // Wait for worker threads to finish execution
            threadPool[i].join();
        }

        if (!errors.empty()) {
            std::rethrow_exception(errors[0]);
        }

        for (std::vector<data::WavFrame> segment : wavFrameSegments) {
            for (data::WavFrame wavFrame : segment) {
                decodedWavFrames.push_back(wavFrame);
            }
        }
    } else { //Single loop execution
        for (data::SelaFrame selaFrame : selaFile.selaFrames) {
            frame::FrameDecoder decoder = frame::FrameDecoder(selaFrame);
            data::WavFrame wavFrame = decoder.process();
            decodedWavFrames.push_back(wavFrame);
        }
    }
}

file::WavFile Decoder::process()
{
    std::vector<data::WavFrame> wavFrames;
    readFrames();
    processFrames(wavFrames);
    return file::WavFile(selaFile.selaHeader.sampleRate, selaFile.selaHeader.bitsPerSample, selaFile.selaHeader.channels, std::move(wavFrames));
}
}
