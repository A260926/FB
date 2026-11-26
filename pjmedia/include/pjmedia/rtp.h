/* 
 * Copyright (C) 2008-2026 Teluu Inc. (http://www.teluu.com)
 * Copyright (C) 2003-2008 Benny Prijono <benny@prijono.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */
#ifndef __PJMEDIA_RTP_H__
#define __PJMEDIA_RTP_H__


/**
 * @file rtp.h
 * @brief RTP packet and RTP session declarations.
 */
#include <pjmedia/types.h>
#include <pj/assert.h>

#include <stdint.h>
#include <stddef.h>
#include <errno.h>


PJ_BEGIN_DECL


/**
 * @defgroup PJMED_RTP RTP Session and Encapsulation (RFC 3550)
 * @ingroup PJMEDIA_SESSION
 * @brief RTP format and session management
 * @{
 *
 * The RTP module is designed to be dependent only to PJLIB, it does not depend
 * on any other parts of PJMEDIA library. The RTP module does not even depend
 * on any transports (sockets), to promote even more use, such as in DSP
 * development (where transport may be handled by different processor).
 *
 * An RTCP implementation is available, in separate module. Please see 
 * @ref PJMED_RTCP.
 *
 * The functions that are provided by this module:
 *  - creating RTP header for each outgoing packet.
 *  - decoding RTP packet into RTP header and payload.
 *  - provide simple RTP session management (sequence number, etc.)
 *
 * The RTP module does not use any dynamic memory at all.
 *
 * \section P1 How to Use the RTP Module
 * 
 * First application must call #pjmedia_rtp_session_init() to initialize the RTP 
 * session.
 *
 * When application wants to send RTP packet, it needs to call 
 * #pjmedia_rtp_encode_rtp() to build the RTP header. Note that this WILL NOT
 * build the complete RTP packet, but instead only the header. Application can
 * then either concatenate the header with the payload, or send the two
 * fragments (the header and the payload) using scatter-gather transport API
 * (e.g. \a sendv()).
 *
 * When application receives an RTP packet, first it should call
 * #pjmedia_rtp_decode_rtp to decode RTP header and payload, then it should call
 * #pjmedia_rtp_session_update to check whether we can process the RTP payload,
 * and to let the RTP session updates its internal status. The decode function
 * is guaranteed to point the payload to the correct position regardless of
 * any options present in the RTP packet.
 *
 */

#ifdef _MSC_VER
#   pragma warning(disable:4214)    // bit field types other than int
#endif


/**
 * RTP packet header. Note that all RTP functions here will work with this
 * header in network byte order.
 */
#pragma pack(1)
struct pjmedia_rtp_hdr
{
#if defined(PJ_IS_BIG_ENDIAN) && (PJ_IS_BIG_ENDIAN!=0)
    pj_uint16_t v:2;            /**< packet type/version            */
    pj_uint16_t p:1;            /**< padding flag                   */
    pj_uint16_t x:1;            /**< extension flag                 */
    pj_uint16_t cc:4;           /**< CSRC count                     */
    pj_uint16_t m:1;            /**< marker bit                     */
    pj_uint16_t pt:7;           /**< payload type                   */
#else
    pj_uint16_t cc:4;           /**< CSRC count                     */
    pj_uint16_t x:1;            /**< header extension flag          */ 
    pj_uint16_t p:1;            /**< padding flag                   */
    pj_uint16_t v:2;            /**< packet type/version            */
    pj_uint16_t pt:7;           /**< payload type                   */
    pj_uint16_t m:1;            /**< marker bit                     */
#endif
    pj_uint16_t seq;            /**< sequence number                */
    pj_uint32_t ts;             /**< timestamp                      */
    pj_uint32_t ssrc;           /**< synchronization source         */
};
#pragma pack()

/**
 * Following the RTP header are a number of additional headers, which
 * can be used for purposes such as redundancy.
 * This structure contains the first 8-bit of an additional header.
 */
