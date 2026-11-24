/*
 * SRT - Secure, Reliable, Transport
 *
 * This example shows how to use srt_schedule_rcv_latency() to smoothly change
 * the TSBPD receive latency of a connected live/message receiver without
 * breaking the connection. The change is purely local: the wire protocol is
 * untouched and the peer keeps its own sending pace.
 *
 * The call is valid only for a connected, TSBPD-enabled receiver socket
 * (neither groups nor file/stream nor sender-only sockets). It schedules a
 * piecewise-linear ramp of the local buffering latency:
 *
 *   target_ms : target latency in milliseconds, range 0..60000.
 *   ramp_ms   : transition duration in milliseconds, range 1..60000. When the
 *               latency is increased, ramp_ms must be strictly greater than
 *               the latency difference.
 *
 * Re-calling while a ramp is still running continues seamlessly from the
 * latency currently in effect. The example below deliberately avoids opening
 * a real connection (it needs no network); it exercises the validation path on
 * an invalid descriptor and documents the exact calls used on a live socket.
 */

#include <cstdio>

#include "srt.h"

int main()
{
    if (srt_startup() != 0)
    {
        std::fprintf(stderr, "srt_startup failed\n");
        return 1;
    }

    int rc = 0;

    // An invalid socket is rejected without touching any state.
    if (srt_schedule_rcv_latency(SRT_INVALID_SOCK, 200, 1000) != SRT_ERROR)
    {
        std::fprintf(stderr, "expected SRT_ERROR for an invalid socket\n");
        rc = 1;
    }

    // Parameter ranges are validated as well.
    if (srt_schedule_rcv_latency(SRT_INVALID_SOCK, -1, 1000) != SRT_ERROR)
    {
        std::fprintf(stderr, "expected SRT_ERROR for a negative target\n");
        rc = 1;
    }
    if (srt_schedule_rcv_latency(SRT_INVALID_SOCK, 200, 0) != SRT_ERROR)
    {
        std::fprintf(stderr, "expected SRT_ERROR for a zero ramp\n");
        rc = 1;
    }

    std::printf("srt_schedule_rcv_latency validation checks passed\n");

    /*
     * On a real connected live receiver `u`, the typical usage is:
     *
     *   // Grow the receive latency from its current value to 300 ms over 2 s.
     *   // The 2000 ms ramp is strictly longer than the latency increase.
     *   if (srt_schedule_rcv_latency(u, 300, 2000) != 0)
     *       handle_error(srt_getlasterror(NULL));
     *
     *   // Later, shrink it back to 120 ms over 500 ms. The transition starts
     *   // from the latency in effect at call time, so it stays continuous.
     *   if (srt_schedule_rcv_latency(u, 120, 500) != 0)
     *       handle_error(srt_getlasterror(NULL));
     *
     * Packets already queued are re-scheduled in place: their payload,
     * sequence and message numbers never change, blocked recv calls are woken
     * to re-evaluate their deadlines, and epoll read events follow the new
     * readiness. The negotiated latency, srctime and statistics keep their
     * original meaning until the first successful call.
     */

    srt_cleanup();
    return rc;
}
