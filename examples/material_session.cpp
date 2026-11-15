// MaterialSession example: open a session, render one frame through the ticket
// API, commit it, and write the accumulated linear-RGB image as a binary PPM.
//
// Usage: material_session <output.ppm>

#include "MaterialSession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace materialsession;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]);
        return 1;
    }

    SessionConfig config; // default 32x24, enkiTS default worker count
    MaterialSession session(config);

    // Begin -> compute into the ticket's private buffer -> commit exactly once.
    FrameTicket* ticket = session.BeginFrame();
    if (!ticket)
        return 2;
    if (session.ComputeTicket(ticket) != TicketStatus::Computed)
    {
        delete ticket;
        return 3;
    }
    if (session.CommitTicket(ticket) != CommitResult::Ok)
    {
        delete ticket;
        return 4;
    }

    const int width = session.Width();
    const int height = session.Height();
    std::vector<float> rgb((size_t)width * height * 3);
    const int frames = session.ReadAccumulation(rgb.data(), rgb.size());
    delete ticket;

    std::FILE* file = std::fopen(argv[1], "wb");
    if (!file)
    {
        std::fprintf(stderr, "cannot open %s for writing\n", argv[1]);
        return 5;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);

    // Clamp HDR linear values and apply a gamma 1/2.2 view transform for PPM.
    std::vector<unsigned char> bytes(rgb.size());
    for (size_t i = 0; i < rgb.size(); ++i)
    {
        float v = std::max(0.0f, std::min(1.0f, rgb[i]));
        bytes[i] = static_cast<unsigned char>(std::pow(v, 1.0f / 2.2f) * 255.0f + 0.5f);
    }
    std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);

    std::printf("MATERIAL_SESSION_PPM path=%s width=%d height=%d frames=%d\n",
                argv[1], width, height, frames);
    return 0;
}