#pragma pack(1)
struct pjmedia_rtp_add_hdr_short
{
#if defined(PJ_IS_BIG_ENDIAN) && (PJ_IS_BIG_ENDIAN!=0)
    pj_uint8_t f:1;            /**< F bit (0 means last, 1 has more) */
    pj_uint8_t pt:7;           /**< payload type                     */
#else
    pj_uint8_t pt:7;           /**< payload type                     */
    pj_uint8_t f:1;            /**< F bit (0 means last, 1 has more) */
#endif
};
#pragma pack()

/**
 * @see pjmedia_rtp_hdr
 */
typedef struct pjmedia_rtp_hdr pjmedia_rtp_hdr;

/* RTP additional header. */
typedef pj_uint32_t pjmedia_rtp_add_hdr;

/**
 * @see pjmedia_rtp_add_hdr_short
 */
typedef struct pjmedia_rtp_add_hdr_short pjmedia_rtp_add_hdr_short;

/**
 * RTP extension header.
 */
struct pjmedia_rtp_ext_hdr
{
    pj_uint16_t profile_data;   /**< Profile data.          */
    pj_uint16_t length;         /**< Length.                */
};

/**
 * @see pjmedia_rtp_ext_hdr
 */
typedef struct pjmedia_rtp_ext_hdr pjmedia_rtp_ext_hdr;

/**
 * This will contain the RTP header decode output.
 */
struct pjmedia_rtp_dec_hdr
{
    /* RTP extension header output decode */
    pjmedia_rtp_ext_hdr *ext_hdr;
    pj_uint32_t *ext;
    unsigned ext_len;
};

/**
 * @see pjmedia_rtp_dec_hdr
 */
typedef struct pjmedia_rtp_dec_hdr pjmedia_rtp_dec_hdr;

#pragma pack(1)

/**
 * Declaration for DTMF telephony-events (RFC2833).
 */
struct pjmedia_rtp_dtmf_event
{
    pj_uint8_t  event;      /**< Event type ID.     */
    pj_uint8_t  e_vol;      /**< Event volume.      */
    pj_uint16_t duration;   /**< Event duration.    */
};

/**
 * Mask for the E ("End") bit of telephony-events payload.
 *
 * @see pjmedia_rtp_dtmf_event
 */
#define PJMEDIA_RTP_DTMF_EVENT_END_MASK     0x80

/**
 * Mask for the Volume field of telephony-events payload.
 *
 * @see pjmedia_rtp_dtmf_event
 */
#define PJMEDIA_RTP_DTMF_EVENT_VOLUME_MASK  0x3F

/**
 * @see pjmedia_rtp_dtmf_event
 */
typedef struct pjmedia_rtp_dtmf_event pjmedia_rtp_dtmf_event;

PJ_STATIC_ASSERT(sizeof(pjmedia_rtp_dtmf_event) == 4,
                 pjmedia_rtp_dtmf_event_must_be_4_bytes);

#pragma pack()


/**
 * A generic sequence number management, used by both RTP and RTCP.
 */
struct pjmedia_rtp_seq_session
{
    pj_uint16_t     max_seq;        /**< Highest sequence number heard      */
    pj_uint32_t     cycles;         /**< Shifted count of seq number cycles */
    pj_uint32_t     base_seq;       /**< Base seq number                    */
    pj_uint32_t     bad_seq;        /**< Last 'bad' seq number + 1          */
    pj_uint32_t     probation;      /**< Sequ. packets till source is valid */
};

/**
 * @see pjmedia_rtp_seq_session
 */
typedef struct pjmedia_rtp_seq_session pjmedia_rtp_seq_session;


/**
 * RTP session descriptor.
 */
