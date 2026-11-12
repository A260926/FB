// Zero phase processing on short segments.
//
// Usage example: why a recording must not be cut into arbitrary little
// pieces when a zero phase (forward / backward) pass is wanted.
//
// A plain bidirectional pass which starts each pass from zero state
// produces a different transient at the head and at the tail, because it
// has no history there.  Doing the same thing piece by piece makes it
// worse: every cut introduces a new pair of edges, and the joins carry
// the strongest artefacts.  Iir::ZeroPhasePlan solves for the forward and
// backward initial states of one fixed segment at once, so both ends are
// consistent and the whole segment is kept, sample for sample.

#include "Iir.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace
{

const double Pi = 3.14159265358979323846;

// A recording-like signal: two tones plus a slow drift and a click near
// the start.  Real short recordings are rarely stationary.
std::vector<double> makeRecording(std::size_t n)
{
    std::vector<double> x(n);
    std::mt19937 rng(2024);
    std::uniform_real_distribution<double> noise(-0.02, 0.02);

    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i);
        x[i] = 0.55 * std::sin(2.0 * Pi * 0.043 * t) +
               0.30 * std::sin(2.0 * Pi * 0.131 * t + 0.7) +
               0.10 * std::sin(2.0 * Pi * 0.006 * t) + noise(rng);
    }
    x[3] += 0.8;
    x[4] -= 0.5;
    return x;
}

// A one way SOS cascade with the same coefficients in both directions,
// built directly on the library type so that the comparison is honest.
bool initialise(Iir::Custom::SOSCascade<2>& cascade,
                const double (*sos)[6])
{
    const double local[2][6] = {
        { sos[0][0], sos[0][1], sos[0][2], sos[0][3], sos[0][4], sos[0][5] },
        { sos[1][0], sos[1][1], sos[1][2], sos[1][3], sos[1][4], sos[1][5] }
    };
    cascade.setup(local);
    return true;
}

// Zero state bidirectional pass over just this piece: first forward from
// zero state, then backward from zero state.  This is what a naive
// implementation does, with unequal transients at both ends.
std::vector<double> naiveBidirectional(Iir::Custom::SOSCascade<2>& cascade,
                                       const std::vector<double>& x)
{
    std::vector<double> forward(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
        forward[i] = cascade.filter(x[i]);

    cascade.reset();
    std::vector<double> backward(x.size());
    for (std::size_t i = x.size(); i-- > 0; )
        backward[i] = cascade.filter(forward[i]);

    return backward;
}

// Concatenate naive results of fixed little blocks.  Returns the result
// and the index of the largest sample-to-sample jump across a block edge.
std::vector<double> blockwiseNaive(Iir::Custom::SOSCascade<2>& cascade,
                                   const std::vector<double>& x,
                                   std::size_t block,
                                   double& worstEdge,
                                   std::size_t& worstIndex,
                                   std::size_t& edgeCount)
{
    const std::size_t blocks = (x.size() + block - 1) / block;
    std::vector<double> out(x.size(), 0.0);
    worstEdge = 0.0;
    worstIndex = 0;
    edgeCount = 0;

    for (std::size_t b = 0; b < blocks; ++b)
    {
        const std::size_t begin = b * block;
        const std::size_t end = std::min(block, x.size() - begin);
        std::vector<double> piece(x.begin() + static_cast<long>(begin),
                                  x.begin() + static_cast<long>(begin + end));
        const std::vector<double> filtered = naiveBidirectional(cascade, piece);

        for (std::size_t i = 0; i < end; ++i)
            out[begin + i] = filtered[i];

        if (b > 0)
        {
            ++edgeCount;
            const double jump = std::abs(out[begin] - out[begin - 1]);
            if (jump > worstEdge)
            {
                worstEdge = jump;
                worstIndex = begin;
            }
        }
    }

    return out;
}

double largestStep(const std::vector<double>& x)
{
    double worst = 0.0;
    for (std::size_t i = 1; i < x.size(); ++i)
        worst = std::max(worst, std::abs(x[i] - x[i - 1]));
    return worst;
}

}

