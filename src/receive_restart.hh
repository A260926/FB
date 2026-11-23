#pragma once

#include "uvgrtp/util.hh"
#include "uvgrtp/frame.hh"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace uvgrtp {

    class rtcp_internal;
    class reception_flow;

    namespace formats {
        class media;
    }

    /* Result of comparing one parsed RTP packet against the active receive generation. */
    enum class receive_restart_result {
        feature_disabled,      /* feature is off: caller must use the old path */
        forward,               /* normal forward, duplicate or bounded late packet */
        candidate,             /* packet stored as a restart candidate, not delivered */
        candidate_duplicate,   /* duplicate of the stored candidate, dropped */
        confirmed              /* candidate confirmed by this packet */
    };

    /* Per-stream receive generation state for a sender that restarts its RTP
     * sequence numbering while keeping the same SSRC.
     *
     * classify() decides what a packet is. On confirmation the generation switch
     * itself is driven by switch_generation(), which invokes the callbacks installed
     * with set_callbacks(): the media layer drops old fragments and pending output,
     * the RTCP layer re-bases its receiver statistics and the reception flow drops
     * frames that were queued but not yet handed to the application.
     */
    class receive_restart {
        public:
            receive_restart();
            ~receive_restart();

            receive_restart(const receive_restart&) = delete;
            receive_restart& operator=(const receive_restart&) = delete;

            /* Configure the feature. Only plaintext H.264/H.265 reception is supported.
             *
             * Enabling is only allowed before the first valid RTP packet has been seen.
             * Disabling is always allowed and releases a stored candidate.
             *
             * Returns RTP_OK on success and RTP_INVALID_VALUE (state unchanged) when the
             * format/flags are unsupported or when enabling after the first packet. */
            rtp_error_t configure(bool enable, rtp_format_t fmt, int rce_flags);

            bool enabled() const { return enabled_; }

            /* True once any valid RTP packet has been classified, even while disabled. */
            bool started() const { return started_; }

            bool has_candidate() const { return candidate_ != nullptr; }

            /* Classify a fully parsed (and decrypted, when applicable) RTP frame.
             *
             * Ownership:
             *   candidate            - receive_restart takes ownership of "frame"
             *   candidate_duplicate  - caller keeps ownership and should free it
             *   forward/confirmed    - caller keeps ownership                          */
            receive_restart_result classify(uvgrtp::frame::rtp_frame* frame,
                std::chrono::steady_clock::time_point arrival = std::chrono::steady_clock::now());

            /* Take the stored candidate. The caller becomes its owner. */
            uvgrtp::frame::rtp_frame* take_candidate();

            uint16_t candidate_seq() const { return candidate_seq_; }

            /* Release the stored candidate (called when the feature is disabled). */
            void clear_candidate();

            /* Invoke the generation switch callbacks. The stored candidate is passed to
             * the RTCP callback so it can re-base the receiver statistics. Does not release
             * the candidate; call take_candidate() afterwards. */
            void switch_generation(uint32_t ssrc);

            void set_callbacks(std::function<void(const uvgrtp::frame::rtp_frame*)> reset_reports,
                std::function<void()> reset_media,
                std::function<void(uint32_t)> discard_pending);

        private:
            bool enabled_ = false;
            bool started_ = false;

            /* Active highest sequence number of the current generation. */
            uint16_t highest_ = 0;

            /* One fully parsed packet waiting for its successor. */
            uvgrtp::frame::rtp_frame* candidate_ = nullptr;
            uint16_t candidate_seq_ = 0;

            std::function<void(const uvgrtp::frame::rtp_frame*)> reset_reports_;
            std::function<void()> reset_media_;
            std::function<void(uint32_t)> discard_pending_;
    };

    /* ---------------------------------------------------------------------
     * In-memory receive pipeline used by the regression tests and the example.
     *
     * It wires the production RTP parser, reception flow, media handler and RTCP
     * statistics/report builder together without a socket or a receive thread.
     * feed() calls the same per-datagram entry point that the production
     * reception thread calls.
     * --------------------------------------------------------------------- */
    class receive_restart_pipeline {
        public:
            receive_restart_pipeline();
            ~receive_restart_pipeline();

            receive_restart_pipeline(const receive_restart_pipeline&) = delete;
            receive_restart_pipeline& operator=(const receive_restart_pipeline&) = delete;

            /* Add one stream to the shared reception flow. Returns the stream index. */
            int add_stream(rtp_format_t fmt, uint32_t local_ssrc, uint32_t remote_ssrc, int rce_flags);

            rtp_error_t configure_receive_restart(size_t index, bool enable);

            /* Deliver one datagram through the production receive path. */
            rtp_error_t feed(const uint8_t* data, size_t size,
                std::chrono::steady_clock::time_point arrival = std::chrono::steady_clock::now());

            /* Non-blocking pull of a frame delivered to a stream without a hook. */
            uvgrtp::frame::rtp_frame* pull(size_t index);

            rtp_error_t install_receive_hook(size_t index, void* arg,
                void (*hook)(void*, uvgrtp::frame::rtp_frame*));

            /* Build an RTCP report through the production report builder and copy the
             * compound packet bytes to the caller. */
            rtp_error_t build_report(size_t index, std::unique_ptr<uint8_t[]>& bytes, size_t& size);

            receive_restart& restart(size_t index);
            rtcp_internal* rtcp(size_t index);
            formats::media* media(size_t index);

            uint32_t local_ssrc(size_t index) const;
            uint32_t remote_ssrc(size_t index) const;
            rtp_format_t format(size_t index) const;
            int rce_flags(size_t index) const;

        private:
            struct impl;
            std::unique_ptr<impl> impl_;
    };
}