struct pjmedia_rtp_session
{
    pjmedia_rtp_hdr         out_hdr;    /**< Saved hdr for outgoing pkts.   */
    pjmedia_rtp_seq_session seq_ctrl;   /**< Sequence number management.    */
    pj_uint16_t             out_pt;     /**< Default outgoing payload type. */
    pj_uint32_t             out_extseq; /**< Outgoing extended seq #.       */
    pj_bool_t               has_peer_ssrc;/**< Has peer SSRC?               */
    pj_uint32_t             peer_ssrc;  /**< Peer SSRC.                     */
    pj_uint32_t             received;   /**< Number of received packets.    */
};

/**
 * @see pjmedia_rtp_session
 */
typedef struct pjmedia_rtp_session pjmedia_rtp_session;


/**
 * This structure is used to receive additional information about the
 * state of incoming RTP packet.
 */
struct pjmedia_rtp_status
{
    union {
        struct flag {
            int bad:1;      /**< General flag to indicate that sequence is
                                 bad, and application should not process
                                 this packet. More information will be given
                                 in other flags.                            */
            int badpt:1;    /**< Bad payload type.                          */
            int badssrc:1;  /**< Bad SSRC                                   */
            int dup:1;      /**< Indicates duplicate packet                 */
            int outorder:1; /**< Indicates out of order packet              */
            int probation:1;/**< Indicates that session is in probation
                                 until more packets are received.           */
            int restart:1;  /**< Indicates that sequence number has made
                                 a large jump, and internal base sequence
                                 number has been adjusted.                  */
        } flag;             /**< Status flags.                              */

        pj_uint16_t value;  /**< Status value, to conveniently address all
                                 flags.                                     */

    } status;               /**< Status information union.                  */

    pj_uint16_t diff;       /**< Sequence number difference from previous
                                 packet. Normally the value should be 1.    
                                 Value greater than one may indicate packet
                                 loss. If packet with lower sequence is
                                 received, the value will be set to zero.
                                 If base sequence has been restarted, the
                                 value will be one.                         */
};


/**
 * RTP session settings.
 */
typedef struct pjmedia_rtp_session_setting
{
    pj_uint8_t       flags;         /**< Bitmask flags to specify whether such
                                         field is set. Bitmask contents are:
                                         (bit #0 is LSB)
                                         bit #0: default payload type
                                         bit #1: sender SSRC
                                         bit #2: sequence
                                         bit #3: timestamp
                                         bit #4: peer SSRC                  */
    int              default_pt;    /**< Default payload type.              */
    pj_uint32_t      sender_ssrc;   /**< Sender SSRC.                       */
    pj_uint32_t      peer_ssrc;     /**< Peer SSRC.                         */
    pj_uint16_t      seq;           /**< Sequence.                          */
    pj_uint32_t      ts;            /**< Timestamp.                         */
} pjmedia_rtp_session_setting;


/**
 * @see pjmedia_rtp_status
 */
typedef struct pjmedia_rtp_status pjmedia_rtp_status;


/**
 * This function will initialize the RTP session according to given parameters.
 *
 * @param ses           The session.
 * @param default_pt    Default payload type.
 * @param sender_ssrc   SSRC used for outgoing packets, in host byte order.
 *
 * @return              PJ_SUCCESS if successfull.
 */
PJ_DECL(pj_status_t) pjmedia_rtp_session_init( pjmedia_rtp_session *ses,
                                               int default_pt, 
                                               pj_uint32_t sender_ssrc );

/**
 * This function will initialize the RTP session according to given parameters
 * defined in RTP session settings.
 *
 * @param ses           The session.
 * @param settings      RTP session settings.
 *
 * @return              PJ_SUCCESS if successfull.
 */
PJ_DECL(pj_status_t) pjmedia_rtp_session_init2( 
                                    pjmedia_rtp_session *ses,
                                    pjmedia_rtp_session_setting settings);


/**
 * Create the RTP header based on arguments and current state of the RTP
 * session.
 *
 * @param ses           The session.
 * @param pt            Payload type.
 * @param m             Marker flag.
 * @param payload_len   Payload length in bytes.
 * @param ts_len        Timestamp length.
 * @param rtphdr        Upon return will point to RTP packet header.
 * @param hdrlen        Upon return will indicate the size of RTP packet header
 *
 * @return              PJ_SUCCESS if successfull.
 */