int main(int, char**)
{
    // Two sections from the library demo coefficients: a gentle low pass
    // followed by a mild resonator.
    const double sos[2][6] = {
        { 0.2, 0.4, 0.2, 1.0, -0.7, 0.2 },
        { 0.3, -0.1, 0.05, 2.0, -0.9, 0.18 }
    };

    Iir::Custom::SOSCascade<2> cascade;
    initialise(cascade, sos);

    const std::size_t n = 120;
    const std::vector<double> recording = makeRecording(n);

    // 1) One plan for the whole segment.  The two boundary states are
    //    solved together; the segment is kept in its entirety.
    Iir::ZeroPhasePlan whole(cascade.getCascadeStorage(), 2, n, 1);
    std::vector<double> wholeOut(n);
    const double* wholeInput[1] = { recording.data() };
    double* wholeOutput[1] = { wholeOut.data() };
    whole.process(wholeInput, wholeOutput, 1);

    // 2) The naive zero state bidirectional pass on the same whole
    //    segment: also continuous, but the two ends behave differently.
    const std::vector<double> naiveWhole = naiveBidirectional(cascade, recording);

    // 3) The same naive pass applied block by block.
    const std::size_t block = 16;
    double worstEdge = 0.0;
    std::size_t worstIndex = 0;
    std::size_t edgeCount = 0;
    const std::vector<double> blockwise = blockwiseNaive(cascade, recording, block,
                                                         worstEdge, worstIndex, edgeCount);

    // 4) One plan per fixed block: the states are solved per block, so
    //    every block has its own consistent boundary pair.
    std::vector<double> fixedBlocks(n, 0.0);
    for (std::size_t b = 0; b * block < n; ++b)
    {
        const std::size_t begin = b * block;
        const std::size_t end = std::min(block, n - begin);
        Iir::ZeroPhasePlan piece(cascade.getCascadeStorage(), 2, end, 1);
        std::vector<double> pieceIn(recording.begin() + static_cast<long>(begin),
                                    recording.begin() + static_cast<long>(begin + end));
        std::vector<double> pieceOut(end);
        const double* pieceInput[1] = { pieceIn.data() };
        double* pieceOutput[1] = { pieceOut.data() };
        piece.process(pieceInput, pieceOutput, 1);
        for (std::size_t i = 0; i < end; ++i)
            fixedBlocks[begin + i] = pieceOut[i];
    }

    const double wholeStep = largestStep(wholeOut);
    const double blockStep = largestStep(blockwise);
    const double fixedStep = largestStep(fixedBlocks);
    const double naiveStep = largestStep(naiveWhole);
    const double inputStep = largestStep(recording);

    // The single plan follows the input as smoothly as the one way
    // cascade does; the naive blockwise result adds a step at every cut.
    std::printf("segment samples=%zu block=%zu edges=%zu\n", n, block, edgeCount);
    std::printf("largest sample step: input=%.5f plan whole=%.5f "
                "naive whole=%.5f naive blocks=%.5f plan blocks=%.5f\n",
                inputStep, wholeStep, naiveStep, blockStep, fixedStep);
    std::printf("largest naive block-edge jump=%.5f at sample %zu "
                "(cut every %zu samples)\n",
                worstEdge, worstIndex, block);

    if (blockStep <= wholeStep)
    {
        std::fprintf(stderr, "Unexpected: blockwise processing is smoother.\n");
        return 1;
    }

    // Both ends compared: with one plan both sides carry a small
    // transient of their own; a zero state pass leaves one side cut.
    std::printf("edge amplitudes: plan first=%.2e last=%.2e "
                "naive first=%.2e last=%.2e\n",
                std::abs(wholeOut.front()), std::abs(wholeOut.back()),
                std::abs(naiveWhole.front()), std::abs(naiveWhole.back()));

    return 0;
}