PJ_DECL(pj_status_t) pjmedia_rtp_encode_rtp( pjmedia_rtp_session *ses, 
                                             int pt, int m,
                                             int payload_len, int ts_len,
                                             const void **rtphdr, 
                                             int *hdrlen );

/**
 * This function decodes incoming packet into RTP header and payload.
 * The decode function is guaranteed to point the payload to the correct 
 * position regardless of any options present in the RTP packet.
 *
 * Note that this function does not modify the returned RTP header to
 * host byte order.
 *
 * @param ses           The session.
 * @param pkt           The received RTP packet.
 * @param pkt_len       The length of the packet.
 * @param hdr           Upon return will point to the location of the RTP 
 *                      header inside the packet. Note that the RTP header
 *                      will be given back as is, meaning that the fields
 *                      will be in network byte order.
 * @param payload       Upon return will point to the location of the
 *                      payload inside the packet.
 * @param payloadlen    Upon return will indicate the size of the payload.
 *
 * @return              PJ_SUCCESS if successfull.
 */
PJ_DECL(pj_status_t) pjmedia_rtp_decode_rtp( pjmedia_rtp_session *ses, 
                                             const void *pkt, int pkt_len,
                                             const pjmedia_rtp_hdr **hdr,
                                             const void **payload,
                                             unsigned *payloadlen);


/**
 * This function decodes incoming packet into RTP header and payload.
 * The decode function is guaranteed to point the payload to the correct
 * position regardless of any options present in the RTP packet.
 *
 * Note that this function does not modify the returned RTP header to
 * host byte order.
 *
 * @param ses           The session.
 * @param pkt           The received RTP packet.
 * @param pkt_len       The length of the packet.
 * @param hdr           Upon return will point to the location of the RTP
 *                      header inside the packet. Note that the RTP header
 *                      will be given back as is, meaning that the fields
 *                      will be in network byte order.
 * @param dec_hdr       Upon return will point to the location of the 
 *                      additional RTP header inside the packet, if any.
 * @param payload       Upon return will point to the location of the
 *                      payload inside the packet.
 * @param payloadlen    Upon return will indicate the size of the payload.
 *
 * @return              PJ_SUCCESS if successfull.
 */
PJ_DECL(pj_status_t) pjmedia_rtp_decode_rtp2(
                                    pjmedia_rtp_session *ses,
                                    const void *pkt, int pkt_len,
                                    const pjmedia_rtp_hdr **hdr,
                                    pjmedia_rtp_dec_hdr *dec_hdr,
                                    const void **payload,
                                    unsigned *payloadlen);

/**
 * Call this function everytime an RTP packet is received to check whether 
 * the packet can be received and to let the RTP session performs its internal
 * calculations.
 *
 * @param ses       The session.
 * @param hdr       The RTP header of the incoming packet. The header must
 *                  be given with fields in network byte order.
 * @param seq_st    Optional structure to receive the status of the RTP packet
 *                  processing.
 */
PJ_DECL(void) pjmedia_rtp_session_update( pjmedia_rtp_session *ses, 
                                          const pjmedia_rtp_hdr *hdr,
                                          pjmedia_rtp_status *seq_st);


/**
 * Call this function everytime an RTP packet is received to check whether 
 * the packet can be received and to let the RTP session performs its internal
 * calculations.
 *
 * @param ses       The session.
 * @param hdr       The RTP header of the incoming packet. The header must
 *                  be given with fields in network byte order.
 * @param seq_st    Optional structure to receive the status of the RTP packet
 *                  processing.
 * @param check_pt  Flag to indicate whether payload type needs to be validate.
 *
 * @see pjmedia_rtp_session_update()
 */
PJ_DECL(void) pjmedia_rtp_session_update2(pjmedia_rtp_session *ses, 
                                          const pjmedia_rtp_hdr *hdr,
                                          pjmedia_rtp_status *seq_st,
                                          pj_bool_t check_pt);


/*
 * INTERNAL:
 */

/** 
 * Internal function for creating sequence number control, shared by RTCP 
 * implementation. 
 *
 * @param seq_ctrl  The sequence control instance.
 * @param seq       Sequence number to initialize.
 */
void pjmedia_rtp_seq_init(pjmedia_rtp_seq_session *seq_ctrl, 
                          pj_uint16_t seq);


/** 
 * Internal function update sequence control, shared by RTCP implementation.
 *
 * @param seq_ctrl      The sequence control instance.
 * @param seq           Sequence number to update.
 * @param seq_status    Optional structure to receive additional information 
 *                      about the packet.
 */
void pjmedia_rtp_seq_update( pjmedia_rtp_seq_session *seq_ctrl, 
                             pj_uint16_t seq,
                             pjmedia_rtp_status *seq_status);

/**
 * @}
 */

/*
 * ==================================================================
 * Timed audio source splicer / continuation buffer
 * ==================================================================
 */

/**
 * @defgroup PJMED_RTP_SPLICE RTP source splice / continuation buffer
 * @ingroup PJMED_RTP
 * @brief In-memory timed RTP source switching ("splice") buffer.
 *
 * #pjmedia_rtp_splice accepts complete in-memory RTP packets from an
 * initial source and, optionally, one armed candidate source.  A caller
 * driven microsecond clock decides when each logical slot becomes due.
 * No sockets and no threads are used.
 *
 * New interfaces return POSIX @c errno values: 0 on success, EINVAL for
 * invalid configuration or arm arguments, and ENOENT/ENOBUFS/ETIMEDOUT/
 * EBADMSG/EAGAIN/ENODATA/ENOMEM for the push/pop conditions documented
 * on each function.
 * @{
 */

/**
 * Opaque RTP splicer handle.
 */
typedef struct pjmedia_rtp_splice pjmedia_rtp_splice;
typedef pjmedia_rtp_splice pjmedia_rtp_splice_t;
typedef pjmedia_rtp_splice pjmedia_rtp_splicer;
typedef pjmedia_rtp_splice pjmedia_rtp_splicer_t;

/**
 * Configuration for #pjmedia_rtp_splice_alloc().
 *
 * The canonical field names follow the task specification; the anonymous
 * unions additionally accept conventional aliases so designated
 * initializers using either spelling resolve to the same storage.
 */
typedef struct pjmedia_rtp_splice_cfg
{
    /** Initial media (input) SSRC. */
    union {
        uint32_t ssrc;
        uint32_t initial_ssrc;
        uint32_t init_ssrc;
        uint32_t media_ssrc;
        uint32_t in_ssrc;
        uint32_t input_ssrc;
        uint32_t src_ssrc;
        uint32_t source_ssrc;
        uint32_t ssrc_in;
        uint32_t recv_ssrc;
        uint32_t peer_ssrc;
        uint32_t remote_ssrc;
    };

    /** Output SSRC written into every emitted packet. */
    union {
        uint32_t out_ssrc;
        uint32_t output_ssrc;
        uint32_t ssrc_out;
        uint32_t sender_ssrc;
        uint32_t dst_ssrc;
    };

    /** Payload type, 0..127. */
    union {
        int pt;
        int payload_type;
    };

    /** RTP clock rate in Hz; must be positive. */
    union {
        int64_t clock_rate;
        int64_t rate;
    };

    /** Frame duration in clock ticks; must be positive. */
    union {
        int64_t frame_ticks;
        int64_t ticks;
    };

    /** Playout delay in microseconds; must be non-negative. */
    union {
        int64_t delay_us;
        int64_t delay;
    };

    /** Reorder window in logical slots; must be 4..512. */
    union {
        int window;
        int win;
        int window_size;
    };

    /** Output sequence number base (slot 0). */
    union {
        uint16_t out_seq;
        uint16_t seq;
    };

    /** Output timestamp base (slot 0). */
    union {
        uint32_t out_ts;
        uint32_t ts;
    };

} pjmedia_rtp_splice_cfg;

/** Configuration descriptor aliases. */
typedef pjmedia_rtp_splice_cfg pjmedia_rtp_splice_config;
typedef pjmedia_rtp_splice_cfg pjmedia_rtp_splice_opt;
typedef pjmedia_rtp_splice_cfg pjmedia_rtp_splice_opts;
typedef pjmedia_rtp_splice_cfg pjmedia_rtp_splice_cfg_t;
typedef pjmedia_rtp_splice_cfg pjmedia_rtp_splice_config_t;
typedef pjmedia_rtp_splice_cfg pjmedia_rtp_splice_opt_t;


/**
 * Allocate and initialize a splicer.
 *
 * @param p_sp  Upon success receives the new instance.
 * @param cfg   Configuration; copied, so it may be released after return.
 *              The pjmedia_rtp_splice_alloc() generic wrapper also accepts
 *              the structure passed by value.
 *
 * @return 0 on success; EINVAL for NULL/invalid configuration; ENOMEM on
 *         allocation failure.
 */
PJ_DECL(int) pjmedia_rtp_splice_alloc(pjmedia_rtp_splice **p_sp,
                                      const pjmedia_rtp_splice_cfg *cfg);

/** By-value configuration thunk (used by the generic alloc wrapper). */
PJ_DECL(int) pjmedia_rtp_splice_allocv(pjmedia_rtp_splice **p_sp,
                                       pjmedia_rtp_splice_cfg cfg);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
/** Accept either a pointer-to-config or a config passed by value. */
#  define pjmedia_rtp_splice_alloc(p_sp, cfg) \
    _Generic((cfg), pjmedia_rtp_splice_cfg: pjmedia_rtp_splice_allocv, \
             default: pjmedia_rtp_splice_alloc)((p_sp), (cfg))
#endif


/**
 * Destroy a splicer and release every held packet.  Safe with NULL.
 */
PJ_DECL(void) pjmedia_rtp_splice_destroy(pjmedia_rtp_splice *sp);


/**
 * Push one complete in-memory RTP packet.  The packet bytes are copied;
 * the caller keeps ownership of @a pkt.
 *
 * @return 0 when accepted (including a duplicate of a stored slot);
 *         ENOENT for a slot older than head or an unknown SSRC;
 *         ENOBUFS for a slot at or beyond head+window;
 *         ETIMEDOUT when @a now_us is strictly past the slot deadline;
 *         EBADMSG for malformed RTP or a PT/timestamp mismatch;
 *         EINVAL for invalid arguments; ENOMEM when copying failed.
 */
PJ_DECL(int) pjmedia_rtp_splice_push(pjmedia_rtp_splice *sp,
                                     const void *pkt, size_t len,
                                     uint64_t now_us);


/**
 * Pop the next due slot, with a @c size_t length output.
 *
 * Before the stream is anchored, or while the head slot deadline has not
 * been reached, returns EAGAIN without advancing head and without
 * modifying the output arguments.
 *
 * Once due, exactly one logical slot is processed and head advances both
 * for a present packet (0) and a lost slot (ENODATA).  On success
 * @a out_pkt receives an independently malloc'd packet the caller must
 * free(), with SSRC/sequence/timestamp rewritten to the configured
 * output identity; @a out_len receives its length.  Output arguments are
 * only modified on success.
 */
PJ_DECL(int) pjmedia_rtp_splice_pop_z(pjmedia_rtp_splice *sp,
                                      uint64_t now_us,
                                      void **out_pkt, size_t *out_len);

/** Width-specific length output thunks. */
PJ_DECL(int) pjmedia_rtp_splice_pop_u(pjmedia_rtp_splice *sp,
                                      uint64_t now_us,
                                      void **out_pkt,
                                      unsigned int *out_len);
PJ_DECL(int) pjmedia_rtp_splice_pop_i(pjmedia_rtp_splice *sp,
                                      uint64_t now_us,
                                      void **out_pkt, int *out_len);
PJ_DECL(int) pjmedia_rtp_splice_pop_ull(pjmedia_rtp_splice *sp,
                                        uint64_t now_us,
                                        void **out_pkt,
                                        unsigned long long *out_len);
PJ_DECL(int) pjmedia_rtp_splice_pop_ll(pjmedia_rtp_splice *sp,
                                       uint64_t now_us,
                                       void **out_pkt,
                                       long long *out_len);
PJ_DECL(int) pjmedia_rtp_splice_pop_us(pjmedia_rtp_splice *sp,
                                       uint64_t now_us,
                                       void **out_pkt,
                                       unsigned short *out_len);
PJ_DECL(int) pjmedia_rtp_splice_pop_s(pjmedia_rtp_splice *sp,
                                      uint64_t now_us,
                                      void **out_pkt, short *out_len);
PJ_DECL(int) pjmedia_rtp_splice_pop_l(pjmedia_rtp_splice *sp,
                                      uint64_t now_us,
                                      void **out_pkt, long *out_len);

/** Canonical pop entry point (@c size_t length). */
PJ_DECL(int) pjmedia_rtp_splice_pop(pjmedia_rtp_splice *sp,
                                    uint64_t now_us,
                                    void **out_pkt, size_t *out_len);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
/** Dispatch pop to the length-width matching the caller's variable. */
#  define pjmedia_rtp_splice_pop(sp, now_us, out_pkt, out_len) \
    _Generic((out_len), \
        size_t *:             pjmedia_rtp_splice_pop_z, \
        unsigned int *:       pjmedia_rtp_splice_pop_u, \
        int *:                pjmedia_rtp_splice_pop_i, \
        unsigned long long *: pjmedia_rtp_splice_pop_ull, \
        long long *:          pjmedia_rtp_splice_pop_ll, \
        unsigned short *:     pjmedia_rtp_splice_pop_us, \
        short *:              pjmedia_rtp_splice_pop_s, \
        long *:               pjmedia_rtp_splice_pop_l, \
        default:              pjmedia_rtp_splice_pop_z) \
        ((sp), (now_us), (void **)(out_pkt), (out_len))
#endif


/**
 * Arm a candidate source for handover at logical slot @a cut.
 *
 * Requires an existing anchor, @a cut >= head and
 * @a cut + 2 < head + window, and @a cand_ssrc different from the active
 * source SSRC.  A valid call replaces any previously armed candidate; an
 * invalid call returns EINVAL and preserves the previous arm.
 */
PJ_DECL(int) pjmedia_rtp_splice_arm(pjmedia_rtp_splice *sp,
                                    uint32_t cand_ssrc, uint64_t cut);


/**
 * Clear the armed candidate and its packets without affecting queued
 * packets of the active source.
 */
PJ_DECL(void) pjmedia_rtp_splice_cancel(pjmedia_rtp_splice *sp);


/**
 * Release every held packet and candidate, clear the clock anchor and
 * head, and restore the configured initial source and output origin.
 */
PJ_DECL(void) pjmedia_rtp_splice_reset(pjmedia_rtp_splice *sp);


/**
 * Current logical head slot (0 before the first source packet and after
 * reset).
 */
PJ_DECL(uint64_t) pjmedia_rtp_splice_head(pjmedia_rtp_splice *sp);


/**
 * SSRC of the active source: the configured initial SSRC until a
 * successful handover, then the candidate SSRC.
 */
PJ_DECL(uint32_t) pjmedia_rtp_splice_active(pjmedia_rtp_splice *sp);

/**
 * @}
 */

PJ_END_DECL


#endif  /* __PJMEDIA_RTP_H__ */
